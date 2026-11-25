#define _GNU_SOURCE
#include "roce_app.h"
#include "roce_cm_dpdk.h"
#include "roce_doorbell.h"
#include "roce_lab.h"
#include "roce_mr_ipc.h"
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
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_memory.h>
#include <rte_pause.h>

#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

enum {
    RX_BURST = 32, MAX_TX_BURST = 32, NS_PER_SEC = 1000000000,
    RETRY_NS = 10000000, CONTROL_POLL_NS = 1000000,
    MBUF_CACHE_SIZE = 256,
};

_Static_assert((int)ROCE_CM_RX_BURST <= (int)RX_BURST, "the data burst can hold prefetched CM packets");
_Static_assert(ROCE_LAB_MAX_QPS <= 32, "the completion mask must hold all worker QPs");

struct datapath_options {
    uint32_t send_window;
    uint32_t queue_window;
    uint32_t tx_burst;
    uint32_t writes;
    uint32_t qps;
    uint32_t workers;
    uint32_t start_gate;
    uint32_t app_post_batch;
    uint32_t tap_mtu;
    unsigned int lcores[ROCE_LAB_MAX_QPS];
    const char *app_socket;
    const char *mr_socket;
    const char *rdmacm_socket;
    struct roce_address cm_peer;
    bool report_bandwidth;
    bool scatter_gather;
    bool native_queues;
    bool two_sge_writes;
    bool multi_mr_writes;
    bool multi_mr_targets;
    bool prefault_app;
};

struct tx_memory_ref {
    struct rte_mbuf_ext_shared_info shared;
} __rte_cache_aligned;

typedef int (*mlx5_dmabuf_map_fn)(uint16_t, void *, size_t, int, uint64_t);
typedef int (*mlx5_dmabuf_unmap_fn)(uint16_t, void *, size_t);
typedef int (*mlx5_tx_burst_completion_fn)(uint16_t, uint16_t);

struct tx_region {
    struct roce_mr_lease lease;
    uint32_t refs;
    bool external;
    bool mapped;
};

struct tx_regions {
    pthread_mutex_t mutex;
    const struct roce_mr_lease *anchor;
    mlx5_dmabuf_map_fn map;
    mlx5_dmabuf_unmap_fn unmap;
    uint16_t port;
    struct tx_region entries[ROCE_MR_IPC_MAX_REGISTRATIONS];
};

union write_qp {
    struct roce_requester requester;
    struct roce_responder responder;
};

enum run_state { WRITE_WAITING, WRITE_RUNNING, WRITE_STOPPED };

struct native_write {
    const uint8_t *source;
    const uint8_t *second;
    uint64_t remote;
    uint32_t rkey;
    uint32_t first_packet;
    uint32_t end_packet;
    uint32_t length;
    uint32_t first_length;
    struct tx_region *regions[ROCE_NATIVE_MAX_SEND_SGES];
};

struct write_run {
    const struct datapath_options *options;
    struct rte_mempool *pool;
    struct roce_app_work *work;
    struct roce_native_work *native;
    struct roce_cm_port *cm;
    struct native_write (*native_writes)[ROCE_NATIVE_QUEUE_DEPTH];
    const uint8_t *local_buffer;
    uint64_t local_iova;
    size_t local_length;
    uint32_t local_lkey;
    struct tx_memory_ref *tx_refs;
    struct tx_regions *regions;
    const struct roce_mr_lease *receive_anchor;
    struct roce_mr_destination destinations[ROCE_LAB_MAX_QPS];
    uint16_t port;
    int application_socket;
    bool sender;
    atomic_int state;
    atomic_uint finished_workers;
    atomic_bool failed;
};

struct poll_usage {
    uint64_t cpu_ns;
    uint64_t minor_faults;
    uint64_t major_faults;
    bool has_faults;
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
    uint64_t doorbell_generation;
    uint64_t next_doorbell_recovery;
    uint32_t fully_posted;
    uint32_t pending_completions;
    uint32_t posted[ROCE_LAB_MAX_QPS];
    uint32_t next_write[ROCE_LAB_MAX_QPS];
    uint32_t acknowledged_writes[ROCE_LAB_MAX_QPS];
    uint32_t completions[ROCE_LAB_MAX_QPS];
    uint64_t admitted_bytes[ROCE_LAB_MAX_QPS];
    bool finished;
    struct poll_usage initial_usage;
    struct poll_usage usage;
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

static void attach_payload(struct rte_mbuf *mbuf, struct rte_mbuf_ext_shared_info *shared,
                           const uint8_t *data, uint16_t length)
{
    rte_pktmbuf_attach_extbuf(mbuf, (void *)data, (rte_iova_t)(uintptr_t)data, length, shared);
    mbuf->data_len = length;
    mbuf->pkt_len = length;
}

static struct rte_mbuf *encode_gathered_packet(struct rte_mempool *pool,
                                               struct rte_mbuf_ext_shared_info *shared,
                                               const struct roce_packet *packet)
{
    struct rte_mbuf *segments[4];
    uint16_t count = 3 + (packet->payload_tail_length != 0);
    if (rte_pktmbuf_alloc_bulk(pool, segments, count) != 0)
        return NULL;
    struct rte_mbuf *header = segments[0], *payload = segments[1], *trailer = segments[count - 1];
    uint16_t header_len, trailer_len;
    if (!roce_encode_split(rte_pktmbuf_mtod(header, uint8_t *),
                           rte_pktmbuf_mtod(trailer, uint8_t *), packet, &header_len, &trailer_len)) {
        for (uint16_t i = 0; i < count; ++i)
            rte_pktmbuf_free(segments[i]);
        return NULL;
    }
    uint16_t payload_len = (uint16_t)packet->payload_length;
    uint16_t tail_len = packet->payload_tail_length;
    rte_mbuf_ext_refcnt_update(shared, count - 2);
    attach_payload(payload, shared, packet->payload, payload_len - tail_len);
    payload->next = trailer;
    if (tail_len) {
        struct rte_mbuf *tail = segments[2];
        attach_payload(tail, shared, packet->payload_tail, tail_len);
        payload->next = tail;
        tail->next = trailer;
    }
    trailer->data_len = trailer_len;
    trailer->pkt_len = trailer_len;
    header->data_len = header_len;
    header->pkt_len = header_len + payload_len + trailer_len;
    header->nb_segs = count;
    header->next = payload;
    return header;
}

static struct rte_mbuf_ext_shared_info *write_tx_reference(const struct write_worker *worker,
                                                          uint32_t index, uint32_t sequence)
{
    const struct write_run *run = worker->run;
    uint32_t slot = run->native ? (worker->first_qp + index) * ROCE_NATIVE_QUEUE_DEPTH +
                                    sequence % ROCE_NATIVE_QUEUE_DEPTH : worker->queue;
    return &run->tx_refs[slot].shared;
}

static uint32_t stage_native_write(struct write_worker *worker, uint32_t index, uint32_t staged,
                                   struct roce_packet *packet)
{
    const struct roce_requester *requester = &worker->states[index].requester;
    if (!roce_requester_can_stage(requester, staged))
        return UINT32_MAX;
    uint32_t packet_index = requester->next + staged;
    const struct native_write *writes = worker->run->native_writes[worker->first_qp + index];
    uint32_t sequence = RTE_MAX(worker->next_write[index], worker->acknowledged_writes[index]);
    /* Short TX and retry can resume before the last staged WRITE. */
    if (packet_index < writes[sequence % ROCE_NATIVE_QUEUE_DEPTH].first_packet)
        sequence = worker->acknowledged_writes[index];
    while (packet_index >= writes[sequence % ROCE_NATIVE_QUEUE_DEPTH].end_packet)
        ++sequence;
    worker->next_write[index] = sequence;
    const struct native_write *write = &writes[sequence % ROCE_NATIVE_QUEUE_DEPTH];
    uint32_t segment = packet_index - write->first_packet;
    uint32_t offset = segment * requester->qp.pmtu;
    const uint8_t *payload = offset < write->first_length ? write->source + offset :
        write->second + (offset - write->first_length);
    roce_requester_write_packet(requester, packet_index, segment, payload, write->remote, write->rkey,
                                write->length, packet);
    if (offset < write->first_length && packet->payload_length > write->first_length - offset) {
        packet->payload_tail = write->second;
        packet->payload_tail_length = (uint16_t)(packet->payload_length - (write->first_length - offset));
    }
    return sequence;
}

static bool send_requests(struct write_worker *worker,
                          uint32_t index, uint32_t posted_packets,
                          uint32_t credit, uint64_t now)
{
    const struct write_run *run = worker->run;
    struct roce_requester *requester = &worker->states[index].requester;
    uint32_t budget = RTE_MIN(posted_packets - requester->next,
                              credit + (requester->sent - requester->next));
    uint32_t limit = RTE_MIN(budget, run->options->tx_burst);
    struct rte_mbuf *packets[MAX_TX_BURST];
    struct roce_packet packet;
    uint16_t count = 0;
    while (count < limit) {
        uint32_t sequence = run->native ? stage_native_write(worker, index, count, &packet) :
                                         roce_requester_stage(requester, count, &packet);
        if (sequence == UINT32_MAX)
            break;
        /* A full queue window must still generate an ACK before another send. */
        if (run->options->queue_window && count + 1u == budget)
            packet.ack_requested = true;
        if (run->tx_refs && packet.payload_length) {
            packets[count] = encode_gathered_packet(run->pool, write_tx_reference(worker, index, sequence), &packet);
        } else {
            packets[count] = encode_packet(run->pool, &packet);
        }
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

static struct poll_usage thread_usage(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    struct poll_usage usage = { .cpu_ns = (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec };
    struct rusage counters;
    if (!getrusage(RUSAGE_THREAD, &counters) && counters.ru_minflt >= 0 && counters.ru_majflt >= 0) {
        usage.minor_faults = (uint64_t)counters.ru_minflt;
        usage.major_faults = (uint64_t)counters.ru_majflt;
        usage.has_faults = true;
    }
    return usage;
}

static bool prefault_application(const uint8_t *buffer, size_t bytes)
{
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return false;

    const volatile uint8_t *alias = buffer;
    const size_t page = (size_t)page_size;
    const size_t first_boundary = page - (uintptr_t)buffer % page;
    const uint64_t start = roce_now_ns();
    size_t pages = 1;
    (void)alias[0];
    for (size_t offset = first_boundary; offset < bytes; offset += page) {
        (void)alias[offset];
        ++pages;
    }
    fprintf(stderr, "APP_PREFAULT bytes=%zu pages=%zu elapsed_ns=%" PRIu64 "\n",
            bytes, pages, roce_now_ns() - start);
    return true;
}

static void report_dpdk_rx(uint16_t port, const struct rte_mempool *pool, uint16_t queues)
{
    fprintf(stderr, "DPDK_PACKET_POOL hugepages=%d chunks=%u\n", rte_eal_has_hugepages(), pool->nb_mem_chunks);
    for (uint16_t queue = 0; queue < queues; ++queue) {
        struct rte_eth_burst_mode mode = {0};
        const char *description = rte_eth_rx_burst_mode_get(port, queue, &mode) ? "Unavailable" : mode.info;
        fprintf(stderr, "DPDK_RX_MODE queue=%u mode=%s\n", queue, description);
    }
}

static void report_bandwidth(bool sender, uint64_t bytes, uint64_t started, uint64_t elapsed,
                             const struct poll_usage *usage, const struct datapath_options *options)
{
    if (!elapsed)
        return;
    printf("BANDWIDTH role=%s payload_bytes=%" PRIu64 " elapsed_ns=%" PRIu64
           " thread_cpu_ns=%" PRIu64 " payload_gbps=%.3f poll_cores=%.3f writes=%u qps=%u workers=%u"
           " start_ns=%" PRIu64,
           sender ? "tx" : "rx", bytes, elapsed, usage->cpu_ns,
           (double)bytes * 8.0 / (double)elapsed, (double)usage->cpu_ns / (double)elapsed,
           options->writes, options->qps, options->workers, started);
    if (usage->has_faults)
        printf(" poll_minor_faults=%" PRIu64 " poll_major_faults=%" PRIu64,
               usage->minor_faults, usage->major_faults);
    if (sender) {
        printf(" send_window=%u tx_burst=%u", options->send_window, options->tx_burst);
        if (options->queue_window)
            printf(" queue_window=%u", options->queue_window);
        printf(" tx_mode=%s", options->scatter_gather ? "scatter-gather" : "copy");
    }
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
    struct write_run *run = worker->run;
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
            if (run->native)
                worker->states[i].requester.total = 0;
        } else {
            worker->states[i].responder = (struct roce_responder) {
                .qp = qp, .buffer = buffer + offset,
                .expected = roce_native_psn(peer->psn, index),
                .region = { .address = local->region.address + offset,
                            .rkey = local->region.rkey, .size = qp_bytes },
            };
            if (run->receive_anchor) {
                struct roce_responder *responder = &worker->states[i].responder;
                run->destinations[index].anchor = run->receive_anchor;
                responder->resolve = roce_mr_resolve_write;
                responder->release = roce_mr_release_write;
                responder->target_context = &run->destinations[index];
            }
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
    uint16_t count;
    if (run->cm && run->cm->early_count) {
        count = run->cm->early_count;
        memcpy(received, run->cm->early, count * sizeof(*received));
        run->cm->early_count = 0;
    } else {
        count = rte_eth_rx_burst(run->port, worker->queue, received, RX_BURST);
    }
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
                    uint32_t acknowledged = requester->acked;
                    roce_requester_feedback(requester, &packet, now);
                    if (requester->acked != acknowledged) {
                        worker->completed += requester->acked == requester->total &&
                            (!run->native || worker->posted[index] == writes);
                        if (run->work || run->native)
                            worker->pending_completions |= UINT32_C(1) << index;
                    }
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
        } else if (run->cm && frame && length == ROCE_CM_FRAME_SIZE) {
            struct roce_cm_session *session = &run->cm->session;
            if (roce_cm_session_receive(session, now, frame, length) && session->phase != ROCE_CM_ESTABLISHED) {
                for (; i < count; ++i)
                    rte_pktmbuf_free(received[i]);
                break;
            }
        }
        rte_pktmbuf_free(received[i]);
    }
    if (run->cm) {
        struct roce_cm_session *session = &run->cm->session;
        if (session->send_pending || session->drep_pending)
            roce_cm_transmit(run->port, run->pool, session, now);
        if (session->phase != ROCE_CM_ESTABLISHED)
            return false;
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

static bool unmap_tx_region(const struct tx_regions *regions, struct tx_region *entry)
{
    const struct roce_mr_lease *lease = &entry->lease;
    if (entry->mapped) {
        if (regions->unmap(regions->port, lease->mapping, lease->mapped_length))
            return false;
        entry->mapped = false;
    }
    if (entry->external) {
        if (rte_extmem_unregister(lease->mapping, lease->mapped_length))
            return false;
        entry->external = false;
    }
    roce_mr_release(&entry->lease);
    entry->refs = 0;
    return true;
}

static bool map_tx_region(const struct tx_regions *regions, struct tx_region *entry)
{
    if (!regions->map)
        return true;
    const struct roce_mr_lease *lease = &entry->lease;
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || rte_extmem_register(lease->mapping, lease->mapped_length, NULL, 0, (size_t)page))
        return false;
    entry->external = true;
    int fd = roce_mr_export_dmabuf(lease);
    if (fd < 0)
        return false;
    int error = regions->map(regions->port, lease->mapping, lease->mapped_length, fd, 0);
    close(fd);
    entry->mapped = !error;
    return entry->mapped;
}

static const uint8_t *acquire_tx_span(struct tx_regions *regions, uint32_t lkey, uint64_t address,
                                      uint32_t length, struct tx_region **retained)
{
    const uint8_t *source = NULL;
    pthread_mutex_lock(&regions->mutex);
    struct tx_region *entry = NULL, *unused = NULL;
    for (size_t i = 0; i < RTE_DIM(regions->entries); ++i) {
        struct tx_region *candidate = &regions->entries[i];
        if (candidate->refs && candidate->lease.lkey == lkey) {
            entry = candidate;
            break;
        }
        if (!unused && !candidate->lease.server)
            unused = candidate;
    }
    bool new_lease = !entry;
    if (new_lease && unused &&
        !roce_mr_acquire_related(regions->anchor, lkey, regions->map ? ROCE_MR_IPC_WRITE : 0, &unused->lease))
        entry = unused;
    if (entry) {
        const struct roce_mr_lease *lease = &entry->lease;
        if (roce_native_contains(lease->iova, lease->length, address, length) &&
            (!new_lease || map_tx_region(regions, entry))) {
            ++entry->refs;
            *retained = entry;
            source = lease->buffer + (address - lease->iova);
        } else if (new_lease && !unmap_tx_region(regions, entry)) {
            fprintf(stderr, "could not unwind a native TX mapping\n");
        }
    }
    pthread_mutex_unlock(&regions->mutex);
    return source;
}

static bool release_tx_regions(struct tx_regions *regions, struct native_write *write)
{
    if (!write->regions[0] && !write->regions[1])
        return true;
    bool released = true;
    pthread_mutex_lock(&regions->mutex);
    for (size_t i = 0; i < RTE_DIM(write->regions); ++i) {
        struct tx_region *entry = write->regions[i];
        if (entry && !--entry->refs && !unmap_tx_region(regions, entry))
            released = false;
        write->regions[i] = NULL;
    }
    pthread_mutex_unlock(&regions->mutex);
    return released;
}

static const uint8_t *resolve_tx_span(const struct write_run *run, uint32_t lkey, uint64_t address,
                                      uint32_t length, struct tx_region **retained)
{
    if (lkey == run->local_lkey)
        return roce_native_contains(run->local_iova, run->local_length, address, length) ?
               run->local_buffer + (address - run->local_iova) : NULL;
    return run->regions ? acquire_tx_span(run->regions, lkey, address, length, retained) : NULL;
}

static enum roce_native_wc_status validate_native_write(const struct write_worker *worker, uint32_t index,
                                                        const struct roce_native_write *wqe,
                                                        const struct roce_native_sge *second)
{
    const struct roce_requester *requester = &worker->states[index].requester;
    uint64_t bytes = wqe->length;
    if (!bytes || bytes > ROCE_NATIVE_MAX_WRITE || second->length >= bytes)
        return ROCE_NATIVE_WC_LOCAL_LENGTH;
    if (!worker->run->options->multi_mr_targets &&
        (wqe->rkey != requester->remote.rkey ||
         !roce_native_contains(requester->remote.address, requester->remote.size * worker->run->options->writes,
                               wqe->remote_address, bytes)))
        return ROCE_NATIVE_WC_REMOTE_ACCESS;
    return ROCE_NATIVE_WC_SUCCESS;
}

static bool admit_native_qp(struct write_worker *worker, uint32_t index)
{
    const struct write_run *run = worker->run;
    uint32_t qp = worker->first_qp + index;
    struct roce_native_sq *sq = &run->native->qps[qp];
    const struct roce_native_sge *second = run->options->two_sge_writes ?
        ((const struct roce_native_sge_work *)run->native)->second[qp] : NULL;
    struct roce_requester *requester = &worker->states[index].requester;
    uint32_t head = atomic_load_explicit(&sq->posted, memory_order_acquire);
    uint32_t sequence = worker->posted[index];
    if (head == sequence)
        return true;
    if (head > run->options->writes || head - sequence > ROCE_NATIVE_QUEUE_DEPTH ||
        head - worker->completions[index] > ROCE_NATIVE_QUEUE_DEPTH)
        return false;
    uint64_t bytes = worker->admitted_bytes[index];
    uint64_t capacity = requester->remote.size * run->options->writes;
    uint32_t packets = requester->total;
    for (; sequence < head; ++sequence) {
        uint32_t slot = sequence % ROCE_NATIVE_QUEUE_DEPTH;
        const struct roce_native_write wqe = sq->entries[slot];
        const struct roce_native_sge tail = second ? second[slot] : (struct roce_native_sge){0};
        enum roce_native_wc_status result = validate_native_write(worker, index, &wqe, &tail);
        if (result != ROCE_NATIVE_WC_SUCCESS) {
            sq->status[slot] = result;
            return false;
        }
        uint32_t count = (wqe.length - 1) / requester->qp.pmtu + 1;
        bytes += wqe.length;
        if (bytes > capacity || capacity - bytes < run->options->writes - sequence - 1 ||
            (sequence + 1 == run->options->writes && bytes != capacity) || count > UINT32_MAX - packets) {
            sq->status[slot] = ROCE_NATIVE_WC_LOCAL_LENGTH;
            return false;
        }
        /* Keep both PSN bounds even when the preceding slot has been reused. */
        struct native_write *write = &run->native_writes[qp][slot];
        *write = (struct native_write) {
            .remote = wqe.remote_address, .rkey = wqe.rkey, .length = wqe.length,
            .first_length = wqe.length - tail.length,
            .first_packet = packets, .end_packet = packets + count,
        };
        write->source = resolve_tx_span(run, wqe.lkey, wqe.local_address, write->first_length, &write->regions[0]);
        if (write->source && tail.length)
            write->second = resolve_tx_span(run, tail.lkey, tail.address, tail.length, &write->regions[1]);
        if (!write->source || (tail.length && !write->second)) {
            sq->status[slot] = ROCE_NATIVE_WC_LOCAL_PROTECTION;
            return false;
        }
        packets += count;
    }
    requester->total = packets;
    worker->admitted_bytes[index] = bytes;
    worker->posted[index] = head;
    worker->fully_posted += head == run->options->writes;
    atomic_store_explicit(&sq->admitted, head, memory_order_release);
    return true;
}

static bool admit_native_writes(struct write_worker *worker, uint64_t now)
{
    const struct write_run *run = worker->run;
    if (!run->native || worker->fully_posted == worker->qps)
        return true;
    struct roce_doorbell *doorbell = &run->native->doorbells[worker->queue];
    uint64_t notifications = atomic_load_explicit(&doorbell->notifications, memory_order_acquire);
    uint64_t generation = notifications & ~ROCE_NATIVE_DOORBELL_OVERFLOW;
    bool overflow = (notifications & ROCE_NATIVE_DOORBELL_OVERFLOW) != 0;
    bool recover = now >= worker->next_doorbell_recovery;
    if (generation == worker->doorbell_generation && !overflow && !recover)
        return true;

    /* Drain in bounded batches. A full batch may have more notifications behind it. */
    worker->doorbell_generation = UINT64_MAX;
    for (unsigned i = 0; i < ROCE_DOORBELL_SLOTS; ++i) {
        uint64_t item = roce_doorbell_take(doorbell, &worker->doorbell_cursor);
        if (!item) {
            worker->doorbell_generation = generation;
            break;
        }
        uint32_t index = (uint32_t)(item >> 32) - worker->first_qp - 1;
        if (index >= worker->qps || !admit_native_qp(worker, index))
            return false;
    }
    if (!overflow && !recover)
        return true;
    if (overflow)
        atomic_fetch_and_explicit(&doorbell->notifications, ~ROCE_NATIVE_DOORBELL_OVERFLOW,
                                  memory_order_acquire);
    /* Recover published heads if notification was interrupted or the queue was full. */
    worker->next_doorbell_recovery = now + CONTROL_POLL_NS;
    for (uint32_t i = 0; i < worker->qps; ++i)
        if (!admit_native_qp(worker, i))
            return false;
    return true;
}

static bool publish_completions(struct write_worker *worker)
{
    const struct write_run *run = worker->run;
    uint32_t pending = worker->pending_completions;
    if (!pending)
        return true;
    if (!run->native && run->tx_refs && rte_mbuf_ext_refcnt_read(&run->tx_refs[worker->queue].shared) != 1)
        return true;
    worker->pending_completions = 0;
    bool polled_tx = false;
    for (; pending; pending &= pending - 1) {
        uint32_t i = (uint32_t)__builtin_ctz(pending);
        const struct roce_requester *requester = &worker->states[i].requester;
        uint32_t completed;
        if (run->native) {
            struct roce_native_sq *sq = &run->native->qps[worker->first_qp + i];
            struct native_write *writes = run->native_writes[worker->first_qp + i];
            completed = worker->acknowledged_writes[i];
            while (completed < worker->posted[i] &&
                   writes[completed % ROCE_NATIVE_QUEUE_DEPTH].end_packet <= requester->acked)
                ++completed;
            worker->acknowledged_writes[i] = completed;
            atomic_store_explicit(&sq->acknowledged, completed, memory_order_relaxed);
            uint32_t retired = worker->completions[i];
            for (; retired < completed; ++retired) {
                if (run->tx_refs) {
                    struct rte_mbuf_ext_shared_info *shared = write_tx_reference(worker, i, retired);
                    if (rte_mbuf_ext_refcnt_read(shared) != 1 && !polled_tx) {
                        /* mlx5 can reap signaled TX completions even while the SQ has no new work. */
                        rte_eth_tx_descriptor_status(run->port, worker->queue, 0);
                        polled_tx = true;
                    }
                    if (rte_mbuf_ext_refcnt_read(shared) != 1)
                        break;
                }
                if (run->regions && !release_tx_regions(run->regions, &writes[retired % ROCE_NATIVE_QUEUE_DEPTH])) {
                    fprintf(stderr, "native TX mapping release failed\n");
                    return false;
                }
                sq->status[retired % ROCE_NATIVE_QUEUE_DEPTH] = ROCE_NATIVE_WC_SUCCESS;
            }
            /* The PMD can finish DMA after the peer's last ACK. */
            if (retired < completed)
                worker->pending_completions |= UINT32_C(1) << i;
            if (retired == worker->completions[i])
                continue;
            atomic_store_explicit(&sq->retired, retired, memory_order_release);
            completed = retired;
        } else {
            completed = requester->acked / requester->packets_per_write;
            if (completed != worker->completions[i])
                atomic_store_explicit(&run->work->qps[worker->first_qp + i].completed, completed,
                                      memory_order_release);
        }
        worker->completions[i] = completed;
    }
    return true;
}

static void record_requester_failure(const struct write_worker *worker, uint32_t index)
{
    const struct roce_requester *requester = &worker->states[index].requester;
    fprintf(stderr, "QP %06x WRITE failed after %u retries (%u/%u packets acknowledged)\n",
            requester->qp.local_qpn, requester->retries, requester->acked, requester->total);
    uint32_t write = worker->acknowledged_writes[index];
    if (!worker->run->native || write >= worker->posted[index])
        return;

    static const uint8_t status[] = {
        [ROCE_REQUESTER_RETRY_EXCEEDED] = ROCE_NATIVE_WC_RETRY_EXCEEDED,
        [ROCE_REQUESTER_REMOTE_INVALID] = ROCE_NATIVE_WC_REMOTE_INVALID,
        [ROCE_REQUESTER_REMOTE_ACCESS] = ROCE_NATIVE_WC_REMOTE_ACCESS,
        [ROCE_REQUESTER_REMOTE_OPERATION] = ROCE_NATIVE_WC_REMOTE_OPERATION,
    };
    struct roce_native_sq *sq = &worker->run->native->qps[worker->first_qp + index];
    sq->status[write % ROCE_NATIVE_QUEUE_DEPTH] = status[requester->failure];
}

static bool all_native_writes_retired(const struct write_worker *worker)
{
    if (worker->run->native)
        for (uint32_t i = 0; i < worker->qps; ++i)
            if (worker->completions[i] != worker->run->options->writes)
                return false;
    return true;
}

static bool poll_worker(struct write_worker *worker, uint64_t now)
{
    struct write_run *run = worker->run;
    if (run->sender && worker->finished)
        return true;
    if (!admit_writes(worker) || !admit_native_writes(worker, now)) {
        fprintf(stderr, "application work rejected on worker %u\n", worker->queue);
        return false;
    }
    bool received = receive_packets(worker, now);
    if (!publish_completions(worker))
        return false;
    if (received && worker->completed == worker->qps && !worker->finished && all_native_writes_retired(worker)) {
        worker->finished = true;
        if (run->options->report_bandwidth)
            worker->finished_ns = roce_now_ns();
        atomic_fetch_add_explicit(&run->finished_workers, 1, memory_order_relaxed);
    }
    if (!run->sender || worker->finished)
        return received;

    uint32_t outstanding = 0;
    for (uint32_t i = 0; i < worker->qps; ++i) {
        struct roce_requester *requester = &worker->states[i].requester;
        if (received)
            roce_requester_poll_timer(requester, now, RETRY_NS);
        if (requester->failure) {
            record_requester_failure(worker, i);
            return false;
        }
        outstanding += requester->sent - requester->acked;
    }
    if (!received)
        return false;
    uint32_t window = run->options->queue_window;
    uint32_t credit = window ? window - RTE_MIN(window, outstanding) : run->options->tx_burst;
    for (uint32_t i = 0; i < worker->qps; ++i) {
        uint32_t next = worker->next;
        struct roce_requester *requester = &worker->states[next].requester;
        uint32_t posted_packets = run->work ?
            worker->posted[next] * requester->packets_per_write : requester->total;
        if (++worker->next == worker->qps)
            worker->next = 0;
        if (send_requests(worker, next, posted_packets, credit, now))
            break;
    }
    return true;
}

static bool all_workers_finished(const struct write_run *run)
{
    return atomic_load_explicit(&run->finished_workers, memory_order_relaxed) == run->options->workers;
}

static void sample_usage(struct write_worker *worker)
{
    if (!worker->run->options->report_bandwidth || worker->usage.cpu_ns)
        return;
    struct poll_usage usage = thread_usage();
    const struct poll_usage *initial = &worker->initial_usage;
    usage.cpu_ns -= initial->cpu_ns;
    usage.has_faults = usage.has_faults && initial->has_faults &&
                       usage.minor_faults >= initial->minor_faults && usage.major_faults >= initial->major_faults;
    if (usage.has_faults) {
        usage.minor_faults -= initial->minor_faults;
        usage.major_faults -= initial->major_faults;
    }
    worker->usage = usage;
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

    if (run->options->report_bandwidth)
        worker->initial_usage = thread_usage();
    while (atomic_load_explicit(&run->state, memory_order_relaxed) == WRITE_RUNNING) {
        if (!poll_worker(worker, roce_now_ns())) {
            atomic_store_explicit(&run->failed, true, memory_order_relaxed);
            return 1;
        }
        /* RX continues polling after placement in case a final ACK needs replay. */
        if (run->sender && worker->finished)
            break;
        if (worker->finished && all_workers_finished(run))
            sample_usage(worker);
        rte_pause();
    }
    sample_usage(worker);
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
                sample_usage(main_worker);
        }
        rte_pause();
    }
    return false;
}

static bool verify_result(int fd, int application, const struct write_worker *workers, uint64_t started,
                          const uint8_t *buffer, uint64_t bytes)
{
    const struct write_run *run = workers[0].run;
    const struct datapath_options *options = run->options;
    uint64_t finished = started, retries = 0, packets = 0;
    struct poll_usage usage = { .has_faults = true };
    for (uint32_t i = 0; i < options->workers; ++i) {
        const struct write_worker *worker = &workers[i];
        if (worker->finished_ns > finished)
            finished = worker->finished_ns;
        usage.cpu_ns += worker->usage.cpu_ns;
        usage.minor_faults += worker->usage.minor_faults;
        usage.major_faults += worker->usage.major_faults;
        usage.has_faults = usage.has_faults && worker->usage.has_faults;
        for (uint32_t j = 0; j < worker->qps; ++j) {
            if (run->sender) {
                const struct roce_requester *requester = &worker->states[j].requester;
                retries += requester->retries;
                packets += requester->total;
            } else {
                const struct roce_responder *responder = &worker->states[j].responder;
                if (responder->msn != options->writes || responder->remaining ||
                    responder->bytes_received != bytes / options->qps) {
                    fprintf(stderr, "QP %06x ended with %u WRITEs and %" PRIu64 " packets\n",
                            responder->qp.local_qpn, responder->msn, responder->accepted);
                    return false;
                }
                packets += responder->accepted;
            }
        }
    }

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
        report_bandwidth(run->sender, bytes, started, finished - started, &usage, options);
    return true;
}

static int run(int fd, uint16_t port, struct rte_mempool *pool,
               const struct roce_lab_offer *local, const struct roce_lab_offer *peer,
               const struct roce_lab_options *lab, uint8_t *buffer,
               const struct roce_app_memory *application, bool steer,
               const struct datapath_options *options, struct tx_memory_ref *tx_refs,
               struct tx_regions *regions, struct roce_cm_port *cm)
{
    struct write_run run = { .options = options, .pool = pool, .port = port, .sender = lab->sender,
                             .state = WRITE_WAITING, .work = application->work,
                             .native = lab->sender ? application->native : NULL,
                             .local_iova = local->region.address, .local_buffer = buffer,
                             .local_length = application->length, .local_lkey = application->mr.lkey,
                             .application_socket = application->socket,
                             .tx_refs = tx_refs, .regions = regions, .cm = cm,
                             .receive_anchor = !lab->sender && lab->multi_mr_targets ? &application->mr : NULL };
    if (run.native) {
        run.native_writes = rte_zmalloc(NULL, options->qps * sizeof(*run.native_writes), RTE_CACHE_LINE_SIZE);
        if (!run.native_writes) {
            fprintf(stderr, "could not allocate native WRITEs\n");
            return 1;
        }
    }
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
    bool ready;
    if (cm)
        ready = !options->start_gate || roce_lab_wait_start_gate((uint16_t)options->start_gate, lab->control_port);
    else if (options->start_gate)
        ready = roce_lab_ready_after_gate(fd, (uint16_t)options->start_gate, lab->control_port);
    else
        ready = roce_lab_ready(fd);
    if (ready) {
        started = roce_now_ns();
        if (options->report_bandwidth)
            workers[0].initial_usage = thread_usage();
        atomic_store_explicit(&run.state, WRITE_RUNNING, memory_order_release);
        completed = wait_for_completion(fd, &workers[0], started);
        sample_usage(&workers[0]);
    } else {
        fprintf(stderr, "control readiness exchange%s failed\n", options->start_gate ? " or start gate" : "");
    }
    result = !completed;

stop_workers:
    atomic_store_explicit(&run.state, WRITE_STOPPED, memory_order_release);
    for (uint32_t i = 1; i <= launched; ++i)
        if (rte_eal_wait_lcore(options->lcores[i]) != 0)
            result = 1;

    if (run.receive_anchor)
        for (uint32_t i = 0; i < options->workers; ++i)
            for (uint32_t j = 0; j < workers[i].qps; ++j)
                roce_responder_release_target(&workers[i].states[j].responder);

    if (!result) {
        uint64_t bytes = local->region.size * options->writes * options->qps;
        result = !verify_result(fd, application->socket, workers, started, buffer, bytes);
    }
    if (result)
        fprintf(stderr, "WRITE or verification exchange did not complete\n");

cleanup_flows:
    for (uint32_t i = 0; i < options->qps; ++i)
        if (flows[i])
            rte_flow_destroy(port, flows[i], NULL);
    rte_free(run.native_writes);
    return result;
}

static bool parse_datapath_options(int argc, char **argv, bool sender, struct datapath_options *options)
{
    *options = (struct datapath_options) {
        .send_window = ROCE_SEND_WINDOW_DEFAULT,
        .tx_burst = MAX_TX_BURST,
        .writes = 1,
        .qps = 1,
        .workers = 1,
    };
    const char *tx_mode = NULL;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--report-bw") == 0) {
            options->report_bandwidth = true;
        } else if (strcmp(argv[i], "--prefault-app") == 0) {
            options->prefault_app = true;
        } else if (strcmp(argv[i], "--scatter-gather") == 0) {
            tx_mode = "scatter-gather";
        } else if (strcmp(argv[i], "--tx-mode") == 0) {
            if (++i == argc)
                return false;
            tx_mode = argv[i];
        } else if (strcmp(argv[i], "--verbs-queues") == 0) {
            options->native_queues = true;
        } else if (strcmp(argv[i], "--two-sge-writes") == 0) {
            options->two_sge_writes = true;
        } else if (strcmp(argv[i], "--multi-mr-writes") == 0) {
            options->two_sge_writes = true;
            options->multi_mr_writes = true;
        } else if (strcmp(argv[i], "--multi-mr-targets") == 0) {
            options->multi_mr_targets = true;
        } else if (strcmp(argv[i], "--tap-mtu") == 0) {
            if (++i == argc || options->tap_mtu ||
                !roce_parse_uint(argv[i], UINT16_MAX - RTE_ETHER_HDR_LEN, &options->tap_mtu) || !options->tap_mtu)
                return false;
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
        } else if (strcmp(argv[i], "--rdmacm-socket") == 0) {
            if (++i == argc || !argv[i][0] || options->rdmacm_socket)
                return false;
            options->rdmacm_socket = argv[i];
        } else {
            return false;
        }
    }
    if (!tx_mode && sender)
        tx_mode = getenv("ROCE_DPDK_TX_MODE");
    if (!tx_mode) {
        options->scatter_gather = sender && options->app_socket;
    } else if (strcmp(tx_mode, "scatter-gather") == 0) {
        options->scatter_gather = true;
    } else if (strcmp(tx_mode, "copy") != 0) {
        fprintf(stderr, "TX mode must be copy or scatter-gather (--tx-mode or ROCE_DPDK_TX_MODE)\n");
        return false;
    }
    return true;
}

static bool connect_application(struct roce_app_memory *application, const struct roce_lab_options *options,
                                const struct datapath_options *datapath, const struct roce_lab_offer *local,
                                const struct roce_lab_offer *peer)
{
    bool connected = !datapath->native_queues || application->native ?
        !datapath->cm_peer.ipv4 || roce_app_connected(application, options) :
        roce_app_offer_native(application, options, local, peer, true, NULL);
    return connected && (!options->sender || !options->multi_mr_targets ||
                         roce_app_send_auxiliary(application, options, peer));
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
        !parse_datapath_options(argc - 8, argv + 8, options.sender, &datapath)) {
        fprintf(stderr, "usage: roce_write EAL_ARGS -- tx|rx BYTES CONTROL_IP TCP_PORT DATA_IP PMTU DPDK_PORT"
                        " [--report-bw] [--tx-burst N] [--send-window N] [--writes N] [--qps N]"
                        " [--workers N] [--queue-window N] [--start-gate PORT] [--app-socket PATH]"
                        " [--mr-socket PATH] [--app-post-batch N] [--verbs-queues] [--two-sge-writes] [--multi-mr-writes]"
                        " [--multi-mr-targets]"
                        " [--prefault-app]"
                        " [--tx-mode copy|scatter-gather] [--scatter-gather]"
                        " [--tap-mtu N] [--cm-peer IPV4 MAC] [--rdmacm-socket PATH]\n");
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
    if (datapath.prefault_app && !datapath.app_socket) {
        fprintf(stderr, "prefaulting requires an application-backed buffer\n");
        goto cleanup_eal;
    }
    if (datapath.native_queues && (!datapath.mr_socket || !options.sender)) {
        fprintf(stderr, "native requester queues require a requester MR socket\n");
        goto cleanup_eal;
    }
    if (datapath.two_sge_writes && (!datapath.native_queues || options.size < 2)) {
        fprintf(stderr, "two-SGE WRITEs require a native requester and at least two bytes per WRITE\n");
        goto cleanup_eal;
    }
    if (datapath.multi_mr_targets && (!datapath.mr_socket || datapath.writes < 2 ||
                                      (options.sender && !datapath.native_queues))) {
        fprintf(stderr, "multiple targets require application MRs, at least two WRITEs, and native requester queues\n");
        goto cleanup_eal;
    }
    if (datapath.rdmacm_socket &&
        (!datapath.app_socket || !datapath.mr_socket || !datapath.cm_peer.ipv4 ||
         (options.sender && !datapath.native_queues) ||
         strcmp(datapath.rdmacm_socket, datapath.app_socket) == 0 ||
         strcmp(datapath.rdmacm_socket, datapath.mr_socket) == 0)) {
        fprintf(stderr, "librdmacm needs separate app, MR and CM sockets, and native queues on the requester\n");
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
    options.multi_mr_targets = datapath.multi_mr_targets;
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
    bool tap = strcmp(info.driver_name, "net_tap") == 0;
    bool mlx5 = strcmp(info.driver_name, "mlx5_pci") == 0;
    bool dma_gather = datapath.scatter_gather && mlx5;
    if (datapath.tap_mtu && !tap) {
        fprintf(stderr, "--tap-mtu requires a net_tap port\n");
        goto cleanup_eal;
    }
    if (datapath.tap_mtu)
        mtu = (uint16_t)datapath.tap_mtu;
    if (datapath.scatter_gather &&
        (!options.sender || !datapath.app_socket || !(info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MULTI_SEGS) ||
         (!memif && !tap && !mlx5) || (dma_gather && rte_eal_iova_mode() != RTE_IOVA_VA))) {
        fprintf(stderr, "scatter/gather requires an application-backed sender and memif, TAP, or mlx5 with IOVA=VA\n");
        goto cleanup_eal;
    }
    if (datapath.two_sge_writes && dma_gather &&
        ((info.tx_desc_lim.nb_seg_max && info.tx_desc_lim.nb_seg_max < 4) ||
         (info.tx_desc_lim.nb_mtu_seg_max && info.tx_desc_lim.nb_mtu_seg_max < 4))) {
        fprintf(stderr, "two-SGE scatter/gather WRITEs require four TX segments\n");
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
    struct tx_regions extra_regions = { .mutex = PTHREAD_MUTEX_INITIALIZER, .port = port,
                                        .anchor = &application.mr };
    struct tx_regions *regions = datapath.multi_mr_writes ? &extra_regions : NULL;
    struct roce_cm_port cm = {0};
    struct roce_cma_service cm_service = { .listener = -1, .socket = -1 };
    struct roce_cma_service *cma = datapath.rdmacm_socket ? &cm_service : NULL;
    struct roce_mr_server *registrations = NULL;
    void *mlx5_pmd = NULL;
    uint8_t *buffer = datapath.app_socket ? NULL : malloc((size_t)bytes);
    uint32_t tx_ref_count = datapath.scatter_gather ?
        (datapath.native_queues ? options.qps * ROCE_NATIVE_QUEUE_DEPTH : options.workers) : 0;
    struct tx_memory_ref *tx_refs = tx_ref_count ?
        rte_zmalloc(NULL, tx_ref_count * sizeof(*tx_refs), RTE_CACHE_LINE_SIZE) : NULL;
    if (!pool || (!datapath.app_socket && !buffer) || (tx_ref_count && !tx_refs)) {
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
    for (uint32_t i = 0; i < tx_ref_count; ++i) {
        tx_refs[i].shared.free_cb = release_tx_reference;
        rte_mbuf_ext_refcnt_set(&tx_refs[i].shared, 1);
    }
    if (datapath.tap_mtu) {
        /* Configuring the DPDK MTU does not update the Linux TAP interface. */
        err = rte_eth_dev_set_mtu(port, mtu);
        if (err) {
            fprintf(stderr, "could not set TAP MTU %u: %s\n", mtu, strerror(-err));
            goto cleanup_port;
        }
    }
    if (datapath.report_bandwidth)
        report_dpdk_rx(port, pool, (uint16_t)options.workers);
    bool in_band = datapath.cm_peer.ipv4 != 0;
    if (dma_gather && datapath.mr_socket) {
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
        extra_regions.map = dmabuf_map;
        extra_regions.unmap = dmabuf_unmap;
        if (datapath.native_queues) {
            mlx5_tx_burst_completion_fn enable_completion =
                (mlx5_tx_burst_completion_fn)dlsym(mlx5_pmd, "rte_pmd_mlx5_enable_tx_burst_completion");
            if (!enable_completion) {
                fprintf(stderr, "native scatter/gather requires mlx5 TX burst completion support\n");
                goto cleanup_port;
            }
            for (uint16_t queue = 0; queue < options.workers; ++queue) {
                err = enable_completion(port, queue);
                if (err) {
                    fprintf(stderr, "could not enable TX burst completions on queue %u: %s\n",
                            queue, rte_strerror(-err));
                    goto cleanup_port;
                }
            }
            if (datapath.report_bandwidth)
                fprintf(stderr, "DPDK_TX_BURST_COMPLETION queues=%u\n", options.workers);
        }
    }
    if (datapath.mr_socket) {
        registrations = roce_mr_server_start(datapath.mr_socket);
        if (!registrations) {
            perror("verbs memory registration listener");
            goto cleanup_port;
        }
    }
    if (cma && !roce_cma_service_listen(cma, datapath.rdmacm_socket)) {
        perror("librdmacm listener");
        goto cleanup_port;
    }
    if (datapath.app_socket) {
        uint32_t post_batch = datapath.app_post_batch ? RTE_MIN(datapath.app_post_batch, options.writes) : options.writes;
        if (datapath.native_queues)
            post_batch = RTE_MIN(post_batch, roce_native_queue_capacity(options.writes));
        const struct roce_app_config configuration = {
            .registrations = registrations,
            .post_batch = post_batch,
            .dma_writable = dma_gather,
            .in_band_cm = in_band, .scatter_gather = datapath.scatter_gather,
            .native_queues = datapath.native_queues, .two_sge_writes = datapath.two_sge_writes,
            .multi_mr_writes = datapath.multi_mr_writes,
            .rdmacm = cma != NULL,
        };
        uint64_t attached = datapath.report_bandwidth ? roce_now_ns() : 0;
        if (!roce_app_attach(datapath.app_socket, &options, &configuration, &application)) {
            perror("application memory registration");
            goto cleanup_port;
        }
        buffer = application.buffer;
        if (datapath.report_bandwidth)
            fprintf(stderr, "APP_ATTACH bytes=%" PRIu64 " elapsed_ns=%" PRIu64 "\n",
                    bytes, roce_now_ns() - attached);
    }
    if (dma_gather) {
        uint64_t mapping_start = datapath.report_bandwidth ? roce_now_ns() : 0;
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
        if (datapath.report_bandwidth)
            fprintf(stderr, "APP_DMA_SETUP bytes=%zu elapsed_ns=%" PRIu64 "\n",
                    dma_length, roce_now_ns() - mapping_start);
    }
    if (datapath.prefault_app && !prefault_application(buffer, (size_t)bytes)) {
        fprintf(stderr, "could not prefault the application buffer\n");
        goto cleanup_port;
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
    if ((!in_band && !roce_lab_random(&local.psn)) || (!cma && !roce_lab_random(&local.qpn)) ||
        (!registrations && !roce_lab_random(&local.region.rkey)))
        goto cleanup_port;
    if (registrations) {
        local.region.address = application.mr.iova;
        local.region.rkey = application.mr.rkey;
        local.auxiliary = application.auxiliary;
    }
    local.psn &= ROCE_PSN_MASK;
    local.qpn = cma ? application.qpn : 2 + local.qpn % (ROCE_PSN_MASK - options.qps - 1);

    if (cma) {
        const struct roce_cma_lab_address addresses = {
            .local_ipv4 = local.data.ipv4, .peer_ipv4 = datapath.cm_peer.ipv4,
            .service_port = options.control_port,
        };
        if (!roce_cma_service_attach(cma, application.socket, &addresses, local.qpn, options.sender)) {
            perror("librdmacm session attachment");
            goto cleanup_port;
        }
    }
    int fd = roce_lab_open(&options);
    if (fd < 0) {
        perror("control socket");
        goto cleanup_port;
    }
    bool connected = in_band ?
        roce_cm_lab_exchange(port, pool, &options, &local, &datapath.cm_peer, &peer, &cm, cma, &application) :
        roce_lab_exchange(fd, &options, &local, &peer);
    if (!connected) {
        if (in_band && cm.session.phase == ROCE_CM_REJECTED) {
            fprintf(stderr, "RoCEv2 CM rejected reason=%u\n", cm.session.reject_reason);
        } else {
            fprintf(stderr, "%s setup failed\n", in_band ? "RoCEv2 CM" : "control");
            if (cma)
                roce_cma_service_event(cma, ROCE_CMA_CONNECT_ERROR,
                                       cm.session.phase == ROCE_CM_FAILED ? ETIMEDOUT : EPROTO, NULL, 0);
        }
    } else if (application.socket >= 0 && !connect_application(&application, &options, &datapath, &local, &peer)) {
        perror("application establishment");
    } else {
        if (in_band)
            printf("RoCEv2 CM established local_qpn=%06x peer_qpn=%06x\n", local.qpn, peer.qpn);
        result = run(fd, port, pool, &local, &peer, &options, buffer, &application,
                     options.workers > 1 && !memif, &datapath, tx_refs, regions,
                     in_band ? &cm : NULL);
        if (!result && in_band) {
            if (roce_cm_lab_close(port, pool, &cm, cma)) {
                puts("RoCEv2 CM disconnected");
            } else {
                fprintf(stderr, "RoCEv2 CM disconnect failed\n");
                if (cma)
                    roce_cma_service_event(cma, ROCE_CMA_DISCONNECTED,
                                           cm.session.disconnect_error ? cm.session.disconnect_error : EPROTO, NULL, 0);
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
    for (uint16_t i = 0; i < cm.early_count; ++i)
        rte_pktmbuf_free(cm.early[i]);
    if (datapath.scatter_gather) {
        err = rte_eth_dev_stop(port);
        if (err) {
            fprintf(stderr, "could not stop scatter/gather port: %s\n", rte_strerror(-err));
            return 1;
        }
        for (uint32_t i = 0; i < tx_ref_count; ++i) {
            if (rte_mbuf_ext_refcnt_read(&tx_refs[i].shared) != 1) {
                fprintf(stderr, "TX reference %u still retains application buffers\n", i);
                return 1;
            }
        }
    }
    if (regions) {
        for (size_t i = 0; i < RTE_DIM(regions->entries); ++i)
            if (!unmap_tx_region(regions, &regions->entries[i])) {
                fprintf(stderr, "could not release a native TX mapping after port shutdown\n");
                return 1;
            }
    }
    if (datapath.scatter_gather) {
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
        roce_app_abort(&application, &options);
cleanup_buffers:
    pthread_mutex_destroy(&extra_regions.mutex);
    rte_free(tx_refs);
    if (cma)
        roce_cma_service_close(cma);
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
