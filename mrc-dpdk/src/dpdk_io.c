#include "dpdk_io.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>

enum {
    DEFAULT_PACKET_COUNT = 8191,
    DEFAULT_POOL_PORT_COUNT = 2,
    RX_DESCRIPTORS = 1024,
};

struct rte_mempool *mrc_dpdk_packet_pool(void)
{
    return mrc_dpdk_packet_pool_for_frame(0);
}

struct rte_mempool *mrc_dpdk_packet_pool_for_frame(uint16_t frame_length)
{
    return mrc_dpdk_packet_pool_for_ports(frame_length, DEFAULT_POOL_PORT_COUNT);
}

struct rte_mempool *mrc_dpdk_packet_pool_for_ports(uint16_t frame_length, unsigned int port_count)
{
    unsigned int room = frame_length + RTE_PKTMBUF_HEADROOM;
    if (room > UINT16_MAX || !port_count)
        return NULL;
    if (room < RTE_MBUF_DEFAULT_BUF_SIZE)
        room = RTE_MBUF_DEFAULT_BUF_SIZE;
    unsigned int packets = DEFAULT_PACKET_COUNT;
    if (port_count > DEFAULT_POOL_PORT_COUNT) {
        unsigned int extra_ports = port_count - DEFAULT_POOL_PORT_COUNT;
        if (extra_ports > (UINT_MAX - packets) / RX_DESCRIPTORS)
            return NULL;
        packets += extra_ports * RX_DESCRIPTORS;
    }
    return rte_pktmbuf_pool_create("mrc_packets", packets, 0, 0, (uint16_t)room, SOCKET_ID_ANY);
}

int mrc_dpdk_open_port(uint16_t port, struct rte_mempool *pool, struct rte_ether_addr *mac)
{
    return mrc_dpdk_open_port_with_mtu(port, pool, mac, 0);
}

int mrc_dpdk_open_port_with_mtu(uint16_t port, struct rte_mempool *pool,
                               struct rte_ether_addr *mac, uint16_t mtu)
{
    if (!rte_eth_dev_is_valid_port(port))
        return -ENODEV;
    struct rte_eth_conf config = { .rxmode.mtu = mtu };
    int result = rte_eth_dev_configure(port, 1, 1, &config);
    if (result)
        return result;

    uint16_t rx_desc = RX_DESCRIPTORS, tx_desc = 1024;
    result = rte_eth_dev_adjust_nb_rx_tx_desc(port, &rx_desc, &tx_desc);
    int socket = rte_eth_dev_socket_id(port);
    if (!result)
        result = rte_eth_rx_queue_setup(port, 0, rx_desc, socket, NULL, pool);
    if (!result)
        result = rte_eth_tx_queue_setup(port, 0, tx_desc, socket, NULL);
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

void mrc_dpdk_close_port(uint16_t port)
{
    rte_eth_dev_stop(port);
    rte_eth_dev_close(port);
}

bool mrc_dpdk_transmit(uint16_t port, struct rte_mbuf *packet)
{
    if (rte_eth_tx_burst(port, 0, &packet, 1) == 1)
        return true;

    rte_pktmbuf_free(packet);
    return false;
}
