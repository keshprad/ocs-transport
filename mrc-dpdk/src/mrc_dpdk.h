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

/* Borrow the context's asynchronous event FD for poll/select.
 * Read and acknowledge events with the OCP functions. With O_NONBLOCK,
 * mrc_get_async_event fails with errno EAGAIN when the queue is empty. */
int mrc_dpdk_get_async_fd(struct mrc_context *context, int *fd);

/* Built-in controller EV formats. EV values use big-endian uint32 in the
 * first four bytes of mrc_ctl_ev_t, with remaining bytes zero. */
#define MRC_DPDK_EV_FORMAT_UDP UINT64_C(1)
#define MRC_DPDK_EV_FORMAT_STEV UINT64_C(2)

/* Select a stable EV and CC profile directory once per MRC context.
 * Both the controller and application contexts use the same storage. */
int mrc_dpdk_set_profile_store(struct mrc_context *context, const char *directory);
/* Compatibility name for mrc_dpdk_set_profile_store. */
int mrc_dpdk_set_ev_store(struct mrc_context *context, const char *directory);

struct rte_mempool;

/* Application-owned, started DPDK ports with queue zero for RX and TX.
 * QPs in this process can share these ports. Keep application I/O on the
 * attached queues inside the MRC progress calls until the QPs detach. */
struct mrc_dpdk_route {
    uint16_t ports[2];
    uint8_t peer_macs[2][6];
    struct rte_mempool *pool;
    uint16_t peer_pkey; /* Host order. Zero uses the local P_Key. */
    uint16_t udp_port; /* Zero uses the OCP default. */
    uint8_t trim_dscp; /* Zero selects the default trim DSCP (49). */
    bool requester; /* Selects the direction when bidirectional is false. */
    bool bidirectional; /* Attach requester and responder on the same QP. */
};

/* Bind and drive the QP from the application's DPDK worker. Poll completion
 * queues independently. Both calls return zero or a positive errno. */
int mrc_dpdk_attach_qp(struct mrc_qp *qp, const struct mrc_dpdk_route *route);
int mrc_dpdk_progress_qp(struct mrc_qp *qp);

/* Advertise a 0-127 receiver window penalty in subsequent SACKs for this
 * attached responder QP. Zero disables the penalty. Set restore with zero
 * to request restoration of the peer's saved congestion window. This call
 * returns zero or a positive errno. */
int mrc_dpdk_set_rx_flow_control(struct mrc_qp *qp, uint8_t penalty, bool restore);

#endif
