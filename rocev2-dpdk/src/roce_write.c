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
#include <unistd.h>

enum { RX_BURST = 32, NS_PER_SEC = 1000000000, RETRY_NS = 10000000, CONTROL_POLL_NS = 1000000 };

static bool send_packet(uint16_t port, struct rte_mempool *pool,
                        const struct roce_packet *packet)
{
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(pool);
    if (!mbuf)
        return false;
    size_t length = roce_encode(rte_pktmbuf_mtod(mbuf, uint8_t *),
                               rte_pktmbuf_tailroom(mbuf), packet);
    if (!length || !rte_pktmbuf_append(mbuf, (uint16_t)length)) {
        rte_pktmbuf_free(mbuf);
        return false;
    }
    return mrc_dpdk_transmit(port, mbuf);
}

static int run(int fd, uint16_t port, struct rte_mempool *pool, const struct roce_qp *qp,
               const struct roce_lab_offer *local, const struct roce_lab_offer *peer,
               bool sender, uint8_t *buffer)
{
    struct roce_requester requester;
    struct roce_responder responder = {
        .qp = *qp, .region = local->region, .buffer = buffer, .expected = peer->psn,
    };
    if (sender && !roce_requester_init(&requester, qp, local->psn, peer->region,
                                      buffer, ROCE_LAB_RETRIES))
        return 1;

    uint64_t now = roce_now_ns();
    uint64_t deadline = now + (uint64_t)ROCE_LAB_TIMEOUT * NS_PER_SEC;
    uint64_t control_poll = now;
    for (; now < deadline; now = roce_now_ns()) {
        struct rte_mbuf *received[RX_BURST];
        uint16_t count = rte_eth_rx_burst(port, 0, received, RX_BURST);
        for (uint16_t i = 0; i < count; ++i) {
            uint8_t storage[ROCE_MAX_FRAME];
            uint32_t length = rte_pktmbuf_pkt_len(received[i]);
            const void *frame = length <= sizeof(storage) ?
                rte_pktmbuf_read(received[i], 0, length, storage) : NULL;
            struct roce_packet packet;
            if (frame && roce_decode(&packet, frame, length)) {
                if (sender) {
                    roce_requester_feedback(&requester, &packet, now);
                } else {
                    struct roce_packet feedback;
                    if (roce_responder_receive(&responder, &packet, &feedback))
                        send_packet(port, pool, &feedback);
                }
            }
            rte_pktmbuf_free(received[i]);
        }

        if (sender) {
            roce_requester_poll_timer(&requester, now, RETRY_NS);
            if (requester.failed) {
                fprintf(stderr, "WRITE failed after %u retries (%u/%u packets acknowledged)\n",
                        requester.retries, requester.acked, requester.total);
                return 1;
            }
            if (requester.acked == requester.total) {
                if (!roce_lab_send(fd, 'D') || roce_lab_receive(fd, 'V', false) != 1)
                    break;
                printf("WRITE complete: %u bytes, %u packets, %u retries, peer verified\n",
                       local->region.size, requester.total, requester.retries);
                return 0;
            }
            struct roce_packet packet;
            while (roce_requester_next(&requester, &packet) && send_packet(port, pool, &packet))
                roce_requester_sent(&requester, now);
        } else if (now >= control_poll || responder.failed) {
            if (responder.failed) {
                fprintf(stderr, "WRITE failed responder validation at PSN %06x\n", responder.expected);
                return 1;
            }
            control_poll = now + CONTROL_POLL_NS;
            int done = roce_lab_receive(fd, 'D', true);
            if (done < 0)
                break;
            if (done > 0) {
                if (responder.msn != 1 || responder.remaining ||
                    !roce_lab_verify(buffer, local->region.size) || !roce_lab_send(fd, 'V'))
                    return 1;
                printf("WRITE verified: %u bytes, %u packets\n", local->region.size, responder.accepted);
                return 0;
            }
        }
        rte_pause();
    }
    fprintf(stderr, "WRITE or verification exchange did not complete\n");
    return 1;
}

int main(int argc, char **argv)
{
    int consumed = rte_eal_init(argc, argv);
    if (consumed < 0)
        return 1;
    argc -= consumed;
    argv += consumed;

    struct roce_lab_options options;
    int result = 1;
    uint16_t port = RTE_MAX_ETHPORTS;
    if (argc != 8 || !roce_lab_parse(argv, &options)) {
        fprintf(stderr, "usage: roce_write EAL_ARGS -- tx|rx BYTES CONTROL_IP TCP_PORT DATA_IP PMTU DPDK_PORT\n");
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
    uint8_t *buffer = malloc(options.size);
    if (!pool || !buffer) {
        fprintf(stderr, "packet pool or host buffer allocation failed\n");
        goto cleanup_buffers;
    }
    roce_lab_fill(buffer, options.size, options.sender);

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
    local.qpn = 2 + local.qpn % (ROCE_PSN_MASK - 2);

    int fd = roce_lab_open(&options);
    if (fd < 0) {
        perror("control socket");
        goto cleanup_port;
    }
    if (roce_lab_exchange(fd, &options, &local, &peer) && roce_lab_ready(fd)) {
        struct roce_qp qp = {
            .local = local.data, .peer = peer.data, .local_qpn = local.qpn,
            .peer_qpn = peer.qpn, .pmtu = local.pmtu < peer.pmtu ? local.pmtu : peer.pmtu,
        };
        result = run(fd, port, pool, &qp, &local, &peer, options.sender, buffer);
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
