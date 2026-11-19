#ifndef MRC_DPDK_ABI_H
#define MRC_DPDK_ABI_H

#include <mrc.h>

#include "mrc_provider.h"

#include <pthread.h>

struct mrc_context {
    struct ibv_context *verbs; /* Borrowed until all MRC resources are destroyed. */
    pthread_mutex_t mutex;
    size_t children;
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

/* Import a caller-owned verbs registration with the permissions enabled for
 * MRC. Keep the verbs MR live until the provider lkey is deregistered. */
int mrc_abi_bind_mr(const struct mrc_context *context, struct mrc_provider_pd *provider,
                     const struct ibv_pd *verbs_pd, const struct ibv_mr *mr,
                     uint64_t iova, unsigned int access);

#endif
