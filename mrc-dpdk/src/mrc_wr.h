#ifndef MRC_WR_H
#define MRC_WR_H

#include "mrc_wire.h"

enum mrc_wr_result {
    MRC_WR_OK,
    MRC_WR_NO_RESOURCE,
    MRC_WR_INVALID,
    MRC_WR_REMOTE_ACCESS,
    MRC_WR_IMM_LIMIT,
    MRC_WR_RQ_EMPTY,
    MRC_WR_QP_ERROR,
};

struct mrc_wr_region {
    uint8_t *memory;
    uint64_t address;
    size_t length;
    uint32_t r_key;
};

struct mrc_wr_slot {
    uint64_t base_va;
    size_t staged_head;
    uint32_t first_psn;
    uint32_t dmalen;
    uint32_t packets;
    uint32_t placed;
    uint32_t immediate;
    uint16_t msn;
    uint16_t rqmsn;
    bool used;
    bool anchored;
    bool uniform_rqmsn;
    bool final_seen;
    bool with_imm;
};

struct mrc_wr_staged {
    uint64_t va;
    size_t next;
    uint32_t psn;
};

struct mrc_wr_config {
    struct mrc_wr_region region;
    uint32_t pmtu;
    uint32_t initial_psn;
    uint32_t initial_msn; /* Full 24-bit MSN; METH carries its low 16 bits. */
    uint16_t initial_rqmsn;
    size_t max_wimm_inflight;
    struct mrc_wr_slot *work;
    size_t work_capacity;
    struct mrc_wr_staged *staged;
    uint8_t *staged_payload; /* staged_capacity * pmtu bytes */
    size_t staged_capacity;
    uint64_t *recv_queue;
    size_t recv_capacity;
};

struct mrc_wr_responder {
    struct mrc_wr_config config;
    uint32_t next_psn;
    uint32_t next_msn;
    uint16_t next_rqmsn;
    size_t free_stage;
    size_t wimm_inflight;
    size_t recv_head;
    size_t recv_posted;
    bool failed;
};

struct mrc_wr_completion {
    uint64_t wr_id;
    uint32_t msn;
    uint32_t last_psn;
    uint32_t byte_len;
    uint32_t immediate;
};

bool mrc_wr_init(struct mrc_wr_responder *responder, const struct mrc_wr_config *config);
bool mrc_wr_post_receive(struct mrc_wr_responder *responder, uint64_t wr_id);

/* Caller filters non-NEW PSNs using the reliability window. A successful receive
 * may retain an unanchored MIDDLE packet in caller-provided staging storage. */
enum mrc_wr_result mrc_wr_receive(struct mrc_wr_responder *responder, uint32_t psn,
                                  uint8_t opcode, const struct mrc_wire_write *write);

/* Pass the reliability cumulative ACK after accepting a packet. Poll again with
 * the same value while produced is true. Send a successful Transport ACK only
 * after all corresponding calls have succeeded. */
enum mrc_wr_result mrc_wr_advance(struct mrc_wr_responder *responder, uint32_t delivery_cack,
                                  struct mrc_wr_completion *completion, bool *produced);
uint32_t mrc_wr_ack_msn(const struct mrc_wr_responder *responder);

#endif
