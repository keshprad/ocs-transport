#ifndef MRC_CC_H
#define MRC_CC_H

#include <stdbool.h>
#include <stdint.h>

#define MRC_CC_NOMINAL_HDRSIZE 40u
#define MRC_CC_RECVD_UNIT 256u
#define MRC_CC_RECVD_MASK UINT32_C(0x00ffffff)
#define MRC_CC_TIMESTAMP_UNIT_NS UINT64_C(128)

/* OCP MRC 1.0 §8.3.1 and §8.6. Pass the untrimmed UDP Length field. */
uint32_t mrc_cc_nominal_size(uint16_t udp_length);
/* OCP MRC 1.0 §7.5.5.8. Encode a service duration or adjust a wire timestamp. */
uint16_t mrc_cc_service_timestamp(uint64_t elapsed_ns, bool report, bool timestamp_present,
                                  bool tsr, uint16_t timestamp);

struct mrc_cc_rx {
    uint32_t received_bytes;
};

/* Count accepted, nonduplicate data only, with the same nominal size as TX. */
void mrc_cc_rx_record(struct mrc_cc_rx *rx, uint32_t nominal_bytes,
                      bool new_data, bool trimmed);
uint32_t mrc_cc_rx_wire_count(const struct mrc_cc_rx *rx);

enum mrc_cc_algorithm {
    MRC_CC_DISABLED,
    MRC_CC_NSCC,
};

struct mrc_cc_config {
    uint8_t disable_qa;
    uint32_t adjust_bytes_threshold;
    uint32_t adjust_period_threshold;
    uint32_t base_rtt;
    float eta;
    float fi;
    float fi_scale;
    float gamma;
    float max_md_jump;
    uint32_t max_cwnd;
    uint8_t qa_gate;
    uint32_t qa_threshold;
    uint32_t target_delay;
};

struct mrc_cc_delay_window {
    double sum_ns;
    uint64_t samples;
};

struct mrc_cc_quick_adapt {
    uint64_t end_ns;
    uint64_t achieved_bytes;
    uint64_t bytes_ignored;
    uint32_t bytes_to_ignore;
    uint32_t threshold_ns;
    uint32_t trim_delay_ns;
    uint8_t gate;
    bool enabled;
    bool trim_pending;
};

struct mrc_cc_nscc {
    uint32_t max_wnd;
    uint32_t saved_cwnd;
    uint64_t adjust_period_ns;
    uint32_t adjust_bytes_threshold;
    double eta;
    double fi;
    double fi_scale;
    double gamma;
    double max_md_jump;
    uint64_t sender_link_bytes_per_second;
    double scaling_a;
    double inc_bytes;
    uint64_t received_bytes;
    uint64_t fast_bytes;
    uint64_t last_adjust_ns;
    uint64_t last_decrease_ns;
    uint64_t last_ack_ns;
    uint64_t delay_start_ns;
    struct mrc_cc_delay_window previous_delay;
    struct mrc_cc_delay_window current_delay;
    struct mrc_cc_quick_adapt qa;
    bool clock_started;
    bool increase_mode;
};

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
    enum mrc_cc_algorithm algorithm;
    struct mrc_cc_nscc nscc;
};

enum mrc_cc_adjustment {
    MRC_CC_NO_ADJUSTMENT,
    MRC_CC_PROPORTIONAL_INCREASE,
    MRC_CC_FAIR_INCREASE,
    MRC_CC_MULTIPLICATIVE_DECREASE,
};

struct mrc_cc_tx_sample {
    uint64_t sent_at_ns;
    uint64_t arrived_at_ns; /* Local RX poll time. Zero uses the processing time. */
    uint64_t service_time_ns; /* Decoded responder duration, or zero without negotiation. */
    uint32_t retransmissions;
};

struct mrc_cc_receiver_flow {
    uint8_t penalty;
    bool restore;
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
bool mrc_cc_config_valid(const struct mrc_cc_config *config, uint32_t nominal_mtu);
/* Apply a controller configuration before sending. NULL disables NSCC. */
bool mrc_cc_apply_config(struct mrc_cc *cc, const struct mrc_cc_config *config);
/*
 * Set link-derived BDP and MaxWnd before sending. A zero receiver rate uses
 * the sender rate. With no explicit rate, init uses initial_cwnd as MaxWnd
 * and derives the tuning BDP from it. The ACK algorithm works in either mode.
 */
bool mrc_cc_configure_nscc(struct mrc_cc *cc, uint64_t sender_link_bytes_per_second,
                            uint64_t receiver_link_bytes_per_second,
                            uint64_t config_base_rtt_ns, uint64_t now_ns);
bool mrc_cc_can_send(const struct mrc_cc *cc, uint32_t nominal_bytes);
bool mrc_cc_on_send(struct mrc_cc *cc, uint32_t nominal_bytes);
/* Include the packet being prepared when computing the remaining window. */
bool mrc_cc_ack_request(const struct mrc_cc *cc, uint32_t pending_bytes, uint32_t ack_trigger_bytes);

/*
 * SACKs use local packet timestamps and the echoed RTX flag (OCP §8.4).
 * Use NULL for missing TX state or a probe response without a local timestamp.
 * ECN is the SACK SKIP mark, separate from an ALWAYS_SKIP path decision.
 * Fresh RTT/ECN feedback drives UE 1.0.1 §3.6.13 window updates, including
 * Quick Adapt when enabled. Pass receiver flow-control feedback even when the RTT
 * sample is unavailable. The caller handles the recommended probe for
 * obtaining an unloaded RTT.
 */
bool mrc_cc_on_sack(struct mrc_cc *cc, uint32_t rcvd_bytes, bool ecn, bool rtx,
                    uint64_t arrive_ns, const struct mrc_cc_tx_sample *sample,
                    const struct mrc_cc_receiver_flow *flow, struct mrc_cc_feedback *feedback);

/*
 * Declare each transmission lost at most once. Pass 0 to mrc_cc_on_nack if
 * delivery was already accounted for. Trimming can trigger Quick Adapt
 * or lower cwnd by one packet. Other NACK reasons only retire inflight bytes.
 */
bool mrc_cc_on_nack(struct mrc_cc *cc, uint32_t nominal_bytes, bool trimmed,
                    bool rtx, uint64_t arrive_ns, const struct mrc_cc_tx_sample *sample);
bool mrc_cc_on_inferred_loss(struct mrc_cc *cc, uint32_t nominal_bytes);

/* next_nominal_bytes is 0 if neither new data nor a retransmit is queued. */
enum mrc_cc_qp_state mrc_cc_scheduler_state(const struct mrc_cc *cc,
                                           uint32_t next_nominal_bytes,
                                           bool outstanding);

#endif
