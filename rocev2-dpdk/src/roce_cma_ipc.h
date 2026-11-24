#ifndef ROCE_CMA_IPC_H
#define ROCE_CMA_IPC_H

#include <stdbool.h>
#include <stdint.h>

enum {
    ROCE_CMA_IPC_VERSION = 2,
    /* The lab REQ has 56 user bytes, of which 32 carry the MR offer. */
    ROCE_CMA_PRIVATE_MAX = 23,
};

enum roce_cma_message_type {
    ROCE_CMA_RESOLVE_ADDR = 1,
    ROCE_CMA_RESOLVE_ROUTE,
    ROCE_CMA_LISTEN,
    ROCE_CMA_CONNECT,
    ROCE_CMA_ACCEPT,
    ROCE_CMA_ESTABLISH,
    ROCE_CMA_DISCONNECT,
    ROCE_CMA_ADDR_RESOLVED = 16,
    ROCE_CMA_ADDR_ERROR,
    ROCE_CMA_ROUTE_RESOLVED,
    ROCE_CMA_ROUTE_ERROR,
    ROCE_CMA_CONNECT_REQUEST,
    ROCE_CMA_CONNECT_RESPONSE,
    ROCE_CMA_ESTABLISHED,
    ROCE_CMA_REJECTED,
    ROCE_CMA_CONNECT_ERROR,
    ROCE_CMA_DISCONNECTED,
};

/* Native byte order on the authenticated local Unix socket. */
struct roce_cma_message {
    uint32_t version;
    uint32_t type;
    int32_t status;
    uint32_t qp_num;
    uint32_t local_ipv4;
    uint32_t peer_ipv4;
    uint16_t local_port;
    uint16_t peer_port;
    uint8_t private_length;
    uint8_t private_data[ROCE_CMA_PRIVATE_MAX];
};

struct roce_cma_lab_address {
    uint32_t local_ipv4;
    uint32_t peer_ipv4;
    uint16_t service_port;
    uint16_t reserved;
};

/* Send returns 0 or a positive errno. Receive returns 1, 0, or a negative errno. */
int roce_cma_send(int socket, const struct roce_cma_message *message);
int roce_cma_receive(int socket, struct roce_cma_message *message, bool nonblocking);

#endif
