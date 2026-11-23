#ifndef MRC_DPDK_IO_H
#define MRC_DPDK_IO_H

#include <stdbool.h>
#include <stdint.h>

struct rte_ether_addr;
struct rte_mbuf;
struct rte_mempool;

struct rte_mempool *mrc_dpdk_packet_pool(void);
/* frame_length includes the Ethernet header. The pool reserves space for the FCS. */
struct rte_mempool *mrc_dpdk_packet_pool_for_frame(uint16_t frame_length);
struct rte_mempool *mrc_dpdk_packet_pool_for_ports(uint16_t frame_length, unsigned int port_count);
struct rte_mempool *mrc_dpdk_packet_pool_with_cache(uint16_t frame_length, unsigned int rx_queues,
                                                  unsigned int cache_size);
int mrc_dpdk_open_port(uint16_t port, struct rte_mempool *pool,
                      struct rte_ether_addr *mac);
int mrc_dpdk_open_port_with_mtu(uint16_t port, struct rte_mempool *pool,
                               struct rte_ether_addr *mac, uint16_t mtu);
int mrc_dpdk_open_port_queues(uint16_t port, struct rte_mempool *pool,
                             struct rte_ether_addr *mac, uint16_t mtu, uint16_t queues);
int mrc_dpdk_open_port_with_offloads(uint16_t port, struct rte_mempool *pool,
                                    struct rte_ether_addr *mac, uint16_t mtu, uint16_t queues,
                                    uint64_t tx_offloads);
void mrc_dpdk_close_port(uint16_t port);

/* Consumes the mbuf whether or not the driver accepts it. */
bool mrc_dpdk_transmit(uint16_t port, struct rte_mbuf *packet);

#endif
