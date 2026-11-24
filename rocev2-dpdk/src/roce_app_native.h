#ifndef ROCE_APP_NATIVE_H
#define ROCE_APP_NATIVE_H

#include <stdbool.h>
#include <stdint.h>

struct ibv_pd;

/* The caller owns the PD and descriptors. Failure signals the worker by shutting down the app socket. */
bool roce_app_run_native_verbs(struct ibv_pd *pd, int queue_fd, int app_socket,
                               uint32_t post_batch, bool scatter_gather);

#endif
