#include "roce_lab.h"
#include "../../mrc-dpdk/src/dpdk_io.h"

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_pause.h>

#include <inttypes.h>
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
    uint32_t tx_burst;
    uint32_t writes;
    uint32_t qps;
    uint32_t workers;
    unsigned int lcores[ROCE_LAB_MAX_QPS];
    bool report_bandwidth;
};

union write_qp {
    struct roce_requester requester;
    struct roce_responder responder;
};

enum run_state { WRITE_WAITING, WRITE_RUNNING, WRITE_STOPPED };

struct write_run {
    const struct datapath_options *options;
    struct rte_mempool *pool;
    uint16_t port;
    bool sender;
    atomic_int state;
    atomic_uint finished_workers;
    atomic_bool failed;
};

struct write_worker {
    struct write_run *run;
    uint16_t queue;
    uint32_t base_qpn;
    uint32_t qps;
    uint32_t completed;
    uint32_t next;
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

static bool send_requests(const struct write_worker *worker,
                          struct roce_requester *requester, uint64_t now)
{
    const struct write_run *run = worker->run;
    struct rte_mbuf *packets[MAX_TX_BURST];
    struct roce_packet packet;
    uint16_t count = 0;
    while (count < run->options->tx_burst && roce_requester_next(requester, count, &packet)) {
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

static void report_bandwidth(bool sender, uint64_t bytes, uint64_t elapsed, uint64_t cpu,
                             const struct datapath_options *options)
{
    if (!elapsed)
        return;
    printf("BANDWIDTH role=%s payload_bytes=%" PRIu64 " elapsed_ns=%" PRIu64
           " thread_cpu_ns=%" PRIu64 " payload_gbps=%.3f poll_cores=%.3f writes=%u qps=%u workers=%u",
           sender ? "tx" : "rx", bytes, elapsed, cpu,
           (double)bytes * 8.0 / (double)elapsed, (double)cpu / (double)elapsed,
           options->writes, options->qps, options->workers);
    if (sender)
        printf(" send_window=%u tx_burst=%u", options->send_window, options->tx_burst);
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
        uint32_t psn_offset = index * UINT32_C(0x100001);
        if (run->sender) {
            struct roce_region remote = peer->region;
            remote.address += offset;
            if (!roce_requester_init(&worker->states[i].requester, &qp,
                                     (local->psn + psn_offset) & ROCE_PSN_MASK,
                                     remote, buffer + offset, ROCE_LAB_RETRIES,
                                     options->send_window, options->writes))
                return false;
        } else {
            worker->states[i].responder = (struct roce_responder) {
                .qp = qp, .buffer = buffer + offset,
                .expected = (peer->psn + psn_offset) & ROCE_PSN_MASK,
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

static bool poll_worker(struct write_worker *worker, uint64_t now)
{
    struct write_run *run = worker->run;
    if (run->sender && worker->finished)
        return true;
    if (!receive_packets(worker, now))
        return false;
    if (worker->completed == worker->qps && !worker->finished) {
        worker->finished = true;
        if (run->options->report_bandwidth)
            worker->finished_ns = roce_now_ns();
        atomic_fetch_add_explicit(&run->finished_workers, 1, memory_order_relaxed);
    }
    if (!run->sender || worker->finished)
        return true;

    for (uint32_t i = 0; i < worker->qps; ++i) {
        struct roce_requester *requester = &worker->states[i].requester;
        roce_requester_poll_timer(requester, now, RETRY_NS);
        if (requester->failed) {
            fprintf(stderr, "QP %06x WRITE failed after %u retries (%u/%u packets acknowledged)\n",
                    requester->qp.local_qpn, requester->retries, requester->acked, requester->total);
            return false;
        }
    }
    for (uint32_t i = 0; i < worker->qps; ++i) {
        struct roce_requester *requester = &worker->states[worker->next].requester;
        if (++worker->next == worker->qps)
            worker->next = 0;
        if (send_requests(worker, requester, now))
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
        if (run->sender) {
            if (all_workers_finished(run))
                return true;
        } else {
            if (main_worker->finished && all_workers_finished(run))
                sample_cpu(main_worker);
            if (now >= control_poll) {
                control_poll = now + CONTROL_POLL_NS;
                int done = roce_lab_receive(fd, 'D', true);
                if (done)
                    return done == 1;
            }
        }
        rte_pause();
    }
    return false;
}

static bool verify_result(int fd, const struct write_worker *workers, uint64_t started,
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
        if (!roce_lab_verify(buffer, (size_t)bytes) || !roce_lab_send(fd, 'V'))
            return false;
        printf("WRITE verified: %" PRIu64 " bytes, %" PRIu64 " packets\n", bytes, packets);
    }
    if (options->report_bandwidth)
        report_bandwidth(run->sender, bytes, finished - started, cpu, options);
    return true;
}

static int run(int fd, uint16_t port, struct rte_mempool *pool,
               const struct roce_lab_offer *local, const struct roce_lab_offer *peer,
               bool sender, uint8_t *buffer, bool steer, const struct datapath_options *options)
{
    struct write_run run = { .options = options, .pool = pool, .port = port, .sender = sender,
                             .state = WRITE_WAITING };
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
    if (roce_lab_ready(fd)) {
        started = roce_now_ns();
        workers[0].cpu_started = options->report_bandwidth ? thread_cpu_ns() : 0;
        atomic_store_explicit(&run.state, WRITE_RUNNING, memory_order_release);
        completed = wait_for_completion(fd, &workers[0], started);
        sample_cpu(&workers[0]);
    } else {
        fprintf(stderr, "control readiness exchange failed\n");
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
        result = !verify_result(fd, workers, started, buffer, bytes, qp_packets);
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
        } else if (strcmp(argv[i], "--tx-burst") == 0) {
            if (++i == argc || !roce_parse_uint(argv[i], MAX_TX_BURST, &options->tx_burst) ||
                !options->tx_burst)
                return false;
        } else if (strcmp(argv[i], "--send-window") == 0) {
            if (++i == argc ||
                !roce_parse_uint(argv[i], ROCE_SEND_WINDOW_MAX, &options->send_window) ||
                !options->send_window)
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
        } else {
            return false;
        }
    }
    return true;
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
                        " [--report-bw] [--tx-burst N] [--send-window N] [--writes N] [--qps N] [--workers N]\n");
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
    uint8_t *buffer = malloc((size_t)bytes);
    if (!pool || !buffer) {
        fprintf(stderr, "packet pool or host buffer allocation failed\n");
        goto cleanup_buffers;
    }
    roce_lab_fill(buffer, (size_t)bytes, options.sender);

    struct rte_ether_addr mac;
    int err = mrc_dpdk_open_port_queues(port, pool, &mac, mtu, (uint16_t)options.workers);
    if (err) {
        fprintf(stderr, "could not open DPDK port %u: %s\n", port, strerror(-err));
        goto cleanup_buffers;
    }
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
        !roce_lab_random(&local.region.rkey))
        goto cleanup_port;
    local.psn &= ROCE_PSN_MASK;
    local.qpn = 2 + local.qpn % (ROCE_PSN_MASK - options.qps - 1);

    int fd = roce_lab_open(&options);
    if (fd < 0) {
        perror("control socket");
        goto cleanup_port;
    }
    if (roce_lab_exchange(fd, &options, &local, &peer)) {
        result = run(fd, port, pool, &local, &peer, options.sender, buffer,
                     options.workers > 1 && !memif, &datapath);
    } else {
        fprintf(stderr, "control setup failed\n");
    }
    if (result) {
        struct rte_eth_stats stats;
        if (rte_eth_stats_get(port, &stats) == 0)
            fprintf(stderr, "DPDK port %u RX=%" PRIu64 " TX=%" PRIu64
                    " missed=%" PRIu64 " RX errors=%" PRIu64 " TX errors=%" PRIu64 "\n",
                    port, stats.ipackets, stats.opackets, stats.imissed, stats.ierrors, stats.oerrors);
    }
    close(fd);
cleanup_port:
    mrc_dpdk_close_port(port);
cleanup_buffers:
    free(buffer);
    if (pool)
        rte_mempool_free(pool);
cleanup_eal:
    rte_eal_cleanup();
    return result;
}
