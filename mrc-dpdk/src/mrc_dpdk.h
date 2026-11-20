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

/* Built-in controller EV formats. UDP and STEV use a big-endian uint32
 * in the first four bytes of mrc_ctl_ev_t. SRv6 values contain an IPv6
 * destination followed by the single SRH container when applicable.
 * Unused bytes are zero. */
#define MRC_DPDK_EV_FORMAT_UDP UINT64_C(1)
#define MRC_DPDK_EV_FORMAT_STEV UINT64_C(2)
#define MRC_DPDK_EV_FORMAT_SRV6 UINT64_C(3)
#define MRC_DPDK_EV_FORMAT_SRV6_SRH UINT64_C(4)

/* Select a stable EV and CC profile directory once per MRC context.
 * Both the controller and application contexts use the same storage. */
int mrc_dpdk_set_profile_store(struct mrc_context *context, const char *directory);
/* Compatibility name for mrc_dpdk_set_profile_store. */
int mrc_dpdk_set_ev_store(struct mrc_context *context, const char *directory);

struct rte_mempool;

/* Map a service level from 0 through 15 to data and retransmission DSCPs.
 * Both default to zero. Requester attachment snapshots the mapping chosen
 * by the QP's address vector. Reliability probes use the data DSCP.
 * DSCP 48 is reserved for Control. Returns zero or positive errno. */
int mrc_dpdk_set_sl_dscp(struct mrc_context *context, unsigned int sl,
                        unsigned int data_dscp, unsigned int retry_dscp);

/* Application-owned, started DPDK ports with queue zero for RX and TX.
 * QPs in this process can share these ports. Keep application I/O on the
 * attached queues inside the MRC progress calls until the QPs detach.
 * Both port MTUs and the pool's mbuf data room must fit the selected QP
 * path MTU and its wire headers. Attach returns EMSGSIZE for insufficient
 * port or pool capacity. */
struct mrc_dpdk_route {
    uint16_t ports[2];
    uint8_t peer_macs[2][6];
    struct rte_mempool *pool;
    /* Host order. Zero uses RC membership. A nonzero value pins the compatible peer key. */
    uint16_t peer_pkey;
    uint16_t udp_port; /* Zero uses the OCP default. */
    uint8_t trim_dscp; /* Zero selects the default trim DSCP (49). */
    bool requester; /* Selects the direction when bidirectional is false. */
    bool bidirectional; /* Attach requester and responder on the same QP. */
};

/* Per-rail IPv6 outer addresses and optional single-segment SRH. An all-zero
 * initial destination selects peer_sid. An all-zero SRH segment omits the
 * header. An SRH may start with zero or one segment left. An SRv6 EV
 * profile selected by the requester supplies its outgoing destination
 * and SRH in place of these per-rail defaults. */
struct mrc_dpdk_srv6_path {
    uint8_t local_sid[16];
    uint8_t peer_sid[16];
    uint8_t initial_dst[16];
    uint8_t srh_segment[16];
    uint8_t segments_left;
};

/* Connectionless route over two application-owned, started DPDK ports.
 * The port numbers in controller operations are 1 and 2, in route order.
 * Zero pkey selects 0xffff. Zero peer_pkey uses pkey. Zero udp_port uses
 * the OCP default. Optional SRv6 paths configure the return route and
 * endpoint SIDs. A request supplies its own outgoing EV. */
struct mrc_dpdk_endpoint_route {
    uint16_t ports[2];
    uint8_t peer_macs[2][6];
    struct rte_mempool *pool;
    union ibv_gid local_gid;
    union ibv_gid peer_gid;
    uint16_t pkey;
    uint16_t peer_pkey;
    uint16_t udp_port;
};

/* One connectionless route per context. Progress the endpoint to respond
 * to incoming requests. A blocking controller batch drives its own RX/TX.
 * The route can share DPDK queues with MRC QPs. Detach before stopping
 * the ports, freeing the pool, or destroying the context. These calls
 * return zero or a positive errno. */
int mrc_dpdk_attach_endpoint(struct mrc_context *context, const struct mrc_dpdk_endpoint_route *route,
                              const struct mrc_dpdk_srv6_path paths[2]);
int mrc_dpdk_progress_endpoint(struct mrc_context *context);
int mrc_dpdk_detach_endpoint(struct mrc_context *context);

/* Bind and drive the QP from the application's DPDK worker. Poll completion
 * queues independently. These calls return zero or a positive errno. */
int mrc_dpdk_attach_qp(struct mrc_qp *qp, const struct mrc_dpdk_route *route);
int mrc_dpdk_attach_srv6_qp(struct mrc_qp *qp, const struct mrc_dpdk_route *route,
                           const struct mrc_dpdk_srv6_path paths[2]);
int mrc_dpdk_progress_qp(struct mrc_qp *qp);

/* Advertise a 0-127 receiver window penalty in subsequent SACKs for this
 * attached responder QP. Zero disables the penalty. Set restore with zero
 * to request restoration of the peer's saved congestion window. This call
 * returns zero or a positive errno. */
int mrc_dpdk_set_rx_flow_control(struct mrc_qp *qp, uint8_t penalty, bool restore);

/* Configure last-hop trim recognition on an attached responder QP.
 * Zero disables the last-hop class. A nonzero DSCP must differ from
 * the regular trim and Control classes. Returns zero or positive errno. */
int mrc_dpdk_set_trim_last_hop_dscp(struct mrc_qp *qp, unsigned int dscp);

#endif
