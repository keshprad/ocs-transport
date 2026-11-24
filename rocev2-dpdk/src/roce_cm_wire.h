#ifndef ROCE_CM_WIRE_H
#define ROCE_CM_WIRE_H

#include "roce_wire.h"

enum {
    ROCE_CM_GID_SIZE = 16,
    ROCE_CM_REQ_PRIVATE_SIZE = 92,
    ROCE_CM_REP_PRIVATE_SIZE = 196,
    ROCE_CM_RTU_PRIVATE_SIZE = 224,
    ROCE_CMA_IPV4_SIZE = 36,
    ROCE_CMA_IPV4_REQ_USER_SIZE = ROCE_CM_REQ_PRIVATE_SIZE - ROCE_CMA_IPV4_SIZE,
};

/* RDMA_PS_TCP service IDs for IPv4 connected RDMA CM. */
#define ROCE_CMA_TCP_SERVICE_BASE UINT64_C(0x01060000)

enum roce_cm_type {
    ROCE_CM_REQ = 0x0010,
    ROCE_CM_REP = 0x0013,
    ROCE_CM_RTU = 0x0014,
};

struct roce_cm_path {
    uint16_t local_lid;
    uint16_t remote_lid;
    uint8_t local_gid[ROCE_CM_GID_SIZE];
    uint8_t remote_gid[ROCE_CM_GID_SIZE];
    uint32_t flow_label;
    uint8_t packet_rate;
    uint8_t traffic_class;
    uint8_t hop_limit;
    uint8_t service_level;
    bool subnet_local;
    uint8_t ack_timeout;
};

struct roce_cm_connection {
    uint64_t local_ca_guid;
    uint32_t local_qpn;
    uint32_t starting_psn;
    uint32_t ece_vendor_id;
    uint32_t ece_attr_mod;
    uint8_t responder_resources;
    uint8_t initiator_depth;
    uint8_t rnr_retry_count;
    bool flow_control;
    bool srq;
};

/* Integers use host byte order. QPN, PSN and ECE vendor ID are 24-bit values. */
struct roce_cm_message {
    enum roce_cm_type type;
    uint64_t tid;
    uint32_t local_comm_id;
    uint32_t remote_comm_id; /* REP and RTU. */
    struct roce_cm_connection connection; /* REQ and REP. */
    union {
        struct {
            uint64_t service_id;
            struct roce_cm_path primary;
            uint16_t pkey;
            uint8_t mtu; /* IB MTU code, 1 through 5. */
            uint8_t remote_response_timeout;
            uint8_t local_response_timeout;
            uint8_t retry_count;
            uint8_t max_cm_retries;
        } req;
        struct {
            uint8_t target_ack_delay;
            uint8_t failover_accepted;
        } rep;
    };
    const uint8_t *private_data;
    size_t private_length;
};

/* Encodes one RC MAD with a primary path and zero-fills unused bytes.
 * Private data may reference the output buffer. */
size_t roce_cm_encode(uint8_t *mad, size_t capacity, const struct roce_cm_message *message);

/* Decodes exactly one MAD. The returned private region borrows the input MAD
 * and spans the complete fixed-width wire field, including any padding. */
bool roce_cm_decode(struct roce_cm_message *message, const uint8_t *mad, size_t length);

/* The Linux RDMA CM IPv4 prefix in the REQ's private data. */
struct roce_cma_ipv4 {
    uint16_t source_port;
    uint32_t source_ipv4;
    uint32_t destination_ipv4;
};

bool roce_cma_encode_ipv4(uint8_t *data, size_t capacity, const struct roce_cma_ipv4 *addresses);
bool roce_cma_decode_ipv4(struct roce_cma_ipv4 *addresses, const uint8_t *data, size_t length);

#endif
