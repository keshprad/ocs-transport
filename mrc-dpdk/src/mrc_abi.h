#ifndef MRC_DPDK_ABI_H
#define MRC_DPDK_ABI_H

#include "mrc_dpdk.h"

#include "mrc_provider.h"

#include <pthread.h>

#define MRC_PUBLIC __attribute__((visibility("default")))

enum {
    MRC_ABI_MAX_MPR = 8,
    MRC_ABI_MAX_WIMM = 1024,
};

struct mrc_abi_route;

struct mrc_abi_pd {
    struct mrc_abi_pd *next;
    const struct ibv_pd *verbs;
    struct mrc_provider_pd *provider;
    size_t users;
};

struct mrc_context {
    struct ibv_context *verbs; /* Borrowed until all MRC resources are destroyed. */
    pthread_mutex_t mutex;
    size_t children;
    struct mrc_abi_pd *pds;
};

struct mrc_comp_channel {
    struct mrc_context *context;
    struct mrc_provider_comp_channel *provider;
};

struct mrc_cq {
    struct mrc_context *context;
    struct mrc_provider_cq *provider;
    void *application_context;
    int capacity;
};

struct mrc_qp {
    struct mrc_context *context;
    struct mrc_abi_pd *pd;
    struct mrc_provider_qp *provider;
    pthread_mutex_t mutex;
    struct mrc_qp_init_attr init;
    struct ibv_qp_attr verbs;
    struct mrc_qp_attr attrs;
    union ibv_gid source_gid;
    struct mrc_abi_route *route;
    bool requester;
    bool responder;
    uint32_t qpn;
};

/* Caller holds the QP mutex. */
void mrc_abi_release_route(struct mrc_qp *qp);

/* Import a caller-owned verbs registration with the permissions enabled for
 * MRC. Keep the verbs MR live until the provider lkey is deregistered. */
int mrc_abi_bind_mr(const struct mrc_context *context, struct mrc_provider_pd *provider,
                     const struct ibv_pd *verbs_pd, const struct ibv_mr *mr,
                     uint64_t iova, unsigned int access);

#endif
