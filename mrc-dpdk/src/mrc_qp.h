#ifndef MRC_QP_H
#define MRC_QP_H

#include <stdbool.h>
#include <stdint.h>

#define MRC_PSN_MASK UINT32_C(0xffffff)
#define MRC_PSN_HALF UINT32_C(0x800000)
#define MRC_MPR_UNIT 128u
#define MRC_MPR_DEFAULT_UNITS 8u
#define MRC_MPR_MAX_UNITS UINT8_MAX
#define MRC_MPR_MAX (MRC_MPR_MAX_UNITS * MRC_MPR_UNIT)
#define MRC_SACK_BITS 64u

/* The exact half-space is unordered and maps to -MRC_PSN_HALF. */
int32_t mrc_psn_delta(uint32_t psn, uint32_t reference);
uint32_t mrc_psn_add(uint32_t psn, int32_t delta);

enum mrc_rx_disposition {
    MRC_RX_NEW,
    MRC_RX_DUPLICATE,
    MRC_RX_AHEAD_OF_WINDOW,
    MRC_RX_INVALID_PSN,
};

enum mrc_rx_packet_flags {
    MRC_RX_AR = 1u << 0,
    MRC_RX_RTX = 1u << 1,
    MRC_RX_ECN = 1u << 2,
};

struct mrc_sack {
    uint32_t cack_psn;
    int16_t ack_psn_offset;
    int16_t sack_offset;
    uint64_t bitmap;
    uint16_t ooo_count;
};

struct mrc_rx_qp {
    uint8_t *slots;
    uint32_t window;
    uint32_t cack_psn;
    uint32_t max_rcv_psn;
    uint32_t lowest_unsacked_psn;
    uint32_t first_index;
    uint32_t contiguous_history;
    uint16_t ooo_count;
    uint64_t sack_trigger_count;
};

/* Slots must hold window bytes. Window is in packets (a multiple of 128). */
bool mrc_rx_init(struct mrc_rx_qp *qp, uint32_t initial_psn, uint32_t window,
                 uint8_t *slots);
enum mrc_rx_disposition mrc_rx_classify(const struct mrc_rx_qp *qp, uint32_t psn);

/* Record nontrimmed packets eligible for reliability tracking. */
enum mrc_rx_disposition mrc_rx_record(struct mrc_rx_qp *qp, uint32_t psn,
                                      uint32_t payload_bytes, unsigned int flags,
                                      uint32_t sack_threshold, bool *sack_due);

/* Call sack_sent after transmitting the snapshot and before recording more arrivals. */
bool mrc_rx_build_sack(const struct mrc_rx_qp *qp, uint32_t trigger_psn,
                       struct mrc_sack *sack);
void mrc_rx_build_probe_sack(const struct mrc_rx_qp *qp, uint16_t probe_id,
                             struct mrc_sack *sack);
void mrc_rx_sack_sent(struct mrc_rx_qp *qp, const struct mrc_sack *sack);

struct mrc_tx_packet {
    uint64_t sent_at;
    uint32_t retransmissions;
    bool received;
};

struct mrc_tx_qp {
    struct mrc_tx_packet *slots;
    uint32_t window;
    uint32_t first_psn;
    uint32_t delivery_cack_psn;
    uint32_t next_psn;
    uint32_t first_index;
};

/* Slots must hold window packet records. The window also bounds retained sends. */
bool mrc_tx_init(struct mrc_tx_qp *qp, uint32_t initial_psn, uint32_t window,
                 struct mrc_tx_packet *slots);
bool mrc_tx_track(struct mrc_tx_qp *qp, uint32_t psn, uint64_t sent_at);
struct mrc_tx_packet *mrc_tx_packet(const struct mrc_tx_qp *qp, uint32_t psn);

bool mrc_tx_apply_sack(struct mrc_tx_qp *qp, const struct mrc_sack *sack,
                       bool probe_response);
/* Apply only a successful semantic Transport ACK. Returns true if it advances. */
bool mrc_tx_apply_transport_ack(struct mrc_tx_qp *qp, uint32_t psn);
bool mrc_tx_note_retransmit(const struct mrc_tx_qp *qp, uint32_t psn,
                            uint64_t sent_at);

/* Table 7-1. Zero retries means no retransmit after the initial timer expires. */
bool mrc_local_ack_timeout_ns(unsigned int exponent, unsigned int linear_retries,
                              unsigned int exponential_retries,
                              unsigned int retries_sent, uint64_t *timeout);

#endif
