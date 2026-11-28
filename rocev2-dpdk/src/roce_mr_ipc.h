#ifndef ROCE_MR_IPC_H
#define ROCE_MR_IPC_H

#include <stdint.h>
#include <sys/types.h>

enum {
    ROCE_MR_IPC_VERSION = 1,
    ROCE_MR_IPC_TICKET_VERSION = 2,
    ROCE_MR_IPC_SCOPED_VERSION = 3,
    ROCE_MR_IPC_SCOPED_TICKET_VERSION = 4,
    ROCE_MR_IPC_MAX_REGISTRATIONS = 64,
    ROCE_MR_IPC_WRITE = 1u << 0,
    ROCE_MR_IPC_REMOTE_WRITE = 1u << 1,
    ROCE_MR_IPC_DEREGISTER = 'D',
};

struct roce_mr_request {
    uint32_t version;
    uint32_t access;
    uint64_t iova;
    uint64_t offset;
    uint64_t length;
    uint64_t mapped_length;
    uint64_t scope;
};

static inline int roce_mr_uses_ticket(uint32_t version)
{
    return version == ROCE_MR_IPC_TICKET_VERSION || version == ROCE_MR_IPC_SCOPED_TICKET_VERSION;
}

struct roce_mr_reply {
    int32_t error;
    uint32_t lkey;
    uint32_t rkey;
    uint32_t reserved;
};

struct roce_mr_ticket_ready {
    int32_t error;
    uint32_t portid;
    uint64_t cookie;
};

/* A channel is bound to one registration until the provider deregisters or disconnects. */
int roce_mr_ipc_connect(const char *path);
int roce_mr_ipc_listen(const char *path);
int roce_mr_ipc_send_registration(int socket, const struct roce_mr_request *request, int mapping_fd);
int roce_mr_ipc_read_reply(int socket, struct roce_mr_reply *reply);
int roce_mr_ipc_receive_registration(int socket, struct roce_mr_request *request, int *mapping_fd);
int roce_mr_ipc_send_reply(int socket, const struct roce_mr_reply *reply);
int roce_mr_ipc_read_ticket_ready(int socket, struct roce_mr_ticket_ready *reply);
int roce_mr_ipc_send_ticket_ready(int socket, const struct roce_mr_ticket_ready *reply);

#endif
