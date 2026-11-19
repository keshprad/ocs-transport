#ifndef MRC_DPDK_H
#define MRC_DPDK_H

#include <mrc.h>
#include <stdbool.h>

/* Import a verbs registration into the matching MRC protection domain.
 * Use the same IOVA and access flags used for the original registration.
 * Keep the verbs MR and PD live until unbind succeeds. Both functions
 * return zero or a positive errno value. */
int mrc_dpdk_bind_mr(struct mrc_context *context, const struct ibv_mr *mr,
                     uint64_t iova, unsigned int access);
int mrc_dpdk_unbind_mr(struct mrc_context *context, const struct ibv_mr *mr);

struct rte_mempool;

/* Application-owned, started DPDK ports with queue zero for RX and TX.
 * One direction uses these ports until reset, an explicit transition to
 * ERROR, or destruction.
 * Do not poll the same RX queues elsewhere during that time. */
struct mrc_dpdk_route {
    uint16_t ports[2];
    uint8_t peer_macs[2][6];
    struct rte_mempool *pool;
    uint16_t peer_pkey; /* Host order. Zero uses the local P_Key. */
    uint16_t udp_port; /* Zero uses the OCP default. */
    uint8_t trim_dscp; /* Zero selects the default trim DSCP (49). */
    bool requester;
};

/* Bind and drive the QP from the application's DPDK worker. Poll completion
 * queues independently. Both calls return zero or a positive errno. */
int mrc_dpdk_attach_qp(struct mrc_qp *qp, const struct mrc_dpdk_route *route);
int mrc_dpdk_progress_qp(struct mrc_qp *qp);

#endif
