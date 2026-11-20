#ifndef MRC_PORT_NETLINK_H
#define MRC_PORT_NETLINK_H

#include <stdbool.h>
#include <stdint.h>

/* Look up the primary netdev for a Linux RDMA device and 1-based port. */
int mrc_port_netlink_rdma_index(uint32_t dev_index, uint8_t port_num, uint32_t *ifindex);
/* Read the driver ID reported by the RDMA device's uverbs character device. */
int mrc_port_netlink_uverbs_driver_id(uint32_t dev_index, uint32_t *driver_id);

/* Read or change only the netdev's administrative IFF_UP flag.
 * All functions return zero on success or a negative errno. */
int mrc_port_netlink_query(uint32_t ifindex, bool *enabled);
int mrc_port_netlink_set(uint32_t ifindex, bool enabled);

#endif
