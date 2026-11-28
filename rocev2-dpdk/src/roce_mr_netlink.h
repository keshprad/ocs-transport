#ifndef ROCE_MR_NETLINK_H
#define ROCE_MR_NETLINK_H

#include "../kernel/roce_mr_uapi.h"

#include <stdint.h>

struct roce_mr_netlink {
    int fd;
    uint32_t portid;
    uint16_t family;
};

struct roce_mr_ticket_notification {
    uint64_t cookie;
    struct roce_mr_ticket ticket;
};

/* On success, the caller owns a nonblocking, close-on-exec descriptor. */
int roce_mr_netlink_open(struct roce_mr_netlink *netlink);

/* Returns 1 for a ticket, 0 for no ticket, or a negative errno. */
int roce_mr_netlink_receive(const struct roce_mr_netlink *netlink,
                            struct roce_mr_ticket_notification *notification);

#endif
