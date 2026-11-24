#ifndef ROCE_CM_DPDK_H
#define ROCE_CM_DPDK_H

#include "roce_cm_session.h"
#include "roce_cma_service.h"
#include "roce_lab.h"

struct rte_mempool;
struct roce_app_memory;

bool roce_cm_lab_exchange(uint16_t port, struct rte_mempool *pool, int control,
                          const struct roce_lab_options *options, const struct roce_lab_offer *local,
                          const struct roce_address *destination, struct roce_lab_offer *peer,
                          struct roce_cm_session *session, struct roce_cma_service *cma,
                          struct roce_app_memory *application);
bool roce_cm_lab_close(uint16_t port, struct rte_mempool *pool, int control,
                      struct roce_cm_session *session, struct roce_cma_service *cma);

#endif
