#ifndef ROCE_DPDK_IO_H
#define ROCE_DPDK_IO_H

#include <stdint.h>

struct rte_ether_addr;
struct rte_mempool;

/* frame_length includes the Ethernet header. The pool reserves space for the FCS. */
struct rte_mempool *roce_dpdk_packet_pool(uint16_t frame_length, unsigned int rx_queues,
                                          unsigned int cache_size);
int roce_dpdk_open_port(uint16_t port, struct rte_mempool *pool,
                        struct rte_ether_addr *mac, uint16_t mtu, uint16_t queues,
                        uint64_t tx_offloads);
void roce_dpdk_close_port(uint16_t port);

#endif
