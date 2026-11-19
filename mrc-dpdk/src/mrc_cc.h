#ifndef MRC_CC_H
#define MRC_CC_H

#include <stdbool.h>
#include <stdint.h>

#define MRC_CC_NOMINAL_HDRSIZE 40u
#define MRC_CC_RECVD_UNIT 256u
#define MRC_CC_RECVD_MASK UINT32_C(0x00ffffff)

/* OCP MRC 1.0 §8.3.1 and §8.6. Pass the untrimmed UDP Length field. */
uint32_t mrc_cc_nominal_size(uint16_t udp_length);

struct mrc_cc_rx {
    uint32_t received_bytes;
};

/* Count accepted, nonduplicate data only, with the same nominal size as TX. */
void mrc_cc_rx_record(struct mrc_cc_rx *rx, uint32_t nominal_bytes,
                      bool new_data, bool trimmed);
uint32_t mrc_cc_rx_wire_count(const struct mrc_cc_rx *rx);

/*
 * Per-QP NSCC accounting for one QP/QPCC. The signed 32-bit inflight
 * counter permits small negative values from ACK counter rounding.
 */
struct mrc_cc {
    int32_t inflight;
    uint32_t prev_rcvd_bytes;
    uint32_t cwnd;
    uint32_t nominal_mtu;
    uint64_t base_rtt_ns;
    uint64_t target_queue_delay_ns;
};

enum mrc_cc_adjustment {
    MRC_CC_NO_ADJUSTMENT,
    MRC_CC_PROPORTIONAL_INCREASE,
    MRC_CC_FAIR_INCREASE,
    MRC_CC_MULTIPLICATIVE_DECREASE,
};

struct mrc_cc_tx_sample {
    uint64_t sent_at_ns;
    uint32_t retransmissions;
};

struct mrc_cc_feedback {
    uint32_t newly_received_bytes;
    bool rtt_valid;
    uint64_t rtt_ns;
    uint64_t queue_delay_ns;
    enum mrc_cc_adjustment adjustment;
};

enum mrc_cc_qp_state {
    MRC_CC_QP_IDLE,
    MRC_CC_QP_READY,
    MRC_CC_QP_ACTIVE,
    MRC_CC_QP_PENDING,
};

bool mrc_cc_init(struct mrc_cc *cc, uint32_t nominal_mtu, uint32_t initial_cwnd,
                 uint64_t initial_base_rtt_ns, uint64_t target_queue_delay_ns);
bool mrc_cc_can_send(const struct mrc_cc *cc, uint32_t nominal_bytes);
bool mrc_cc_on_send(struct mrc_cc *cc, uint32_t nominal_bytes);
/* Evaluate after mrc_cc_on_send(). ACK threshold is expressed in bytes. */
bool mrc_cc_ack_request(const struct mrc_cc *cc, uint32_t ack_trigger_bytes);

/*
 * SACKs use local packet timestamps and the echoed RTX flag (OCP §8.4).
 * Use NULL for missing TX state or a probe response without a local timestamp.
 * ECN is the SACK SKIP mark, separate from an ALWAYS_SKIP path decision.
 * The returned adjustment classifies feedback. Applying the NSCC increase,
 * decrease, quick adaptation and receiver penalty requires a window algorithm.
 */
bool mrc_cc_on_sack(struct mrc_cc *cc, uint32_t rcvd_bytes, bool ecn, bool rtx,
                    uint64_t arrive_ns, const struct mrc_cc_tx_sample *sample,
                    struct mrc_cc_feedback *feedback);

/*
 * Declare each transmission lost at most once. Pass 0 to mrc_cc_on_nack if
 * delivery was already accounted for. Trimming lowers cwnd by one packet
 * in the absence of RCCC. Other NACK reasons only retire inflight bytes.
 */
bool mrc_cc_on_nack(struct mrc_cc *cc, uint32_t nominal_bytes, bool trimmed,
                    bool rtx, uint64_t arrive_ns, const struct mrc_cc_tx_sample *sample);
bool mrc_cc_on_inferred_loss(struct mrc_cc *cc, uint32_t nominal_bytes);

/* next_nominal_bytes is 0 if neither new data nor a retransmit is queued. */
enum mrc_cc_qp_state mrc_cc_scheduler_state(const struct mrc_cc *cc,
                                           uint32_t next_nominal_bytes,
                                           bool outstanding);

#endif
