#include "roce_dpdk_io.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>

#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

enum {
    DEFAULT_PACKET_COUNT = 8191,
    DEFAULT_RX_QUEUES = 2,
    RX_DESCRIPTORS = 1024,
};

struct rte_mempool *roce_dpdk_packet_pool(uint16_t frame_length, unsigned int rx_queues,
                                          unsigned int cache_size)
{
    /* mlx5 includes the Ethernet FCS in its single-segment RX size check. */
    unsigned int room = frame_length + RTE_ETHER_CRC_LEN + RTE_PKTMBUF_HEADROOM;
    if (room > UINT16_MAX || !rx_queues)
        return NULL;
    if (room < RTE_MBUF_DEFAULT_BUF_SIZE)
        room = RTE_MBUF_DEFAULT_BUF_SIZE;
    unsigned int packets = DEFAULT_PACKET_COUNT;
    if (rx_queues > DEFAULT_RX_QUEUES) {
        unsigned int extra_queues = rx_queues - DEFAULT_RX_QUEUES;
        if (extra_queues > (UINT_MAX - packets) / RX_DESCRIPTORS)
            return NULL;
        packets += extra_queues * RX_DESCRIPTORS;
    }
    return rte_pktmbuf_pool_create("roce_packets", packets, cache_size, 0, (uint16_t)room, SOCKET_ID_ANY);
}

int roce_dpdk_open_port(uint16_t port, struct rte_mempool *pool,
                        struct rte_ether_addr *mac, uint16_t mtu, uint16_t queues,
                        uint64_t tx_offloads)
{
    if (!rte_eth_dev_is_valid_port(port))
        return -ENODEV;
    struct rte_eth_conf config = { .rxmode.mtu = mtu, .txmode.offloads = tx_offloads };
    int result = rte_eth_dev_configure(port, queues, queues, &config);
    if (result)
        return result;

    uint16_t rx_desc = RX_DESCRIPTORS, tx_desc = 1024;
    result = rte_eth_dev_adjust_nb_rx_tx_desc(port, &rx_desc, &tx_desc);
    int socket = rte_eth_dev_socket_id(port);
    for (uint16_t i = 0; i < queues && !result; ++i) {
        result = rte_eth_rx_queue_setup(port, i, rx_desc, socket, NULL, pool);
        if (!result)
            result = rte_eth_tx_queue_setup(port, i, tx_desc, socket, NULL);
    }
    if (!result)
        result = rte_eth_macaddr_get(port, mac);
    if (!result)
        result = rte_eth_dev_start(port);
    if (result) {
        rte_eth_dev_close(port);
        return result;
    }

    char name[RTE_ETH_NAME_MAX_LEN];
    if (rte_eth_dev_get_name_by_port(port, name) == 0)
        fprintf(stderr, "DPDK port %u: %s\n", port, name);
    return 0;
}

void roce_dpdk_close_port(uint16_t port)
{
    rte_eth_dev_stop(port);
    rte_eth_dev_close(port);
}
