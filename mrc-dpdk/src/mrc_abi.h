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

#endif
