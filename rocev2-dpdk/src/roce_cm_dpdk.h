#ifndef ROCE_CM_DPDK_H
#define ROCE_CM_DPDK_H

#include "roce_cm_session.h"
#include "roce_cma_service.h"
#include "roce_lab.h"

struct rte_mempool;
struct rte_mbuf;
struct roce_app_memory;

enum { ROCE_CM_RX_BURST = 16 };

struct roce_cm_port {
    struct roce_cm_session session;
    struct rte_mbuf *early[ROCE_CM_RX_BURST];
    uint16_t early_count;
};

bool roce_cm_lab_exchange(uint16_t port, struct rte_mempool *pool,
                          const struct roce_lab_options *options, struct roce_lab_offer *local,
                          const struct roce_address *destination, struct roce_lab_offer *peer,
                          struct roce_cm_port *cm, struct roce_cma_service *cma,
                          struct roce_app_memory *application);
bool roce_cm_lab_close(uint16_t port, struct rte_mempool *pool,
                      struct roce_cm_port *cm, struct roce_cma_service *cma);
void roce_cm_transmit(uint16_t port, struct rte_mempool *pool, struct roce_cm_session *session,
                      uint64_t now);

#endif
