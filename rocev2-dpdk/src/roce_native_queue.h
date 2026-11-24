#ifndef ROCE_NATIVE_QUEUE_H
#define ROCE_NATIVE_QUEUE_H

#include "roce_doorbell.h"
#include "roce_lab.h"

enum {
    ROCE_NATIVE_QUEUE_VERSION = 1,
    ROCE_NATIVE_QUEUE_DEPTH = 256,
    ROCE_NATIVE_QUEUE_FD = 4,
};

#define ROCE_NATIVE_SQ_CLOSED (UINT32_C(1) << 31)

enum roce_native_wc_status {
    ROCE_NATIVE_WC_SUCCESS,
    ROCE_NATIVE_WC_LOCAL_LENGTH,
    ROCE_NATIVE_WC_LOCAL_PROTECTION,
    ROCE_NATIVE_WC_REMOTE_ACCESS,
    ROCE_NATIVE_WC_RETRY_EXCEEDED,
    ROCE_NATIVE_WC_FLUSHED,
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

static inline uint32_t roce_native_posted_count(uint32_t posted)
{
    return posted & ~ROCE_NATIVE_SQ_CLOSED;
}

static inline uint32_t roce_native_psn(uint32_t base, uint32_t index)
{
    return (base + index * UINT32_C(0x100001)) & ROCE_PSN_MASK;
}

struct ibv_context;
/* Associate a provider context with QPs already negotiated by its service. */
int roce_dpdk_bind_queues(struct ibv_context *context, int fd);

#endif
