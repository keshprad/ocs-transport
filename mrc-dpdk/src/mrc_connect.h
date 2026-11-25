#ifndef MRC_CONNECT_H
#define MRC_CONNECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MRC_CONNECT_RECORD_VERSION 1u
#define MRC_CONNECT_RECORD_SIZE 64u

enum mrc_connect_capability {
    MRC_CONNECT_TRIM_NACK = 1u << 0,
    MRC_CONNECT_DYNAMIC_MPR = 1u << 1,
    MRC_CONNECT_SERVICE_TIME = 1u << 2,
};

struct mrc_connect_memory {
    uint64_t address;
    uint64_t length;
    uint32_t r_key;
    bool present;
};

struct mrc_connect_receive {
    uint32_t qpn;
    uint32_t initial_psn;
    uint32_t initial_msn;
    uint16_t initial_rqmsn;
    uint8_t max_mpr; /* Units of 128 packets. */
    uint16_t max_wimm;
    struct mrc_connect_memory memory;
};

struct mrc_connect_offer {
    struct mrc_connect_receive receive;
    uint16_t pmtu; /* Maximum RoCE path MTU in bytes. */
    uint16_t p_key;
    uint32_t supported; /* Local implementation capabilities. */
    uint32_t offered; /* Subset enabled by local policy for this QP. */
};

struct mrc_connect_qp {
    struct mrc_connect_receive send; /* Peer's receive attributes. */
    struct mrc_connect_receive receive; /* Local receive attributes. */
    uint16_t pmtu;
    uint16_t local_p_key;
    uint16_t peer_p_key;
    uint32_t local_supported;
    uint32_t peer_supported;
    uint32_t local_responder_features;
    uint32_t peer_responder_features;
    bool dynamic_mpr;
};

/* OCP §10.1.2.2 leaves the out-of-band encoding open. This record is a
 * versioned lab exchange transported separately from the MRC data plane. */
int mrc_connect_encode(uint8_t *bytes, size_t length, const struct mrc_connect_offer *offer);
int mrc_connect_decode(const uint8_t *bytes, size_t length, struct mrc_connect_offer *offer);
int mrc_connect_negotiate(const struct mrc_connect_offer *local,
                           const struct mrc_connect_offer *peer, struct mrc_connect_qp *qp);

/* Resolve a remote WRITE range after QP setup. A registered zero-byte range
 * is distinct from an offer without a registered memory region. */
bool mrc_connect_write_range(const struct mrc_connect_receive *destination,
                             uint64_t offset, uint64_t length, uint64_t *address,
                             uint32_t *r_key);

#endif
