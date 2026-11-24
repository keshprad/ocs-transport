#ifndef ROCE_APP_NATIVE_H
#define ROCE_APP_NATIVE_H

#include <stdbool.h>
#include <stdint.h>

struct ibv_pd;
struct ibv_cq;
struct ibv_qp;

/* Initialize to zero before creating the QP. The caller owns both handles. */
struct roce_app_native_qp {
    struct ibv_cq *cq;
    struct ibv_qp *qp;
};

int roce_app_create_native_qp(struct ibv_pd *pd, uint32_t depth, uint32_t send_sges, bool receiver,
                              struct roce_app_native_qp *resources);
int roce_app_destroy_native_qp(struct roce_app_native_qp *resources);

/* An odd split exercises packets crossing the boundary at any supported PMTU. */
static inline uint32_t roce_app_first_sge_length(uint32_t bytes)
{
    return (bytes / 2) | 1;
}

/* early_qp is optional and must be configured on queue_fd. The caller retains its ownership. */
bool roce_app_run_native_verbs(struct ibv_pd *pd, int queue_fd, int app_socket,
                               uint32_t post_batch, bool scatter_gather,
                               int (*submitted)(void *), void *context,
                               struct roce_app_native_qp *early_qp);

#endif
