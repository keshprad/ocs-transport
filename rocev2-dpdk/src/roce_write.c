#include "roce_lab.h"
#include "../../mrc-dpdk/src/dpdk_io.h"

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_pause.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    RX_BURST = 32, MAX_TX_BURST = 32, NS_PER_SEC = 1000000000,
    RETRY_NS = 10000000, CONTROL_POLL_NS = 1000000,
};

struct datapath_options {
    uint32_t send_window;
    uint32_t tx_burst;
    uint32_t writes;
    uint32_t qps;
    bool report_bandwidth;
};

union write_qp {
    struct roce_requester requester;
    struct roce_responder responder;
};

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

static bool send_requests(uint16_t port, struct rte_mempool *pool,
                          struct roce_requester *requester, uint16_t max_burst, uint64_t now)
{
    struct rte_mbuf *packets[MAX_TX_BURST];
    struct roce_packet packet;
    uint16_t count = 0;
    while (count < max_burst && roce_requester_next(requester, count, &packet)) {
        packets[count] = encode_packet(pool, &packet);
        if (!packets[count])
            break;
        ++count;
    }
    if (!count)
        return false;
    uint16_t sent = rte_eth_tx_burst(port, 0, packets, count);
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
           " thread_cpu_ns=%" PRIu64 " payload_gbps=%.3f poll_cores=%.3f writes=%u qps=%u",
           sender ? "tx" : "rx", bytes, elapsed, cpu,
           (double)bytes * 8.0 / (double)elapsed, (double)cpu / (double)elapsed,
           options->writes, options->qps);
    if (sender)
        printf(" send_window=%u tx_burst=%u", options->send_window, options->tx_burst);
    putchar('\n');
}

static bool init_qps(union write_qp *states, uint16_t pmtu,
                     const struct roce_lab_offer *local, const struct roce_lab_offer *peer,
                     bool sender, uint8_t *buffer, const struct datapath_options *options)
{
    uint64_t qp_bytes = local->region.size * options->writes;
    for (uint32_t i = 0; i < options->qps; ++i) {
        struct roce_qp qp = {
            .local = local->data, .peer = peer->data, .pmtu = pmtu,
            .local_qpn = local->qpn + i, .peer_qpn = peer->qpn + i,
        };
        uint64_t offset = qp_bytes * i;
        uint32_t psn_offset = i * UINT32_C(0x100001);
        if (sender) {
            struct roce_region remote = peer->region;
            remote.address += offset;
            if (!roce_requester_init(&states[i].requester, &qp,
                                     (local->psn + psn_offset) & ROCE_PSN_MASK,
                                     remote, buffer + offset, ROCE_LAB_RETRIES,
                                     options->send_window, options->writes))
                return false;
        } else {
            states[i].responder = (struct roce_responder) {
                .qp = qp, .buffer = buffer + offset,
                .expected = (peer->psn + psn_offset) & ROCE_PSN_MASK,
                .region = { .address = local->region.address + offset,
                            .rkey = local->region.rkey, .size = qp_bytes },
            };
        }
    }
    return true;
}

static bool receive_packets(uint16_t port, struct rte_mempool *pool, union write_qp *states,
                            uint32_t base_qpn, bool sender, uint64_t now, uint32_t *completed,
                            const struct datapath_options *options)
{
    struct rte_mbuf *received[RX_BURST];
    uint16_t count = rte_eth_rx_burst(port, 0, received, RX_BURST);
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t storage[ROCE_MAX_FRAME];
        uint32_t length = rte_pktmbuf_pkt_len(received[i]);
        const void *frame = length <= sizeof(storage) ?
            rte_pktmbuf_read(received[i], 0, length, storage) : NULL;
        struct roce_packet packet;
        if (frame && roce_decode(&packet, frame, length)) {
            uint32_t index = packet.qpn - base_qpn;
            if (index < options->qps) {
                if (sender) {
                    struct roce_requester *requester = &states[index].requester;
                    bool pending = requester->acked < requester->total;
                    roce_requester_feedback(requester, &packet, now);
                    *completed += pending && requester->acked == requester->total;
                } else {
                    struct roce_responder *responder = &states[index].responder;
                    bool pending = responder->msn < options->writes;
                    struct roce_packet feedback;
                    if (roce_responder_receive(responder, &packet, &feedback)) {
                        struct rte_mbuf *ack = encode_packet(pool, &feedback);
                        if (ack)
                            mrc_dpdk_transmit(port, ack);
                    }
                    if (responder->failed) {
                        for (; i < count; ++i)
                            rte_pktmbuf_free(received[i]);
                        fprintf(stderr, "QP %06x WRITE failed responder validation at PSN %06x\n",
                                responder->qp.local_qpn, responder->expected);
                        return false;
                    }
                    *completed += pending && responder->msn == options->writes;
                }
            }
        }
        rte_pktmbuf_free(received[i]);
    }
    return true;
}

static int run(int fd, uint16_t port, struct rte_mempool *pool,
               const struct roce_lab_offer *local, const struct roce_lab_offer *peer,
               bool sender, uint8_t *buffer, const struct datapath_options *options)
{
    union write_qp states[ROCE_LAB_MAX_QPS];
    uint16_t pmtu = local->pmtu < peer->pmtu ? local->pmtu : peer->pmtu;
    uint64_t bytes = local->region.size * options->writes * options->qps;
    uint64_t qp_packets = ((local->region.size - 1) / pmtu + 1) * options->writes;
    uint64_t packets = qp_packets * options->qps;
    if (!init_qps(states, pmtu, local, peer, sender, buffer, options)) {
        fprintf(stderr, "WRITE sequence exceeds the requester range\n");
        return 1;
    }
    if (!roce_lab_ready(fd)) {
        fprintf(stderr, "control readiness exchange failed\n");
        return 1;
    }

    uint64_t now = roce_now_ns();
    uint64_t started = now;
    uint64_t cpu_started = options->report_bandwidth ? thread_cpu_ns() : 0;
    uint64_t elapsed = 0, cpu = 0;
    uint64_t deadline = now + (uint64_t)ROCE_LAB_TIMEOUT * NS_PER_SEC;
    uint64_t control_poll = now;
    uint32_t completed = 0, next = 0;
    for (; now < deadline; now = roce_now_ns()) {
        if (!receive_packets(port, pool, states, local->qpn, sender, now, &completed, options))
            return 1;
        if (options->report_bandwidth && !elapsed && completed == options->qps) {
            elapsed = roce_now_ns() - started;
            cpu = thread_cpu_ns() - cpu_started;
        }
        if (sender) {
            if (completed == options->qps) {
                if (!roce_lab_send(fd, 'D') || roce_lab_receive(fd, 'V', false) != 1)
                    break;
                uint64_t retries = 0;
                for (uint32_t i = 0; i < options->qps; ++i)
                    retries += states[i].requester.retries;
                printf("WRITE complete: %" PRIu64 " bytes, %" PRIu64 " packets, %" PRIu64
                       " retries, peer verified\n", bytes, packets, retries);
                if (options->report_bandwidth)
                    report_bandwidth(true, bytes, elapsed, cpu, options);
                return 0;
            }
            for (uint32_t i = 0; i < options->qps; ++i) {
                struct roce_requester *requester = &states[i].requester;
                roce_requester_poll_timer(requester, now, RETRY_NS);
                if (requester->failed) {
                    fprintf(stderr, "QP %06x WRITE failed after %u retries (%u/%u packets acknowledged)\n",
                            requester->qp.local_qpn, requester->retries, requester->acked, requester->total);
                    return 1;
                }
            }
            for (uint32_t i = 0; i < options->qps; ++i) {
                struct roce_requester *requester = &states[next].requester;
                if (++next == options->qps)
                    next = 0;
                if (send_requests(port, pool, requester, (uint16_t)options->tx_burst, now))
                    break;
            }
        } else if (now >= control_poll) {
            control_poll = now + CONTROL_POLL_NS;
            int done = roce_lab_receive(fd, 'D', true);
            if (done < 0)
                break;
            if (done > 0) {
                for (uint32_t i = 0; i < options->qps; ++i) {
                    const struct roce_responder *responder = &states[i].responder;
                    if (responder->msn != options->writes || responder->remaining ||
                        responder->accepted != qp_packets) {
                        fprintf(stderr, "QP %06x ended with %u WRITEs and %" PRIu64 " packets\n",
                                responder->qp.local_qpn, responder->msn, responder->accepted);
                        return 1;
                    }
                }
                if (!roce_lab_verify(buffer, (size_t)bytes) || !roce_lab_send(fd, 'V'))
                    return 1;
                printf("WRITE verified: %" PRIu64 " bytes, %" PRIu64 " packets\n", bytes, packets);
                if (options->report_bandwidth)
                    report_bandwidth(false, bytes, elapsed, cpu, options);
                return 0;
            }
        }
        rte_pause();
    }
    fprintf(stderr, "WRITE or verification exchange did not complete\n");
    return 1;
}

static bool parse_datapath_options(int argc, char **argv, struct datapath_options *options)
{
    *options = (struct datapath_options) {
        .send_window = ROCE_SEND_WINDOW_DEFAULT,
        .tx_burst = MAX_TX_BURST,
        .writes = 1,
        .qps = 1,
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
                        " [--report-bw] [--tx-burst N] [--send-window N] [--writes N] [--qps N]\n");
        goto cleanup_eal;
    }
    options.writes = datapath.writes;
    options.qps = datapath.qps;
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
    if (mtu < required_mtu && strcmp(info.driver_name, "net_memif") != 0) {
        fprintf(stderr, "DPDK port MTU %u is below the required %u\n", mtu, required_mtu);
        goto cleanup_eal;
    }
    if (mtu < required_mtu)
        mtu = required_mtu;
    if (mtu > UINT16_MAX - RTE_ETHER_HDR_LEN)
        goto cleanup_eal;

    uint16_t frame = mtu + RTE_ETHER_HDR_LEN;
    struct rte_mempool *pool = mrc_dpdk_packet_pool_for_ports(frame, 1);
    uint8_t *buffer = malloc((size_t)bytes);
    if (!pool || !buffer) {
        fprintf(stderr, "packet pool or host buffer allocation failed\n");
        goto cleanup_buffers;
    }
    roce_lab_fill(buffer, (size_t)bytes, options.sender);

    struct rte_ether_addr mac;
    int err = mrc_dpdk_open_port_with_mtu(port, pool, &mac, mtu);
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
        result = run(fd, port, pool, &local, &peer, options.sender, buffer, &datapath);
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
