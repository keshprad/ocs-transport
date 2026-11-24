#ifndef ROCE_APP_RDMACM_H
#define ROCE_APP_RDMACM_H

#include "roce_cma_ipc.h"

struct rdma_event_channel;
struct rdma_cm_id;
struct ibv_pd;
struct roce_app_native_qp;

/* Initialize to zero before use. The session owns its channel and IDs. */
struct roce_app_rdmacm {
    struct rdma_event_channel *channel;
    struct rdma_cm_id *listener;
    struct rdma_cm_id *id;
    bool disconnect_started;
    bool disconnected;
};

bool roce_app_rdmacm_connect(struct roce_app_rdmacm *cm, const struct roce_cma_lab_address *address,
                             struct ibv_pd *pd, struct roce_app_native_qp *resources,
                             bool sender, uint32_t send_sges, int (*prepare_qp)(void *), void *context);
int roce_app_rdmacm_destroy_qp(struct roce_app_rdmacm *cm);
int roce_app_rdmacm_disconnect(void *opaque);
bool roce_app_rdmacm_finish(struct roce_app_rdmacm *cm);
void roce_app_rdmacm_destroy(struct roce_app_rdmacm *cm);

#endif
