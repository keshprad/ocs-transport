#ifndef MRC_PROVIDER_H
#define MRC_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mrc_provider_pd;
struct mrc_provider_cq;
struct mrc_provider_comp_channel;
struct mrc_provider_qp;

/*
 * Create/destroy own an object's lifetime. Retain/release protect references
 * held by a QP or other child object. Operations on live objects are thread
 * safe. The owner must synchronize destroy with callers that lack a retained
 * reference. Functions return zero or negative errno except CQ poll.
 */

int mrc_provider_pd_create(size_t region_capacity, struct mrc_provider_pd **pd);
int mrc_provider_pd_retain(struct mrc_provider_pd *pd);
int mrc_provider_pd_release(struct mrc_provider_pd *pd);
int mrc_provider_pd_destroy(struct mrc_provider_pd *pd);

enum mrc_provider_mr_flags {
    MRC_PROVIDER_MR_LOCAL_WRITE = 1u << 0,
    MRC_PROVIDER_MR_REMOTE_WRITE = 1u << 1,
};

struct mrc_provider_mr_keys {
    uint32_t lkey;
    uint32_t rkey;
};

/*
 * The owner keeps host memory mapped and accessible until deregistration.
 * Keys are process-local capability identifiers scoped to this PD. They do
 * not provide cryptographic peer authentication. A zero rkey means the
 * registration does not permit remote access.
 */
int mrc_provider_mr_register(struct mrc_provider_pd *pd, void *base, size_t length,
                             uint64_t iova, unsigned int flags,
                             struct mrc_provider_mr_keys *keys);
int mrc_provider_mr_deregister(struct mrc_provider_pd *pd, uint32_t lkey);

enum mrc_provider_mr_operation {
    MRC_PROVIDER_MR_READ_LOCAL,
    MRC_PROVIDER_MR_WRITE_LOCAL,
    MRC_PROVIDER_MR_WRITE_REMOTE,
};

struct mrc_provider_mr_access {
    uint64_t iova;
    size_t length;
    size_t alignment; /* Zero disables the optional power-of-two alignment check. */
    uint32_t key;
    enum mrc_provider_mr_operation operation;
};

/*
 * Initialize a lease to zero before acquisition. A successful lease pins its
 * registration and may not be copied. The address is valid until release.
 */
struct mrc_provider_mr_lease {
    void *address;
    size_t length;
    uint64_t region_iova; /* Bounds of the pinned registration, in IOVA space. */
    size_t region_length;
    struct mrc_provider_pd *owner;
    uint32_t lkey;
};

int mrc_provider_mr_acquire(struct mrc_provider_pd *pd,
                            const struct mrc_provider_mr_access *access,
                            struct mrc_provider_mr_lease *lease);
int mrc_provider_mr_release(struct mrc_provider_mr_lease *lease);

enum mrc_provider_wc_opcode {
    MRC_PROVIDER_WC_WRITE,
    MRC_PROVIDER_WC_RECV_WRITE_IMM,
};

enum mrc_provider_wc_status {
    MRC_PROVIDER_WC_SUCCESS,
    MRC_PROVIDER_WC_LOCAL_LENGTH,
    MRC_PROVIDER_WC_LOCAL_PROTECTION,
    MRC_PROVIDER_WC_LOCAL_OPERATION,
    MRC_PROVIDER_WC_BAD_RESPONSE,
    MRC_PROVIDER_WC_REMOTE_INVALID_REQUEST,
    MRC_PROVIDER_WC_REMOTE_ACCESS,
    MRC_PROVIDER_WC_REMOTE_OPERATION,
    MRC_PROVIDER_WC_RETRY_EXCEEDED,
    MRC_PROVIDER_WC_FLUSHED,
};

struct mrc_provider_wc {
    uint64_t wr_id;
    uint32_t qpn;
    uint32_t byte_length;
    uint32_t immediate; /* Host byte order. */
    enum mrc_provider_wc_opcode opcode;
    enum mrc_provider_wc_status status;
    bool with_immediate;
};

enum mrc_provider_cq_event {
    MRC_PROVIDER_CQ_COMPLETION,
    MRC_PROVIDER_CQ_OVERFLOW,
};

/* The channel FD is borrowed for poll/select. Retrieve notifications with
 * next rather than reading the FD directly. Next returns -EAGAIN when empty. */
int mrc_provider_comp_channel_create(struct mrc_provider_comp_channel **channel);
int mrc_provider_comp_channel_fd(const struct mrc_provider_comp_channel *channel);
int mrc_provider_comp_channel_next(struct mrc_provider_comp_channel *channel,
                                   struct mrc_provider_cq **cq, void **context);
int mrc_provider_comp_channel_destroy(struct mrc_provider_comp_channel *channel);

int mrc_provider_cq_create_on_channel(size_t capacity, struct mrc_provider_comp_channel *channel,
                                      void *context, struct mrc_provider_cq **cq);
int mrc_provider_cq_create(size_t capacity, struct mrc_provider_cq **cq);
int mrc_provider_cq_retain(struct mrc_provider_cq *cq);
int mrc_provider_cq_release(struct mrc_provider_cq *cq);
int mrc_provider_cq_destroy(struct mrc_provider_cq *cq);
/* Discard unread completions and notifications, then wait for already read
 * events to be acknowledged. Both destroy variants reject retained CQs. */
int mrc_provider_cq_destroy_wait(struct mrc_provider_cq *cq);

/*
 * Push and poll use a bounded FIFO. An overrun invalidates pending entries
 * and permanently makes both operations return -EOVERFLOW.
 * Poll returns the number copied on success.
 */
int mrc_provider_cq_push(struct mrc_provider_cq *cq, const struct mrc_provider_wc *wc,
                         bool solicited);
int mrc_provider_cq_poll(struct mrc_provider_cq *cq, struct mrc_provider_wc *entries,
                         size_t count);

/*
 * Arm one notification for a future completion. In solicited mode, successful
 * completions only qualify when solicited is set. Errors always qualify.
 * An overflow event is available even when notifications were not armed.
 * Read completion events on the attached channel, or with next_event for
 * a CQ without a channel. Overflow is available through next_event.
 * Each event returned by either call must be acknowledged exactly once.
 */
int mrc_provider_cq_arm(struct mrc_provider_cq *cq, bool solicited_only);
int mrc_provider_cq_next_event(struct mrc_provider_cq *cq, enum mrc_provider_cq_event *event);
int mrc_provider_cq_ack_events(struct mrc_provider_cq *cq, size_t count);

enum mrc_provider_qp_state {
    MRC_PROVIDER_QP_RESET,
    MRC_PROVIDER_QP_INIT,
    MRC_PROVIDER_QP_RTR,
    MRC_PROVIDER_QP_RTS,
    MRC_PROVIDER_QP_ERROR,
};

struct mrc_provider_qp_config {
    struct mrc_provider_pd *pd;
    struct mrc_provider_cq *send_cq;
    struct mrc_provider_cq *recv_cq;
    size_t send_capacity;
    size_t recv_capacity;
    uint32_t qpn;
    bool sq_sig_all;
};

struct mrc_provider_qp_attr {
    enum mrc_provider_qp_state state;
    uint32_t qpn;
    size_t send_pending;
    size_t recv_pending;
};

/* The data plane owns the wire state and send buffers. The provider tracks
 * posted work and retains its CQ/PD handles until the QP is destroyed. */
int mrc_provider_qp_create(const struct mrc_provider_qp_config *config,
                           struct mrc_provider_qp **qp);
int mrc_provider_qp_query(struct mrc_provider_qp *qp, struct mrc_provider_qp_attr *attr);
int mrc_provider_qp_transition(struct mrc_provider_qp *qp, enum mrc_provider_qp_state expected,
                               enum mrc_provider_qp_state next);
int mrc_provider_qp_destroy(struct mrc_provider_qp *qp);

/* Send sequences start at one and are never reused during a QP's lifetime.
 * wr_id belongs to the application and may be reused or zero. */
int mrc_provider_qp_post_send(struct mrc_provider_qp *qp, uint64_t wr_id, uint32_t length,
                              bool signaled, uint64_t *sequence);
int mrc_provider_qp_post_recv(struct mrc_provider_qp *qp, uint64_t wr_id);

/* Retire sends through an inclusive sequence after semantic ACK. Receive
 * completion consumes one posted WRITEIMM credit. */
int mrc_provider_qp_complete_send(struct mrc_provider_qp *qp, uint64_t sequence);
int mrc_provider_qp_complete_recv(struct mrc_provider_qp *qp, uint32_t length,
                                  uint32_t immediate, bool solicited, uint64_t *wr_id);

/* Enter ERROR and publish the selected send error plus flush completions in
 * queue order. Sequence zero with status FLUSHED flushes both queues. */
int mrc_provider_qp_fail(struct mrc_provider_qp *qp, uint64_t sequence,
                         enum mrc_provider_wc_status status);

#endif
