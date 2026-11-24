#ifndef ROCE_MR_SERVER_H
#define ROCE_MR_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct roce_mr_server;
struct roce_mr_registration;

struct roce_mr_lease {
    struct roce_mr_server *server;
    struct roce_mr_registration *registration;
    uint8_t *buffer;
    size_t length;
    void *mapping;
    size_t mapped_length;
    uint64_t iova;
    uint64_t scope;
    uint32_t lkey;
    uint32_t rkey;
};

/* Keep the anchor lease for the QP lifetime and release each resolved WRITE before the next. */
struct roce_mr_destination {
    const struct roce_mr_lease *anchor;
    struct roce_mr_lease active;
};

struct roce_mr_server *roce_mr_server_start(const char *path);
/* A lease retains the mapped pages even if the registering process exits. */
int roce_mr_acquire(struct roce_mr_server *server, pid_t owner, uint32_t lkey, uint32_t access,
                   struct roce_mr_lease *lease);
/* The anchor retains the authenticated process and PD of the connected QP. */
int roce_mr_acquire_related(const struct roce_mr_lease *anchor, uint32_t lkey, uint32_t access,
                           struct roce_mr_lease *lease);
uint8_t *roce_mr_resolve_write(void *ctx, uint32_t rkey, uint64_t address, uint32_t length);
void roce_mr_release_write(void *ctx);
int roce_mr_export_dmabuf(const struct roce_mr_lease *lease);
void roce_mr_release(struct roce_mr_lease *lease);
/* Wait until the registration and any leases have released the key. */
int roce_mr_wait_deregistered(struct roce_mr_server *server, uint32_t lkey, unsigned int timeout_seconds);
/* Returns EBUSY while work still holds a lease. */
int roce_mr_server_stop(struct roce_mr_server *server);

#endif
