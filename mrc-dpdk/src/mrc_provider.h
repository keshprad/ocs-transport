#ifndef MRC_PROVIDER_H
#define MRC_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mrc_provider_pd;
struct mrc_provider_cq;

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

int mrc_provider_cq_create(size_t capacity, struct mrc_provider_cq **cq);
int mrc_provider_cq_retain(struct mrc_provider_cq *cq);
int mrc_provider_cq_release(struct mrc_provider_cq *cq);
int mrc_provider_cq_destroy(struct mrc_provider_cq *cq);

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
 * Each event returned by next_event must be acknowledged exactly once.
 */
int mrc_provider_cq_arm(struct mrc_provider_cq *cq, bool solicited_only);
int mrc_provider_cq_next_event(struct mrc_provider_cq *cq, enum mrc_provider_cq_event *event);
int mrc_provider_cq_ack_events(struct mrc_provider_cq *cq, size_t count);

#endif
