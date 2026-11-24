#include "roce_app.h"
#include "roce_cm_dpdk.h"
#include "roce_doorbell.h"
#include "roce_lab.h"
#include "../../mrc-dpdk/src/dpdk_io.h"

#include <arpa/inet.h>
#include <dlfcn.h>

#include <rte_dev.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_memory.h>
#include <rte_pause.h>

#include <inttypes.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    RX_BURST = 32, MAX_TX_BURST = 32, NS_PER_SEC = 1000000000,
    RETRY_NS = 10000000, CONTROL_POLL_NS = 1000000,
    MBUF_CACHE_SIZE = 256,
};

struct datapath_options {
    uint32_t send_window;
    uint32_t queue_window;
    uint32_t tx_burst;
    uint32_t writes;
    uint32_t qps;
    uint32_t workers;
    uint32_t start_gate;
    uint32_t app_post_batch;
    unsigned int lcores[ROCE_LAB_MAX_QPS];
    const char *app_socket;
    const char *mr_socket;
    struct roce_address cm_peer;
    bool report_bandwidth;
    bool scatter_gather;
    bool native_queues;
};

struct tx_memory_ref {
    struct rte_mbuf_ext_shared_info shared;
} __rte_cache_aligned;

typedef int (*mlx5_dmabuf_map_fn)(uint16_t, void *, size_t, int, uint64_t);
typedef int (*mlx5_dmabuf_unmap_fn)(uint16_t, void *, size_t);

union write_qp {
    struct roce_requester requester;
    struct roce_responder responder;
};

enum run_state { WRITE_WAITING, WRITE_RUNNING, WRITE_STOPPED };

struct write_run {
    const struct datapath_options *options;
    struct rte_mempool *pool;
    struct roce_app_work *work;
    struct roce_native_work *native;
    uint64_t local_iova;
    uint32_t local_lkey;
    struct tx_memory_ref *tx_refs;
    uint16_t port;
    int application_socket;
    bool sender;
    atomic_int state;
    atomic_uint finished_workers;
    atomic_bool failed;
};

struct write_worker {
    struct write_run *run;
    uint16_t queue;
    uint32_t base_qpn;
    uint32_t first_qp;
    uint32_t qps;
    uint32_t completed;
    uint32_t next;
    uint32_t doorbell_cursor;
    uint32_t fully_posted;
    uint32_t posted[ROCE_LAB_MAX_QPS];
    uint32_t completions[ROCE_LAB_MAX_QPS];
    bool finished;
    uint64_t cpu_started;
    uint64_t cpu_ns;
    uint64_t finished_ns;
    union write_qp states[ROCE_LAB_MAX_QPS];
} __rte_cache_aligned;

static struct rte_mbuf *encode_packet(struct rte_mempool *pool, const struct roce_packet *packet)
{
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(pool);
    if (!mbuf)
        return NULL;
    size_t length = roce_encode(rte_pktmbuf_mtod(mbuf, uint8_t *),
                               rte_pktmbuf_tailroom(mbuf), packet);
    if (!length || !rte_pktmbuf_append(mbuf, (uint16_t)length)) {
        rte_pktmbuf_free(mbuf);
        return NULL;
    }
    return mbuf;
}

static void release_tx_reference(void *address, void *opaque)
{
    /* The transport retains a reference until every TX queue has stopped. */
    (void)address;
    (void)opaque;
}

static struct rte_mbuf *encode_gathered_packet(struct rte_mempool *pool,
                                               struct rte_mbuf_ext_shared_info *shared,
                                               const struct roce_packet *packet)
{
    struct rte_mbuf *segments[3];
    if (rte_pktmbuf_alloc_bulk(pool, segments, RTE_DIM(segments)) != 0)
        return NULL;
    struct rte_mbuf *header = segments[0], *payload = segments[1], *trailer = segments[2];
    uint16_t header_len, trailer_len;
    if (!roce_encode_split(rte_pktmbuf_mtod(header, uint8_t *),
                           rte_pktmbuf_mtod(trailer, uint8_t *), packet, &header_len, &trailer_len)) {
        for (size_t i = 0; i < RTE_DIM(segments); ++i)
            rte_pktmbuf_free(segments[i]);
        return NULL;
    }
    uint16_t payload_len = (uint16_t)packet->payload_length;
    rte_mbuf_ext_refcnt_update(shared, 1);
    rte_pktmbuf_attach_extbuf(payload, (void *)packet->payload, (rte_iova_t)(uintptr_t)packet->payload,
                             payload_len, shared);
    payload->data_len = payload_len;
    payload->pkt_len = payload_len;
    trailer->data_len = trailer_len;
    trailer->pkt_len = trailer_len;
    header->data_len = header_len;
    header->pkt_len = header_len + payload_len + trailer_len;
    header->nb_segs = RTE_DIM(segments);
    header->next = payload;
    payload->next = trailer;
    return header;
}

static bool send_requests(const struct write_worker *worker,
                          struct roce_requester *requester, uint32_t posted_packets,
                          uint32_t credit, uint64_t now)
{
    const struct write_run *run = worker->run;
    uint32_t budget = RTE_MIN(posted_packets - requester->next,
                              credit + (requester->sent - requester->next));
    uint32_t limit = RTE_MIN(budget, run->options->tx_burst);
    struct rte_mbuf *packets[MAX_TX_BURST];
    struct roce_packet packet;
    uint16_t count = 0;
    while (count < limit && roce_requester_next(requester, count, &packet)) {
        /* A full queue window must still generate an ACK before another send. */
        if (run->options->queue_window && count + 1u == budget)
            packet.ack_requested = true;
        if (run->tx_refs && packet.payload_length)
            packets[count] = encode_gathered_packet(run->pool, &run->tx_refs[worker->queue].shared, &packet);
        else
            packets[count] = encode_packet(run->pool, &packet);
        if (!packets[count])
            break;
        ++count;
    }
    if (!count)
        return false;
    uint16_t sent = rte_eth_tx_burst(run->port, worker->queue, packets, count);
    roce_requester_sent(requester, sent, now);
    /* A short burst leaves the unsent PSNs eligible for the next poll. */
    for (uint16_t i = sent; i < count; ++i)
        rte_pktmbuf_free(packets[i]);
    return true;
}

static uint64_t thread_cpu_ns(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
}

static void report_bandwidth(bool sender, uint64_t bytes, uint64_t started, uint64_t elapsed, uint64_t cpu,
                             const struct datapath_options *options)
{
    if (!elapsed)
        return;
    printf("BANDWIDTH role=%s payload_bytes=%" PRIu64 " elapsed_ns=%" PRIu64
           " thread_cpu_ns=%" PRIu64 " payload_gbps=%.3f poll_cores=%.3f writes=%u qps=%u workers=%u"
           " start_ns=%" PRIu64,
           sender ? "tx" : "rx", bytes, elapsed, cpu,
           (double)bytes * 8.0 / (double)elapsed, (double)cpu / (double)elapsed,
           options->writes, options->qps, options->workers, started);
    if (sender)
        printf(" send_window=%u tx_burst=%u", options->send_window, options->tx_burst);
    if (sender && options->queue_window)
        printf(" queue_window=%u", options->queue_window);
    putchar('\n');
}

static uint16_t qp_source_port(uint32_t qpn)
{
    /* Consecutive QPNs have distinct UDP source ports within a benchmark. */
    return (uint16_t)(0xc000 | (qpn & 0x3fff));
}

static bool init_qps(struct write_worker *worker, uint32_t first, uint16_t pmtu,
                     const struct roce_lab_offer *local, const struct roce_lab_offer *peer,
                     uint8_t *buffer)
{
    const struct write_run *run = worker->run;
    const struct datapath_options *options = run->options;
    uint64_t qp_bytes = local->region.size * options->writes;
    for (uint32_t i = 0; i < worker->qps; ++i) {
        uint32_t index = first + i;
        struct roce_qp qp = {
            .local = local->data, .peer = peer->data, .pmtu = pmtu,
            .local_qpn = local->qpn + index, .peer_qpn = peer->qpn + index,
            .udp_source = options->workers > 1 ? qp_source_port(local->qpn + index) : 0,
        };
        uint64_t offset = qp_bytes * index;
        if (run->sender) {
            struct roce_region remote = peer->region;
            remote.address += offset;
            if (!roce_requester_init(&worker->states[i].requester, &qp,
                                     roce_native_psn(local->psn, index),
                                     remote, buffer + offset, ROCE_LAB_RETRIES,
                                     options->send_window, options->writes))
                return false;
        } else {
            worker->states[i].responder = (struct roce_responder) {
                .qp = qp, .buffer = buffer + offset,
                .expected = roce_native_psn(peer->psn, index),
                .region = { .address = local->region.address + offset,
                            .rkey = local->region.rkey, .size = qp_bytes },
            };
        }
    }
    return true;
}

static struct rte_flow *steer_qp(uint16_t port, uint16_t queue,
                                const struct roce_lab_offer *local,
                                const struct roce_lab_offer *peer, uint32_t index)
{
    const struct rte_flow_attr attributes = { .ingress = 1 };
    const struct rte_flow_item_ipv4 ip = { .hdr = {
        .src_addr = rte_cpu_to_be_32(peer->data.ipv4),
        .dst_addr = rte_cpu_to_be_32(local->data.ipv4),
    } };
    const struct rte_flow_item_udp udp = { .hdr = {
        .src_port = rte_cpu_to_be_16(qp_source_port(peer->qpn + index)),
        .dst_port = rte_cpu_to_be_16(ROCE_UDP_PORT),
    } };
    const struct rte_flow_item pattern[] = {
        { .type = RTE_FLOW_ITEM_TYPE_ETH },
        { .type = RTE_FLOW_ITEM_TYPE_IPV4, .spec = &ip, .mask = &rte_flow_item_ipv4_mask },
        { .type = RTE_FLOW_ITEM_TYPE_UDP, .spec = &udp, .mask = &rte_flow_item_udp_mask },
        { .type = RTE_FLOW_ITEM_TYPE_END },
    };
    const struct rte_flow_action_queue target = { .index = queue };
    const struct rte_flow_action actions[] = {
        { .type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &target },
        { .type = RTE_FLOW_ACTION_TYPE_END },
    };
    struct rte_flow_error error = {0};
    struct rte_flow *flow = rte_flow_create(port, &attributes, pattern, actions, &error);
    if (!flow)
        fprintf(stderr, "QP %06x RX steering to queue %u failed: %s\n",
                local->qpn + index, queue, error.message ? error.message : "unknown flow error");
    return flow;
}

static bool receive_packets(struct write_worker *worker, uint64_t now)
{
    const struct write_run *run = worker->run;
    uint32_t writes = run->options->writes;
    struct rte_mbuf *received[RX_BURST];
    uint16_t count = rte_eth_rx_burst(run->port, worker->queue, received, RX_BURST);
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t storage[ROCE_MAX_FRAME];
        uint32_t length = rte_pktmbuf_pkt_len(received[i]);
        const void *frame = length <= sizeof(storage) ?
            rte_pktmbuf_read(received[i], 0, length, storage) : NULL;
        struct roce_packet packet;
        if (frame && roce_decode(&packet, frame, length)) {
            uint32_t index = packet.qpn - worker->base_qpn;
            if (index < worker->qps) {
                if (run->sender) {
                    struct roce_requester *requester = &worker->states[index].requester;
                    bool pending = requester->acked < requester->total;
                    roce_requester_feedback(requester, &packet, now);
                    worker->completed += pending && requester->acked == requester->total;
                } else {
                    struct roce_responder *responder = &worker->states[index].responder;
                    bool pending = responder->msn < writes;
                    struct roce_packet feedback;
                    if (roce_responder_receive(responder, &packet, &feedback)) {
                        struct rte_mbuf *ack = encode_packet(run->pool, &feedback);
                        if (ack && rte_eth_tx_burst(run->port, worker->queue, &ack, 1) == 0)
                            rte_pktmbuf_free(ack);
                    }
                    if (responder->failed) {
                        for (; i < count; ++i)
                            rte_pktmbuf_free(received[i]);
                        fprintf(stderr, "QP %06x WRITE failed responder validation at PSN %06x\n",
                                responder->qp.local_qpn, responder->expected);
                        return false;
                    }
                    worker->completed += pending && responder->msn == writes;
                }
            }
        }
        rte_pktmbuf_free(received[i]);
    }
    return true;
}

static bool admit_writes(struct write_worker *worker)
{
    const struct write_run *run = worker->run;
    if (!run->work || worker->fully_posted == worker->qps)
        return true;
    struct roce_doorbell *doorbell = &run->work->doorbells[worker->queue];
    for (unsigned i = 0; i < ROCE_DOORBELL_SLOTS; ++i) {
        uint64_t item = roce_doorbell_take(doorbell, &worker->doorbell_cursor);
        if (!item)
            break;
        uint32_t qp = (uint32_t)(item >> 32);
        uint32_t count = (uint32_t)item;
        uint32_t index = qp - worker->first_qp - 1;
        if (index >= worker->qps || !count || count > run->options->writes - worker->posted[index]) {
            fprintf(stderr, "invalid application work for worker %u\n", worker->queue);
            return false;
        }
        worker->posted[index] += count;
        worker->fully_posted += worker->posted[index] == run->options->writes;
        atomic_store_explicit(&run->work->qps[qp - 1].admitted, worker->posted[index], memory_order_release);
    }
    return true;
}

static enum roce_native_wc_status validate_native_write(const struct write_worker *worker, uint32_t index,
                                                        uint32_t sequence, struct roce_native_write wqe)
{
    const struct write_run *run = worker->run;
    const struct roce_requester *requester = &worker->states[index].requester;
    uint64_t bytes = requester->remote.size;
    uint64_t ordinal = (uint64_t)(worker->first_qp + index) * run->options->writes + sequence;
    if (wqe.length != bytes)
        return ROCE_NATIVE_WC_LOCAL_LENGTH;
    if (wqe.lkey != run->local_lkey || wqe.local_address != run->local_iova + ordinal * bytes)
        return ROCE_NATIVE_WC_LOCAL_PROTECTION;
    if (wqe.rkey != requester->remote.rkey || wqe.remote_address != requester->remote.address + sequence * bytes)
        return ROCE_NATIVE_WC_REMOTE_ACCESS;
    return ROCE_NATIVE_WC_SUCCESS;
}

static bool admit_native_qp(struct write_worker *worker, uint32_t index)
{
    const struct write_run *run = worker->run;
    struct roce_native_sq *sq = &run->native->qps[worker->first_qp + index];
    uint32_t head = atomic_load_explicit(&sq->posted, memory_order_acquire);
    uint32_t sequence = worker->posted[index];
    if (head == sequence)
        return true;
    if (head > run->options->writes || head - sequence > ROCE_NATIVE_QUEUE_DEPTH ||
        head - worker->completions[index] > ROCE_NATIVE_QUEUE_DEPTH)
        return false;
    for (; sequence < head; ++sequence) {
        uint32_t slot = sequence % ROCE_NATIVE_QUEUE_DEPTH;
        enum roce_native_wc_status result = validate_native_write(worker, index, sequence, sq->entries[slot]);
        if (result != ROCE_NATIVE_WC_SUCCESS) {
            sq->status[slot] = result;
            return false;
        }
    }
    worker->posted[index] = head;
    worker->fully_posted += head == run->options->writes;
    atomic_store_explicit(&sq->admitted, head, memory_order_release);
    return true;
}

static bool admit_native_writes(struct write_worker *worker)
{
    const struct write_run *run = worker->run;
    if (!run->native || worker->fully_posted == worker->qps)
        return true;
    struct roce_doorbell *doorbell = &run->native->doorbells[worker->queue];
    for (unsigned i = 0; i < ROCE_DOORBELL_SLOTS; ++i) {
        uint64_t item = roce_doorbell_take(doorbell, &worker->doorbell_cursor);
        if (!item)
            break;
        uint32_t index = (uint32_t)(item >> 32) - worker->first_qp - 1;
        if (index >= worker->qps || !admit_native_qp(worker, index))
            return false;
    }
    /* A full doorbell ring does not hide already-published SQ entries. */
    for (uint32_t i = 0; i < worker->qps; ++i)
        if (!admit_native_qp(worker, i))
            return false;
    return true;
}

static void publish_completions(struct write_worker *worker)
{
    const struct write_run *run = worker->run;
    if (!run->work && !run->native)
        return;
    bool drained = !run->tx_refs || rte_mbuf_ext_refcnt_read(&run->tx_refs[worker->queue].shared) == 1;
    if (!run->native && !drained)
        return;
    for (uint32_t i = 0; i < worker->qps; ++i) {
        const struct roce_requester *requester = &worker->states[i].requester;
        uint32_t completed = requester->acked / requester->packets_per_write;
        if (run->native) {
            struct roce_native_sq *sq = &run->native->qps[worker->first_qp + i];
            atomic_store_explicit(&sq->acknowledged, completed, memory_order_relaxed);
            if (!drained || completed == worker->completions[i])
                continue;
            for (uint32_t seq = worker->completions[i]; seq < completed; ++seq)
                sq->status[seq % ROCE_NATIVE_QUEUE_DEPTH] = ROCE_NATIVE_WC_SUCCESS;
            atomic_store_explicit(&sq->retired, completed, memory_order_release);
        } else if (completed != worker->completions[i]) {
            atomic_store_explicit(&run->work->qps[worker->first_qp + i].completed, completed,
                                  memory_order_release);
        }
        worker->completions[i] = completed;
    }
}

static bool poll_worker(struct write_worker *worker, uint64_t now)
{
    struct write_run *run = worker->run;
    if (run->sender && worker->finished)
        return true;
    if (!admit_writes(worker) || !admit_native_writes(worker)) {
        fprintf(stderr, "application work rejected on worker %u\n", worker->queue);
        return false;
    }
    if (!receive_packets(worker, now))
        return false;
    publish_completions(worker);
    if (worker->completed == worker->qps && !worker->finished) {
        worker->finished = true;
        if (run->options->report_bandwidth)
            worker->finished_ns = roce_now_ns();
        atomic_fetch_add_explicit(&run->finished_workers, 1, memory_order_relaxed);
    }
    if (!run->sender || worker->finished)
        return true;

    uint32_t outstanding = 0;
    for (uint32_t i = 0; i < worker->qps; ++i) {
        struct roce_requester *requester = &worker->states[i].requester;
        roce_requester_poll_timer(requester, now, RETRY_NS);
        if (requester->failed) {
            fprintf(stderr, "QP %06x WRITE failed after %u retries (%u/%u packets acknowledged)\n",
                    requester->qp.local_qpn, requester->retries, requester->acked, requester->total);
            return false;
        }
        outstanding += requester->sent - requester->acked;
    }
    uint32_t window = run->options->queue_window;
    uint32_t credit = window ? window - RTE_MIN(window, outstanding) : run->options->tx_burst;
    for (uint32_t i = 0; i < worker->qps; ++i) {
        uint32_t next = worker->next;
        struct roce_requester *requester = &worker->states[next].requester;
        uint32_t posted_packets = run->work || run->native ?
            worker->posted[next] * requester->packets_per_write : requester->total;
        if (++worker->next == worker->qps)
            worker->next = 0;
        if (send_requests(worker, requester, posted_packets, credit, now))
            break;
    }
    return true;
}

static bool all_workers_finished(const struct write_run *run)
{
    return atomic_load_explicit(&run->finished_workers, memory_order_relaxed) == run->options->workers;
}

static void sample_cpu(struct write_worker *worker)
{
    if (worker->run->options->report_bandwidth && !worker->cpu_ns)
        worker->cpu_ns = thread_cpu_ns() - worker->cpu_started;
}

static int run_worker(void *arg)
{
    struct write_worker *worker = arg;
    struct write_run *run = worker->run;
    int state;
    while ((state = atomic_load_explicit(&run->state, memory_order_acquire)) == WRITE_WAITING)
        rte_pause();
    if (state != WRITE_RUNNING)
        return 0;

    worker->cpu_started = run->options->report_bandwidth ? thread_cpu_ns() : 0;
    while (atomic_load_explicit(&run->state, memory_order_relaxed) == WRITE_RUNNING) {
        if (!poll_worker(worker, roce_now_ns())) {
            atomic_store_explicit(&run->failed, true, memory_order_relaxed);
            return 1;
        }
        /* RX continues polling after placement in case a final ACK needs replay. */
        if (run->sender && worker->finished)
            break;
        if (worker->finished && all_workers_finished(run))
            sample_cpu(worker);
        rte_pause();
    }
    sample_cpu(worker);
    return 0;
}

static bool wait_for_completion(int fd, struct write_worker *main_worker, uint64_t started)
{
    const struct write_run *run = main_worker->run;
    uint64_t control_poll = started;
    uint64_t deadline = started + (uint64_t)ROCE_LAB_TIMEOUT * NS_PER_SEC;
    for (uint64_t now = started; now < deadline; now = roce_now_ns()) {
        if (!poll_worker(main_worker, now) ||
            atomic_load_explicit(&run->failed, memory_order_relaxed))
            return false;
        if (now >= control_poll) {
            control_poll = now + CONTROL_POLL_NS;
            if (run->application_socket >= 0) {
                struct pollfd peer = { .fd = run->application_socket };
                if (poll(&peer, 1, 0) > 0 && peer.revents & (POLLERR | POLLHUP | POLLNVAL))
                    return false;
            }
            if (!run->sender) {
                int done = roce_lab_receive(fd, 'D', true);
                if (done)
                    return done == 1;
            }
        }
        if (run->sender) {
            if (all_workers_finished(run))
                return true;
        } else {
            if (main_worker->finished && all_workers_finished(run))
                sample_cpu(main_worker);
        }
        rte_pause();
    }
    return false;
}

static bool verify_result(int fd, int application, const struct write_worker *workers, uint64_t started,
                          const uint8_t *buffer, uint64_t bytes, uint64_t qp_packets)
{
    const struct write_run *run = workers[0].run;
    const struct datapath_options *options = run->options;
    uint64_t finished = started, cpu = 0, retries = 0;
    for (uint32_t i = 0; i < options->workers; ++i) {
        const struct write_worker *worker = &workers[i];
        if (worker->finished_ns > finished)
            finished = worker->finished_ns;
        cpu += worker->cpu_ns;
        for (uint32_t j = 0; j < worker->qps; ++j) {
            if (run->sender) {
                retries += worker->states[j].requester.retries;
            } else {
                const struct roce_responder *responder = &worker->states[j].responder;
                if (responder->msn != options->writes || responder->remaining ||
                    responder->accepted != qp_packets) {
                    fprintf(stderr, "QP %06x ended with %u WRITEs and %" PRIu64 " packets\n",
                            responder->qp.local_qpn, responder->msn, responder->accepted);
                    return false;
                }
            }
        }
    }

    uint64_t packets = qp_packets * options->qps;
    if (run->sender) {
        if (!roce_lab_send(fd, 'D') || roce_lab_receive(fd, 'V', false) != 1)
            return false;
        printf("WRITE complete: %" PRIu64 " bytes, %" PRIu64 " packets, %" PRIu64
               " retries, peer verified\n", bytes, packets, retries);
    } else {
        bool verified = application < 0 ? roce_lab_verify(buffer, (size_t)bytes) :
            roce_lab_send(application, 'D') && roce_lab_receive(application, 'V', false) == 1;
        if (!verified || !roce_lab_send(fd, 'V'))
            return false;
        printf("WRITE verified: %" PRIu64 " bytes, %" PRIu64 " packets\n", bytes, packets);
    }
    if (options->report_bandwidth)
        report_bandwidth(run->sender, bytes, started, finished - started, cpu, options);
    return true;
}

static int run(int fd, uint16_t port, struct rte_mempool *pool,
               const struct roce_lab_offer *local, const struct roce_lab_offer *peer,
               const struct roce_lab_options *lab, uint8_t *buffer,
               const struct roce_app_memory *application, bool steer,
               const struct datapath_options *options, struct tx_memory_ref *tx_refs)
{
    struct write_run run = { .options = options, .pool = pool, .port = port, .sender = lab->sender,
                             .state = WRITE_WAITING, .work = application->work,
                             .native = application->native, .local_iova = local->region.address,
                             .local_lkey = application->mr.lkey,
                             .application_socket = application->socket,
                             .tx_refs = tx_refs };
    struct write_worker workers[ROCE_LAB_MAX_QPS] = {0};
    struct rte_flow *flows[ROCE_LAB_MAX_QPS] = {0};
    uint16_t pmtu = local->pmtu < peer->pmtu ? local->pmtu : peer->pmtu;
    int result = 1;
    uint32_t launched = 0;
    uint64_t started = 0;
    for (uint16_t queue = 0; queue < options->workers; ++queue) {
        uint32_t first = queue * options->qps / options->workers;
        uint32_t end = (queue + 1) * options->qps / options->workers;
        struct write_worker *worker = &workers[queue];
        worker->run = &run;
        worker->queue = queue;
        worker->base_qpn = local->qpn + first;
        worker->first_qp = first;
        worker->qps = end - first;
        if (!init_qps(worker, first, pmtu, local, peer, buffer)) {
            fprintf(stderr, "WRITE sequence exceeds the requester range\n");
            goto cleanup_flows;
        }
        if (steer) {
            for (uint32_t i = first; i < end; ++i) {
                flows[i] = steer_qp(port, queue, local, peer, i);
                if (!flows[i])
                    goto cleanup_flows;
            }
        }
    }
    for (uint32_t i = 1; i < options->workers; ++i) {
        if (rte_eal_remote_launch(run_worker, &workers[i], options->lcores[i]) != 0) {
            fprintf(stderr, "could not launch queue %u on lcore %u\n", i, options->lcores[i]);
            goto stop_workers;
        }
        ++launched;
    }

    bool completed = false;
    bool ready = options->start_gate ?
        roce_lab_ready_after_gate(fd, (uint16_t)options->start_gate, lab->control_port) :
        roce_lab_ready(fd);
    if (ready) {
        started = roce_now_ns();
        workers[0].cpu_started = options->report_bandwidth ? thread_cpu_ns() : 0;
        atomic_store_explicit(&run.state, WRITE_RUNNING, memory_order_release);
        completed = wait_for_completion(fd, &workers[0], started);
        sample_cpu(&workers[0]);
    } else {
        fprintf(stderr, "control readiness exchange%s failed\n", options->start_gate ? " or start gate" : "");
    }
    result = !completed;

stop_workers:
    atomic_store_explicit(&run.state, WRITE_STOPPED, memory_order_release);
    for (uint32_t i = 1; i <= launched; ++i)
        if (rte_eal_wait_lcore(options->lcores[i]) != 0)
            result = 1;

    if (!result) {
        uint64_t bytes = local->region.size * options->writes * options->qps;
        uint64_t qp_packets = ((local->region.size - 1) / pmtu + 1) * options->writes;
        result = !verify_result(fd, application->socket, workers, started, buffer, bytes, qp_packets);
    }
    if (result)
        fprintf(stderr, "WRITE or verification exchange did not complete\n");

cleanup_flows:
    for (uint32_t i = 0; i < options->qps; ++i)
        if (flows[i])
            rte_flow_destroy(port, flows[i], NULL);
    return result;
}

static bool parse_datapath_options(int argc, char **argv, struct datapath_options *options)
{
    *options = (struct datapath_options) {
        .send_window = ROCE_SEND_WINDOW_DEFAULT,
        .tx_burst = MAX_TX_BURST,
        .writes = 1,
        .qps = 1,
        .workers = 1,
    };
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--report-bw") == 0) {
            options->report_bandwidth = true;
        } else if (strcmp(argv[i], "--scatter-gather") == 0) {
            options->scatter_gather = true;
        } else if (strcmp(argv[i], "--verbs-queues") == 0) {
            options->native_queues = true;
        } else if (strcmp(argv[i], "--cm-peer") == 0) {
            struct in_addr ip;
            struct rte_ether_addr mac;
            if (argc - i <= 2 || options->cm_peer.ipv4 ||
                inet_pton(AF_INET, argv[i + 1], &ip) != 1 || !ip.s_addr ||
                rte_ether_unformat_addr(argv[i + 2], &mac) || !rte_is_valid_assigned_ether_addr(&mac))
                return false;
            options->cm_peer.ipv4 = ntohl(ip.s_addr);
            memcpy(options->cm_peer.mac, mac.addr_bytes, ROCE_MAC_LEN);
            i += 2;
        } else if (strcmp(argv[i], "--tx-burst") == 0) {
            if (++i == argc || !roce_parse_uint(argv[i], MAX_TX_BURST, &options->tx_burst) ||
                !options->tx_burst)
                return false;
        } else if (strcmp(argv[i], "--send-window") == 0) {
            if (++i == argc ||
                !roce_parse_uint(argv[i], ROCE_SEND_WINDOW_MAX, &options->send_window) ||
                !options->send_window)
                return false;
        } else if (strcmp(argv[i], "--queue-window") == 0) {
            if (++i == argc ||
                !roce_parse_uint(argv[i], ROCE_LAB_MAX_QPS * ROCE_SEND_WINDOW_MAX, &options->queue_window) ||
                !options->queue_window)
                return false;
        } else if (strcmp(argv[i], "--writes") == 0) {
            if (++i == argc || !roce_parse_uint(argv[i], ROCE_PSN_MASK, &options->writes) ||
                !options->writes)
                return false;
        } else if (strcmp(argv[i], "--qps") == 0) {
            if (++i == argc || !roce_parse_uint(argv[i], ROCE_LAB_MAX_QPS, &options->qps) || !options->qps)
                return false;
        } else if (strcmp(argv[i], "--workers") == 0) {
            if (++i == argc ||
                !roce_parse_uint(argv[i], ROCE_LAB_MAX_QPS, &options->workers) || !options->workers)
                return false;
        } else if (strcmp(argv[i], "--start-gate") == 0) {
            if (++i == argc ||
                !roce_parse_uint(argv[i], UINT16_MAX, &options->start_gate) || !options->start_gate)
                return false;
        } else if (strcmp(argv[i], "--app-socket") == 0) {
            if (++i == argc || !argv[i][0] || options->app_socket)
                return false;
            options->app_socket = argv[i];
        } else if (strcmp(argv[i], "--app-post-batch") == 0) {
            if (++i == argc || !roce_parse_uint(argv[i], ROCE_PSN_MASK, &options->app_post_batch) ||
                !options->app_post_batch)
                return false;
        } else if (strcmp(argv[i], "--mr-socket") == 0) {
            if (++i == argc || !argv[i][0] || options->mr_socket)
                return false;
            options->mr_socket = argv[i];
        } else {
            return false;
        }
    }
    return true;
}

static bool connect_application(struct roce_app_memory *application, const struct roce_lab_options *options,
                                const struct datapath_options *datapath, const struct roce_lab_offer *local,
                                const struct roce_lab_offer *peer)
{
    if (!datapath->native_queues)
        return !datapath->cm_peer.ipv4 || roce_app_connected(application, options);
    const struct roce_native_queue_config config = {
        .version = ROCE_NATIVE_QUEUE_VERSION,
        .qps = options->qps, .workers = options->workers, .writes = options->writes,
        .write_size = options->size,
        .local_qpn = local->qpn, .peer_qpn = peer->qpn,
        .local_psn = local->psn, .peer_psn = peer->psn,
        .pmtu = RTE_MIN(local->pmtu, peer->pmtu),
        .local_lkey = application->mr.lkey, .remote_rkey = peer->region.rkey,
        .local_ipv4 = local->data.ipv4, .peer_ipv4 = peer->data.ipv4,
        .local_address = local->region.address, .remote_address = peer->region.address,
    };
    return roce_app_offer_native(application, &config);
}

int main(int argc, char **argv)
{
    int consumed = rte_eal_init(argc, argv);
    if (consumed < 0)
        return 1;
    argc -= consumed;
    argv += consumed;

    struct roce_lab_options options;
    struct datapath_options datapath;
    int result = 1;
    uint16_t port = RTE_MAX_ETHPORTS;
    if (argc < 8 || !roce_lab_parse(argv, &options) ||
        !parse_datapath_options(argc - 8, argv + 8, &datapath)) {
        fprintf(stderr, "usage: roce_write EAL_ARGS -- tx|rx BYTES CONTROL_IP TCP_PORT DATA_IP PMTU DPDK_PORT"
                        " [--report-bw] [--tx-burst N] [--send-window N] [--writes N] [--qps N]"
                        " [--workers N] [--queue-window N] [--start-gate PORT] [--app-socket PATH]"
                        " [--mr-socket PATH] [--app-post-batch N] [--verbs-queues]"
                        " [--scatter-gather] [--cm-peer IPV4 MAC]\n");
        goto cleanup_eal;
    }
    if (datapath.start_gate && !options.sender) {
        fprintf(stderr, "only a requester uses the start gate\n");
        goto cleanup_eal;
    }
    if (datapath.cm_peer.ipv4 && (datapath.qps != 1 || datapath.cm_peer.ipv4 == options.data_ip)) {
        fprintf(stderr, "in-band lab CM requires one QP and a distinct peer address\n");
        goto cleanup_eal;
    }
    if (datapath.mr_socket &&
        (!datapath.app_socket || strcmp(datapath.app_socket, datapath.mr_socket) == 0)) {
        fprintf(stderr, "verbs memory registration requires separate application and MR socket paths\n");
        goto cleanup_eal;
    }
    if (datapath.app_post_batch && (!options.sender || !datapath.app_socket)) {
        fprintf(stderr, "an application-backed requester uses the application post batch\n");
        goto cleanup_eal;
    }
    if (datapath.native_queues &&
        (!datapath.mr_socket || !options.sender || datapath.writes > ROCE_NATIVE_QUEUE_DEPTH)) {
        fprintf(stderr, "native requester queues require an MR socket and at most %u WRITEs per QP\n",
                ROCE_NATIVE_QUEUE_DEPTH);
        goto cleanup_eal;
    }
    unsigned int lcore;
    uint32_t available = 1;
    RTE_LCORE_FOREACH_WORKER(lcore) {
        if (available == datapath.workers)
            break;
        datapath.lcores[available++] = lcore;
    }
    if (datapath.workers > datapath.qps || available < datapath.workers) {
        fprintf(stderr, "each worker needs a QP and an enabled EAL lcore\n");
        goto cleanup_eal;
    }
    options.writes = datapath.writes;
    options.qps = datapath.qps;
    options.workers = datapath.workers;
    uint64_t bytes = (uint64_t)options.size * options.writes * options.qps;
    if (bytes > SIZE_MAX) {
        fprintf(stderr, "total payload does not fit in a single host buffer\n");
        goto cleanup_eal;
    }
    uint32_t port_id;
    if (roce_parse_uint(argv[7], UINT16_MAX, &port_id)) {
        port = (uint16_t)port_id;
    } else if (rte_eth_dev_get_port_by_name(argv[7], &port) < 0) {
        fprintf(stderr, "could not resolve DPDK port %s\n", argv[7]);
        goto cleanup_eal;
    }

    struct rte_eth_dev_info info;
    uint16_t mtu;
    if (rte_eth_dev_info_get(port, &info) || rte_eth_dev_get_mtu(port, &mtu)) {
        fprintf(stderr, "could not query DPDK port %u\n", port);
        goto cleanup_eal;
    }
    uint16_t required_mtu = (uint16_t)(ROCE_MAX_FRAME - ROCE_MAX_PMTU + options.pmtu -
                                       RTE_ETHER_HDR_LEN);
    bool memif = strcmp(info.driver_name, "net_memif") == 0;
    if (datapath.scatter_gather &&
        (!options.sender || !datapath.app_socket || !(info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MULTI_SEGS) ||
         (!memif && (strcmp(info.driver_name, "mlx5_pci") != 0 || rte_eal_iova_mode() != RTE_IOVA_VA)))) {
        fprintf(stderr, "scatter/gather requires an application-backed sender and memif or mlx5 with IOVA=VA\n");
        goto cleanup_eal;
    }
    if (mtu < required_mtu && !memif) {
        fprintf(stderr, "DPDK port MTU %u is below the required %u\n", mtu, required_mtu);
        goto cleanup_eal;
    }
    if (mtu < required_mtu)
        mtu = required_mtu;
    if (mtu > UINT16_MAX - RTE_ETHER_HDR_LEN)
        goto cleanup_eal;

    uint16_t frame = mtu + RTE_ETHER_HDR_LEN;
    struct rte_mempool *pool = mrc_dpdk_packet_pool_with_cache(frame, options.workers, MBUF_CACHE_SIZE);
    struct roce_app_memory application = { .socket = -1 };
    struct roce_mr_server *registrations = NULL;
    void *mlx5_pmd = NULL;
    uint8_t *buffer = datapath.app_socket ? NULL : malloc((size_t)bytes);
    if (!pool || (!datapath.app_socket && !buffer)) {
        fprintf(stderr, "packet pool or host buffer allocation failed\n");
        goto cleanup_buffers;
    }
    /* Each attached segment holds one 16-bit shared-info reference. */
    if (datapath.scatter_gather && pool->size >= UINT16_MAX) {
        fprintf(stderr, "packet pool is too large for scatter/gather buffer references\n");
        goto cleanup_buffers;
    }
    if (buffer)
        roce_lab_fill(buffer, (size_t)bytes, options.sender);

    struct rte_ether_addr mac;
    struct tx_memory_ref tx_refs[ROCE_LAB_MAX_QPS] = {0};
    bool dma_registered = false, dma_mapped = false;
    void *dma_address = NULL;
    size_t dma_length = 0;
    mlx5_dmabuf_map_fn dmabuf_map = NULL;
    mlx5_dmabuf_unmap_fn dmabuf_unmap = NULL;
    uint64_t offloads = datapath.scatter_gather ? RTE_ETH_TX_OFFLOAD_MULTI_SEGS : 0;
    int err = mrc_dpdk_open_port_with_offloads(port, pool, &mac, mtu, (uint16_t)options.workers, offloads);
    if (err) {
        fprintf(stderr, "could not open DPDK port %u: %s\n", port, strerror(-err));
        goto cleanup_buffers;
    }
    for (uint32_t i = 0; datapath.scatter_gather && i < options.workers; ++i) {
        tx_refs[i].shared.free_cb = release_tx_reference;
        rte_mbuf_ext_refcnt_set(&tx_refs[i].shared, 1);
    }
    bool in_band = datapath.cm_peer.ipv4 != 0;
    if (datapath.scatter_gather && datapath.mr_socket && !memif) {
        /* Use the same PMD instance that owns the port and its protection domain. */
        mlx5_pmd = dlopen("librte_net_mlx5.so.24", RTLD_NOW | RTLD_NOLOAD);
        if (mlx5_pmd) {
            dmabuf_map = (mlx5_dmabuf_map_fn)dlsym(mlx5_pmd, "rte_pmd_mlx5_dmabuf_map");
            dmabuf_unmap = (mlx5_dmabuf_unmap_fn)dlsym(mlx5_pmd, "rte_pmd_mlx5_dmabuf_unmap");
        }
        if (!dmabuf_map || !dmabuf_unmap) {
            fprintf(stderr, "physical scatter/gather from verbs memory requires the mlx5 DMA-BUF patch\n");
            goto cleanup_port;
        }
    }
    if (datapath.mr_socket) {
        registrations = roce_mr_server_start(datapath.mr_socket);
        if (!registrations) {
            perror("verbs memory registration listener");
            goto cleanup_port;
        }
    }
    if (datapath.app_socket) {
        uint32_t post_batch = datapath.app_post_batch ? RTE_MIN(datapath.app_post_batch, options.writes) :
            options.writes;
        if (!roce_app_attach(datapath.app_socket, &options, datapath.scatter_gather && !memif,
                             in_band, datapath.scatter_gather, datapath.native_queues, post_batch,
                             registrations, &application)) {
            perror("application memory registration");
            goto cleanup_port;
        }
        buffer = application.buffer;
    }
    if (datapath.scatter_gather && !memif) {
        dma_address = dmabuf_map ? application.mr.mapping : buffer;
        dma_length = dmabuf_map ? application.mr.mapped_length : application.length;
        long page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0 || rte_extmem_register(dma_address, dma_length, NULL, 0, (size_t)page_size)) {
            fprintf(stderr, "external memory registration failed: %s\n", rte_strerror(rte_errno));
            goto cleanup_port;
        }
        dma_registered = true;
        if (dmabuf_map) {
            int fd = roce_mr_export_dmabuf(&application.mr);
            if (fd < 0) {
                perror("application DMA-BUF export");
                goto cleanup_port;
            }
            err = dmabuf_map(port, dma_address, dma_length, fd, 0);
            close(fd);
            if (err) {
                fprintf(stderr, "mlx5 DMA-BUF mapping failed: %s\n", rte_strerror(-err));
                goto cleanup_port;
            }
        } else if (rte_dev_dma_map(info.device, dma_address, (uintptr_t)dma_address, dma_length)) {
            fprintf(stderr, "mlx5 DMA mapping failed: %s\n", rte_strerror(rte_errno));
            goto cleanup_port;
        }
        dma_mapped = true;
        printf("Application %s region mapped for scatter/gather TX (%zu bytes)\n",
               dmabuf_map ? "DMA-BUF" : "DMA", dma_length);
    }
    struct rte_eth_stats initial_stats = {0};
    bool measure_stats = datapath.report_bandwidth && rte_eth_stats_get(port, &initial_stats) == 0;
    /* Virtual PMDs can return a zero MAC. Give those lab endpoints a local MAC. */
    if (rte_is_zero_ether_addr(&mac))
        mac = (struct rte_ether_addr) { .addr_bytes = {
            0x02, 0x00, (uint8_t)(options.data_ip >> 24), (uint8_t)(options.data_ip >> 16),
            (uint8_t)(options.data_ip >> 8), (uint8_t)options.data_ip,
        } };

    struct roce_lab_offer local = {
        .data.ipv4 = options.data_ip, .pmtu = options.pmtu,
        .region = { .address = UINT64_C(0x100000000), .size = options.size },
    }, peer;
    memcpy(local.data.mac, mac.addr_bytes, ROCE_MAC_LEN);
    if (!roce_lab_random(&local.psn) || !roce_lab_random(&local.qpn) ||
        (!registrations && !roce_lab_random(&local.region.rkey)))
        goto cleanup_port;
    if (registrations) {
        local.region.address = application.mr.iova;
        local.region.rkey = application.mr.rkey;
    }
    local.psn &= ROCE_PSN_MASK;
    local.qpn = 2 + local.qpn % (ROCE_PSN_MASK - options.qps - 1);

    int fd = roce_lab_open(&options);
    if (fd < 0) {
        perror("control socket");
        goto cleanup_port;
    }
    struct roce_cm_session cm;
    bool connected = in_band ?
        roce_cm_lab_exchange(port, pool, fd, &options, &local, &datapath.cm_peer, &peer, &cm) :
        roce_lab_exchange(fd, &options, &local, &peer);
    if (!connected) {
        fprintf(stderr, "%s setup failed\n", in_band ? "RoCEv2 CM" : "control");
    } else if (application.socket >= 0 && !connect_application(&application, &options, &datapath, &local, &peer)) {
        perror("application establishment");
    } else {
        if (in_band)
            printf("RoCEv2 CM established local_qpn=%06x peer_qpn=%06x\n", local.qpn, peer.qpn);
        result = run(fd, port, pool, &local, &peer, &options, buffer, &application,
                     options.workers > 1 && !memif, &datapath, datapath.scatter_gather ? tx_refs : NULL);
        if (!result && in_band) {
            if (roce_cm_lab_close(port, pool, fd, &cm)) {
                puts("RoCEv2 CM disconnected");
            } else {
                fprintf(stderr, "RoCEv2 CM disconnect failed\n");
                result = 1;
            }
        }
    }
    if (result || measure_stats) {
        struct rte_eth_stats stats;
        if (rte_eth_stats_get(port, &stats) == 0) {
            if (measure_stats && !result)
                printf("PORT_STATS rx_packets=%" PRIu64 " tx_packets=%" PRIu64
                       " missed=%" PRIu64 " rx_errors=%" PRIu64 " tx_errors=%" PRIu64
                       " rx_nombuf=%" PRIu64 "\n",
                       stats.ipackets - initial_stats.ipackets, stats.opackets - initial_stats.opackets,
                       stats.imissed - initial_stats.imissed, stats.ierrors - initial_stats.ierrors,
                       stats.oerrors - initial_stats.oerrors, stats.rx_nombuf - initial_stats.rx_nombuf);
            else
                fprintf(stderr, "DPDK port %u RX=%" PRIu64 " TX=%" PRIu64
                        " missed=%" PRIu64 " RX errors=%" PRIu64 " TX errors=%" PRIu64 "\n",
                        port, stats.ipackets, stats.opackets, stats.imissed, stats.ierrors, stats.oerrors);
        }
    }
    close(fd);
cleanup_port:
    if (datapath.scatter_gather) {
        err = rte_eth_dev_stop(port);
        if (err) {
            fprintf(stderr, "could not stop scatter/gather port: %s\n", rte_strerror(-err));
            return 1;
        }
        for (uint32_t i = 0; i < options.workers; ++i) {
            if (rte_mbuf_ext_refcnt_read(&tx_refs[i].shared) != 1) {
                fprintf(stderr, "worker %u still has application buffers in its TX queue\n", i);
                return 1;
            }
        }
        if (dma_mapped) {
            err = dmabuf_unmap ? dmabuf_unmap(port, dma_address, dma_length) :
                rte_dev_dma_unmap(info.device, dma_address, (uintptr_t)dma_address, dma_length);
            if (err) {
                fprintf(stderr, "mlx5 DMA unmapping failed: %s\n",
                        rte_strerror(dmabuf_unmap ? -err : rte_errno));
                return 1;
            }
        }
        if (dma_registered && rte_extmem_unregister(dma_address, dma_length)) {
            fprintf(stderr, "external memory deregistration failed: %s\n", rte_strerror(rte_errno));
            return 1;
        }
        err = rte_eth_dev_close(port);
        if (err) {
            fprintf(stderr, "could not close scatter/gather port: %s\n", rte_strerror(-err));
            result = 1;
        }
    } else {
        mrc_dpdk_close_port(port);
    }
    if (!result && application.socket >= 0 && !roce_app_complete(&application, &options)) {
        perror("application WRITE completion");
        result = 1;
    }
    if (result)
        roce_app_flush_native(&application, &options);
cleanup_buffers:
    if (mlx5_pmd)
        dlclose(mlx5_pmd);
    if (application.socket >= 0)
        roce_app_detach(&application);
    else
        free(buffer);
    if (registrations) {
        int error = roce_mr_server_stop(registrations);
        if (error) {
            fprintf(stderr, "verbs memory registration shutdown: %s\n", strerror(error));
            result = 1;
        }
    }
    if (pool)
        rte_mempool_free(pool);
cleanup_eal:
    rte_eal_cleanup();
    return result;
}
