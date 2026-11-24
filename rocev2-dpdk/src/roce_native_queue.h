#ifndef ROCE_NATIVE_QUEUE_H
#define ROCE_NATIVE_QUEUE_H

#include "roce_doorbell.h"
#include "roce_lab.h"

enum {
    ROCE_NATIVE_QUEUE_VERSION = 2,
    ROCE_NATIVE_SGE_QUEUE_VERSION = 3,
    ROCE_NATIVE_MAX_SEND_SGES = 2,
    ROCE_NATIVE_QUEUE_DEPTH = 256,
    ROCE_NATIVE_MAX_WRITE = INT32_MAX,
    ROCE_NATIVE_QUEUE_FD = 4,
    ROCE_NATIVE_QUEUE_RECEIVE_ONLY = 1u << 0,
    ROCE_NATIVE_QUEUE_MULTI_MR = 1u << 1,
};

static inline bool roce_native_valid_queue_flags(uint32_t version, uint32_t flags)
{
    return !flags || flags == ROCE_NATIVE_QUEUE_RECEIVE_ONLY ||
           (version == ROCE_NATIVE_SGE_QUEUE_VERSION && flags == ROCE_NATIVE_QUEUE_MULTI_MR);
}

#define ROCE_NATIVE_SQ_CLOSED (UINT32_C(1) << 31)
#define ROCE_NATIVE_DOORBELL_OVERFLOW UINT64_C(1)

enum roce_native_wc_status {
    ROCE_NATIVE_WC_SUCCESS,
    ROCE_NATIVE_WC_LOCAL_LENGTH,
    ROCE_NATIVE_WC_LOCAL_PROTECTION,
    ROCE_NATIVE_WC_REMOTE_ACCESS,
    ROCE_NATIVE_WC_RETRY_EXCEEDED,
    ROCE_NATIVE_WC_FLUSHED,
    ROCE_NATIVE_WC_REMOTE_INVALID,
    ROCE_NATIVE_WC_REMOTE_OPERATION,
};

struct roce_native_queue_config {
    uint32_t version;
    uint32_t qps;
    uint32_t workers;
    uint32_t writes;
    uint32_t write_size;
    uint32_t local_qpn;
    uint32_t peer_qpn;
    uint32_t local_psn;
    uint32_t peer_psn;
    uint32_t pmtu;
    uint32_t local_lkey;
    uint32_t remote_rkey;
    uint32_t local_ipv4;
    uint32_t peer_ipv4;
    uint32_t flags;
    uint64_t local_address;
    uint64_t remote_address;
};

struct roce_native_write {
    uint64_t wr_id;
    uint64_t local_address;
    uint64_t remote_address;
    uint32_t length;
    uint32_t lkey;
    uint32_t rkey;
};

struct roce_native_sq {
    _Alignas(64) _Atomic uint32_t posted; /* Provider publishes entries. */
    _Alignas(64) _Atomic uint32_t admitted; /* Worker validated entries. */
    _Atomic uint32_t acknowledged; /* Full WRs acknowledged by the peer. */
    _Atomic uint32_t retired; /* Worker publishes status and completion. */
    struct roce_native_write entries[ROCE_NATIVE_QUEUE_DEPTH];
    uint8_t status[ROCE_NATIVE_QUEUE_DEPTH];
};

struct roce_native_work {
    struct roce_native_queue_config config;
    struct roce_doorbell doorbells[ROCE_LAB_MAX_QPS];
    struct roce_native_sq qps[ROCE_LAB_MAX_QPS];
};

/* Version 3 appends second SGEs. The base WRITE length is the sum of both spans. */
struct roce_native_sge {
    uint64_t address;
    uint32_t length;
    uint32_t lkey;
};

struct roce_native_sge_work {
    struct roce_native_work base;
    struct roce_native_sge second[ROCE_LAB_MAX_QPS][ROCE_NATIVE_QUEUE_DEPTH];
};

static inline size_t roce_native_work_size(uint32_t version)
{
    if (version == ROCE_NATIVE_QUEUE_VERSION)
        return sizeof(struct roce_native_work);
    if (version == ROCE_NATIVE_SGE_QUEUE_VERSION)
        return sizeof(struct roce_native_sge_work);
    return 0;
}

static inline bool roce_native_valid_work_size(uint64_t length)
{
    return length == sizeof(struct roce_native_work) || length == sizeof(struct roce_native_sge_work);
}

static inline uint32_t roce_native_send_sges(uint32_t version)
{
    return version == ROCE_NATIVE_SGE_QUEUE_VERSION ? ROCE_NATIVE_MAX_SEND_SGES : 1;
}

static inline uint32_t roce_native_posted_count(uint32_t posted)
{
    return posted & ~ROCE_NATIVE_SQ_CLOSED;
}

static inline bool roce_native_contains(uint64_t base, uint64_t size,
                                        uint64_t address, uint64_t length)
{
    return address >= base && length <= size && address - base <= size - length;
}

static inline void roce_native_notify(struct roce_doorbell *doorbell, uint64_t item)
{
    if (roce_doorbell_publish(doorbell, item))
        atomic_fetch_add_explicit(&doorbell->notifications, 2, memory_order_release);
    else
        atomic_fetch_or_explicit(&doorbell->notifications, ROCE_NATIVE_DOORBELL_OVERFLOW,
                                 memory_order_release);
}

static inline uint32_t roce_native_queue_capacity(uint32_t writes)
{
    return writes < ROCE_NATIVE_QUEUE_DEPTH ? writes : ROCE_NATIVE_QUEUE_DEPTH;
}

static inline uint32_t roce_native_psn(uint32_t base, uint32_t index)
{
    return (base + index * UINT32_C(0x100001)) & ROCE_PSN_MASK;
}

struct ibv_context;
/* Associate negotiated transport parameters with the context's queue resources. */
int roce_dpdk_bind_queues(struct ibv_context *context, int fd);

#endif
