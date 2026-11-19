#include "mrc_cc.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

static void test_nominal_size_and_receive_counter(void)
{
    struct mrc_cc_rx rx = {0};
    assert(mrc_cc_nominal_size(7) == 0);
    assert(mrc_cc_nominal_size(8) == 48);
    assert(mrc_cc_nominal_size(1000) == 1040);
    assert(mrc_cc_nominal_size(UINT16_MAX) == 65575);
    assert(mrc_cc_rx_wire_count(&rx) == 0);

    mrc_cc_rx_record(&rx, 48, true, false);
    assert(mrc_cc_rx_wire_count(&rx) == 1);
    mrc_cc_rx_record(&rx, 1040, false, false);
    mrc_cc_rx_record(&rx, 1040, true, true);
    assert(rx.received_bytes == 48);

    mrc_cc_rx_record(&rx, 208, true, false);
    assert(mrc_cc_rx_wire_count(&rx) == 1);
    mrc_cc_rx_record(&rx, 48, true, false);
    assert(mrc_cc_rx_wire_count(&rx) == 2);

    rx.received_bytes = UINT32_MAX - 255u;
    assert(mrc_cc_rx_wire_count(&rx) == MRC_CC_RECVD_MASK);
    mrc_cc_rx_record(&rx, 48, true, false);
    assert(mrc_cc_rx_wire_count(&rx) == 0);
    mrc_cc_rx_record(&rx, 208, true, false);
    assert(rx.received_bytes == 0);
    assert(mrc_cc_rx_wire_count(&rx) == 0);
    mrc_cc_rx_record(&rx, 48, true, false);
    assert(mrc_cc_rx_wire_count(&rx) == 1);
}

static void test_window_and_scheduler(void)
{
    struct mrc_cc cc;
    assert(!mrc_cc_init(&cc, 47, 3000, 0, 100));
    assert(!mrc_cc_init(&cc, 1500, 1400, 0, 100));
    assert(!mrc_cc_init(&cc, 1500, UINT32_MAX, 0, 100));
    assert(!mrc_cc_init(&cc, 1500, 3000, 0, 0));
    assert(mrc_cc_init(&cc, 1500, 3000, 0, 100));
    assert(mrc_cc_scheduler_state(&cc, 0, false) == MRC_CC_QP_IDLE);
    assert(mrc_cc_scheduler_state(&cc, 1500, false) == MRC_CC_QP_READY);
    assert(!mrc_cc_on_send(&cc, 0));
    assert(!mrc_cc_on_send(&cc, 1501));

    assert(mrc_cc_on_send(&cc, 1500));
    assert(!mrc_cc_ack_request(&cc, 0, 3000));
    assert(mrc_cc_ack_request(&cc, 0, 3001));
    assert(mrc_cc_scheduler_state(&cc, 1500, true) == MRC_CC_QP_READY);
    assert(mrc_cc_ack_request(&cc, 1500, 3000));
    assert(cc.inflight == 1500);
    assert(mrc_cc_on_send(&cc, 1500));
    assert(!mrc_cc_on_send(&cc, 48));
    assert(cc.inflight == 3000);
    assert(mrc_cc_ack_request(&cc, 0, 1));
    assert(mrc_cc_scheduler_state(&cc, 1500, true) == MRC_CC_QP_ACTIVE);
    assert(mrc_cc_scheduler_state(&cc, 0, true) == MRC_CC_QP_PENDING);

    assert(mrc_cc_on_sack(&cc, 5, false, false, 0, NULL, NULL, NULL));
    assert(cc.inflight == 1720);
    assert(!mrc_cc_can_send(&cc, 1500));
    assert(mrc_cc_can_send(&cc, 1280));
    assert(mrc_cc_scheduler_state(&cc, 1280, true) == MRC_CC_QP_READY);
}

static void test_ack_clock_rounding_and_wrap(void)
{
    struct mrc_cc cc;
    struct mrc_cc_feedback feedback;
    struct mrc_cc_rx rx = {0};
    assert(mrc_cc_init(&cc, 1040, 2080, 0, 100));
    assert(mrc_cc_on_send(&cc, 1040));
    mrc_cc_rx_record(&rx, 1040, true, false);
    assert(mrc_cc_on_sack(&cc, mrc_cc_rx_wire_count(&rx), false, false, 0, NULL, NULL,
                          &feedback));
    assert(feedback.newly_received_bytes == 1280);
    assert(cc.inflight == -240);

    assert(mrc_cc_on_send(&cc, 1040));
    mrc_cc_rx_record(&rx, 1040, true, false);
    assert(mrc_cc_on_sack(&cc, mrc_cc_rx_wire_count(&rx), false, false, 0, NULL, NULL,
                          &feedback));
    assert(feedback.newly_received_bytes == 1024);
    assert(cc.inflight == -224);

    cc.prev_rcvd_bytes = MRC_CC_RECVD_MASK - 1u;
    assert(mrc_cc_on_send(&cc, 1040));
    assert(mrc_cc_on_sack(&cc, 1, false, false, 0, NULL, NULL, &feedback));
    assert(feedback.newly_received_bytes == 768);
    assert(cc.prev_rcvd_bytes == 1);
    assert(cc.inflight == 48);

    const uint32_t out_of_order_or_duplicate[] = {MRC_CC_RECVD_MASK, 1, UINT32_C(0x800001)};
    for (size_t i = 0; i < sizeof(out_of_order_or_duplicate) /
                           sizeof(out_of_order_or_duplicate[0]); i++) {
        assert(mrc_cc_on_sack(&cc, out_of_order_or_duplicate[i], false, false, 0,
                              NULL, NULL, &feedback));
        assert(feedback.newly_received_bytes == 0);
        assert(cc.prev_rcvd_bytes == 1);
        assert(cc.inflight == 48);
    }
    assert(!mrc_cc_on_sack(&cc, MRC_CC_RECVD_MASK + 1u, false, false, 0,
                           NULL, NULL, &feedback));

    cc.inflight = INT32_MIN;
    assert(!mrc_cc_on_sack(&cc, 2, false, false, 0, NULL, NULL, &feedback));
    assert(cc.inflight == INT32_MIN);
    assert(cc.prev_rcvd_bytes == 1);
}

static void test_rtt_and_ecn(void)
{
    struct mrc_cc cc;
    struct mrc_cc_feedback feedback;
    struct mrc_cc_tx_sample sample = {.sent_at_ns = 10};
    assert(mrc_cc_init(&cc, 1040, 2080, 0, 100));
    assert(mrc_cc_on_sack(&cc, 0, false, false, 110, &sample, NULL, &feedback));
    assert(feedback.rtt_valid);
    assert(feedback.rtt_ns == 100);
    assert(cc.base_rtt_ns == 100);
    assert(feedback.queue_delay_ns == 0);
    assert(feedback.adjustment == MRC_CC_PROPORTIONAL_INCREASE);

    assert(mrc_cc_on_sack(&cc, 0, true, false, 209, &sample, NULL, &feedback));
    assert(feedback.queue_delay_ns == 99);
    assert(feedback.adjustment == MRC_CC_NO_ADJUSTMENT);
    assert(mrc_cc_on_sack(&cc, 0, true, false, 210, &sample, NULL, &feedback));
    assert(feedback.queue_delay_ns == 100);
    assert(feedback.adjustment == MRC_CC_MULTIPLICATIVE_DECREASE);
    assert(mrc_cc_on_sack(&cc, 0, false, false, 210, &sample, NULL, &feedback));
    assert(feedback.adjustment == MRC_CC_FAIR_INCREASE);

    sample.sent_at_ns = 200;
    assert(mrc_cc_on_sack(&cc, 0, false, true, 281, &sample, NULL, &feedback));
    assert(!feedback.rtt_valid);
    sample.retransmissions = 1;
    assert(mrc_cc_on_sack(&cc, 1, false, false, 281, &sample, NULL, &feedback));
    assert(!feedback.rtt_valid);
    assert(feedback.newly_received_bytes == 256);
    assert(cc.base_rtt_ns == 100);
    assert(mrc_cc_on_sack(&cc, 1, false, true, 281, &sample, NULL, &feedback));
    assert(feedback.rtt_valid);
    assert(cc.base_rtt_ns == 81);
    sample.retransmissions = 2;
    assert(mrc_cc_on_sack(&cc, 1, false, true, 281, &sample, NULL, &feedback));
    assert(!feedback.rtt_valid);

    sample.retransmissions = 0;
    assert(mrc_cc_on_sack(&cc, 1, false, false, sample.sent_at_ns, &sample, NULL, &feedback));
    assert(!feedback.rtt_valid);
    assert(mrc_cc_on_sack(&cc, 1, false, false, 281, NULL, NULL, &feedback));
    assert(!feedback.rtt_valid);
}

static void test_loss_and_trim(void)
{
    struct mrc_cc cc;
    struct mrc_cc_tx_sample sample = {.sent_at_ns = 100};
    assert(mrc_cc_init(&cc, 1040, 3120, 0, 100));
    for (unsigned int i = 0; i < 3; i++)
        assert(mrc_cc_on_send(&cc, 1040));
    assert(mrc_cc_on_nack(&cc, 1040, false, false, 130, &sample));
    assert(cc.inflight == 2080);
    assert(cc.cwnd == 3120);
    assert(cc.base_rtt_ns == 30);

    assert(mrc_cc_on_inferred_loss(&cc, 1040));
    assert(cc.inflight == 1040);
    assert(cc.cwnd == 2080);

    assert(mrc_cc_on_nack(&cc, 1040, true, false, 140, &sample));
    assert(cc.inflight == 0);
    assert(cc.cwnd == 1040);
    assert(cc.base_rtt_ns == 30);

    assert(mrc_cc_on_nack(&cc, 0, true, false, 0, NULL));
    assert(cc.inflight == 0);
    assert(cc.cwnd == 1040);
    assert(!mrc_cc_on_nack(&cc, 1041, true, false, 0, NULL));
    assert(!mrc_cc_on_inferred_loss(&cc, 0));
    assert(mrc_cc_on_send(&cc, 1000));
    assert(mrc_cc_on_inferred_loss(&cc, 1000));
    assert(cc.inflight == 0);
    assert(cc.cwnd == cc.nominal_mtu);
}

static void test_nscc_increase_formulas(void)
{
    struct mrc_cc cc;
    struct mrc_cc_tx_sample sample = {.sent_at_ns = 1};
    const uint64_t line_rate = UINT64_C(12500000000);

    assert(mrc_cc_init(&cc, 1000, 100000, 12000, 12000));
    assert(mrc_cc_configure_nscc(&cc, line_rate, 0, 12000, 0));
    assert(cc.nscc.max_wnd == 225000);
    assert(mrc_cc_on_sack(&cc, 4, false, false, 18001, &sample, NULL, NULL));
    /* PI: floor(4 * 1000 / 12000 * 1024 * 6000 / 100000 + 150). */
    assert(cc.cwnd == 100170);

    assert(mrc_cc_init(&cc, 1000, 100000, 12000, 12000));
    assert(mrc_cc_configure_nscc(&cc, line_rate, 0, 12000, 0));
    assert(mrc_cc_on_sack(&cc, 4, false, false, 25001, &sample, NULL, NULL));
    /* Fair: floor(5 * 1000 * 1024 / 100000 + 150). */
    assert(cc.cwnd == 100201);

    assert(mrc_cc_init(&cc, 1024, 100000, 12000, 12000));
    assert(mrc_cc_configure_nscc(&cc, line_rate, 0, 12000, 1000));
    assert(mrc_cc_on_sack(&cc, 32, false, false, 12001, &sample, NULL, NULL));
    assert(cc.cwnd == 100000); /* Exactly eight nominal MTUs. */
    assert(mrc_cc_on_sack(&cc, 33, false, false, 12002, &sample, NULL, NULL));
    assert(cc.cwnd == 100346); /* Byte threshold, no periodic eta. */

    assert(mrc_cc_init(&cc, 1000, 2000, 12000, 12000));
    assert(mrc_cc_configure_nscc(&cc, line_rate, 0, 12000, 1000));
    assert(mrc_cc_on_sack(&cc, 4, false, false, 12001, &sample, NULL, NULL));
    assert(cc.cwnd == 2000);
    assert(mrc_cc_on_sack(&cc, 8, false, false, 12002, &sample, NULL, NULL));
    assert(cc.cwnd == 2256); /* Fast increase: 1024 * 0.25. */
}

static void test_nscc_decrease_and_staleness(void)
{
    struct mrc_cc cc;
    struct mrc_cc_tx_sample sample = {.sent_at_ns = 1};
    struct mrc_cc_feedback feedback;
    const uint64_t line_rate = UINT64_C(12500000000);

    assert(mrc_cc_init(&cc, 1000, 100000, 12000, 12000));
    assert(mrc_cc_configure_nscc(&cc, line_rate, 0, 12000, 0));
    assert(mrc_cc_on_sack(&cc, 4, true, false, 36001, &sample, NULL, &feedback));
    assert(feedback.adjustment == MRC_CC_MULTIPLICATIVE_DECREASE);
    /* 100000 * (1 - 0.8 * (24000 - 12000) / 24000) + 150. */
    assert(cc.cwnd == 60150);
    assert(mrc_cc_on_sack(&cc, 5, true, false, 48001, &sample, NULL, NULL));
    assert(cc.cwnd == 60300); /* Exactly one base RTT, periodic eta only. */
    assert(mrc_cc_on_sack(&cc, 6, true, false, 48002, &sample, NULL, NULL));
    assert(cc.cwnd < 60300);
    assert(cc.nscc.last_decrease_ns == 48002);

    const uint32_t old_cwnd = cc.cwnd;
    const int32_t old_inflight = cc.inflight;
    assert(mrc_cc_on_sack(&cc, 5, true, false, 1000000, &sample, NULL, &feedback));
    assert(!feedback.rtt_valid);
    assert(feedback.newly_received_bytes == 0);
    assert(cc.cwnd == old_cwnd);
    assert(cc.inflight == old_inflight);
    assert(cc.nscc.last_ack_ns == 48002);

    sample.retransmissions = 1;
    assert(mrc_cc_on_sack(&cc, 7, true, false, 1000001, &sample, NULL, &feedback));
    assert(!feedback.rtt_valid);
    assert(cc.cwnd == old_cwnd);
    assert(cc.inflight == old_inflight - 256); /* ACK-clock still advances. */
    assert(cc.nscc.last_ack_ns == 48002);
    assert(mrc_cc_on_sack(&cc, 7, true, true, 1000002, &sample, NULL, &feedback));
    assert(feedback.rtt_valid);
    assert(cc.cwnd == (old_cwnd / 2) + 150); /* Limit any decrease to 50%. */
}

static void test_nscc_decrease_uses_recent_delay_average(void)
{
    struct mrc_cc cc;
    struct mrc_cc_tx_sample sample = {.sent_at_ns = 24000};
    struct mrc_cc_feedback feedback;
    const uint64_t line_rate = UINT64_C(12500000000);

    assert(mrc_cc_init(&cc, 1000, 100000, 12000, 12000));
    assert(mrc_cc_configure_nscc(&cc, line_rate, 0, 12000, 0));
    assert(mrc_cc_on_sack(&cc, 0, false, false, 36000, &sample, NULL, NULL));
    sample.sent_at_ns = 24001;
    assert(mrc_cc_on_sack(&cc, 0, false, false, 36001, &sample, NULL, NULL));

    sample.sent_at_ns = 2;
    assert(mrc_cc_on_sack(&cc, 0, true, false, 36002, &sample, NULL, &feedback));
    assert(feedback.adjustment == MRC_CC_MULTIPLICATIVE_DECREASE);
    assert(cc.cwnd == 100150); /* Mean queueing delay is 8 us, under the 12 us target. */

    sample.sent_at_ns = 36004;
    assert(mrc_cc_on_sack(&cc, 0, true, false, 72004, &sample, NULL, NULL));
    assert(cc.cwnd == 60240); /* Older low-delay samples have expired. */
}

static void test_nscc_link_derived_bounds(void)
{
    struct mrc_cc cc;
    struct mrc_cc_tx_sample sample = {.sent_at_ns = 2000};
    const uint64_t sender_rate = UINT64_C(10000000000);
    const uint64_t receiver_rate = UINT64_C(5000000000);

    assert(mrc_cc_init(&cc, 1000, 100000, 12000, 12000));
    assert(!mrc_cc_configure_nscc(&cc, 0, receiver_rate, 12000, 0));
    assert(!mrc_cc_configure_nscc(&cc, sender_rate, receiver_rate, 0, 0));
    assert(mrc_cc_configure_nscc(&cc, sender_rate, receiver_rate, 12000, 0));
    assert(cc.nscc.max_wnd == 90000);
    assert(cc.cwnd == 90000);

    assert(mrc_cc_on_sack(&cc, 4, false, false, 5000, &sample, NULL, NULL));
    assert(cc.base_rtt_ns == 3000);
    assert(cc.nscc.max_wnd == 45000);
    assert(cc.cwnd == 45000);

    assert(mrc_cc_on_sack(&cc, 5, true, false, 80000, &sample, NULL, NULL));
    assert(cc.cwnd < 45000);
    sample.sent_at_ns = 81000;
    assert(mrc_cc_on_sack(&cc, 6, false, false, 81001, &sample, NULL, NULL));
    assert(cc.base_rtt_ns == 1);
    assert(cc.nscc.max_wnd == cc.nominal_mtu);
    assert(cc.cwnd == cc.nominal_mtu);
}

static void test_controller_config(void)
{
    struct mrc_cc cc;
    struct mrc_cc_config config = {
        .disable_qa = 1, .adjust_bytes_threshold = 1024, .adjust_period_threshold = 5000,
        .base_rtt = 1000, .target_delay = 1000, .max_cwnd = 4000,
        .fi = 200, .eta = 15, .fi_scale = 0.125f, .gamma = 1, .max_md_jump = 0.75f,
    };
    assert(mrc_cc_init(&cc, 1000, 10000, 12000, 12000));
    assert(mrc_cc_apply_config(&cc, &config));
    assert(cc.cwnd == 4000);
    for (unsigned int i = 0; i < 4; i++)
        assert(mrc_cc_on_send(&cc, 1000));
    assert(!mrc_cc_can_send(&cc, 1000));

    struct mrc_cc_tx_sample sample = { .sent_at_ns = 1 };
    struct mrc_cc_feedback feedback;
    assert(mrc_cc_on_sack(&cc, 4, true, false, 4001, &sample, NULL, &feedback));
    assert(feedback.queue_delay_ns == 3000);
    assert(cc.cwnd == 4000); /* First sample starts the decrease interval. */
    assert(mrc_cc_on_sack(&cc, 8, true, false, 6001, &sample, NULL, NULL));
    assert(cc.cwnd == 3000); /* Gamma reaches the configured 0.75 floor. */
    assert(mrc_cc_on_sack(&cc, 12, false, false, 7001, &sample, NULL, NULL));
    assert(cc.cwnd == 3000); /* Exactly the configured byte threshold. */
    assert(mrc_cc_on_sack(&cc, 13, false, false, 7002, &sample, NULL, NULL));
    assert(cc.cwnd == 3085); /* floor(200 * 1280 / 3000). */
    assert(mrc_cc_on_sack(&cc, 13, false, false, 9001, &sample, NULL, NULL));
    assert(cc.cwnd == 3100); /* Configured time threshold adds eta. */

    sample.sent_at_ns = 8999;
    assert(mrc_cc_on_sack(&cc, 29, false, false, 9001, &sample, NULL, NULL));
    assert(cc.cwnd == 3612); /* Near-base RTT adds 4096 * 0.125. */
    assert(mrc_cc_on_sack(&cc, 45, false, false, 9001, &sample, NULL, NULL));
    assert(cc.cwnd == config.max_cwnd);

    config.max_cwnd = 999;
    assert(!mrc_cc_apply_config(&cc, &config));
    assert(cc.cwnd == 4000);

    assert(mrc_cc_init(&cc, 1000, 10000, 12000, 12000));
    assert(mrc_cc_apply_config(&cc, NULL));
    const uint32_t uncongested_cwnd = cc.cwnd;
    assert(mrc_cc_on_send(&cc, 1000));
    assert(mrc_cc_on_send(&cc, 1000));
    assert(mrc_cc_on_send(&cc, 1000));
    assert(mrc_cc_on_nack(&cc, 1000, true, false, 6000, NULL));
    assert(mrc_cc_on_inferred_loss(&cc, 1000));
    assert(mrc_cc_on_sack(&cc, 4, true, false, 9000, &sample, NULL, &feedback));
    assert(feedback.newly_received_bytes == 1024);
    assert(cc.inflight == -24);
    assert(cc.cwnd == uncongested_cwnd);
}

static void test_receiver_flow_control(void)
{
    struct mrc_cc cc;
    struct mrc_cc_feedback feedback;
    const struct mrc_cc_receiver_flow penalty = { .penalty = 64, .restore = true };
    const struct mrc_cc_receiver_flow maximum_penalty = { .penalty = 127 };
    const struct mrc_cc_receiver_flow clear = { .restore = true };
    const struct mrc_cc_receiver_flow invalid = { .penalty = 128 };

    assert(mrc_cc_init(&cc, 1024, 8192, 1000, 1000));
    for (unsigned int i = 0; i < 8; i++)
        assert(mrc_cc_on_send(&cc, 1024));
    assert(!mrc_cc_on_sack(&cc, 8, false, false, 0, NULL, &invalid, NULL));
    assert(cc.inflight == 8192 && cc.cwnd == 8192);

    /* Feedback applies without RTT. Clamp to 6144 bytes in flight, then
     * subtract 64/128 of the 2048 newly received bytes. */
    assert(mrc_cc_on_sack(&cc, 8, false, false, 0, NULL, &penalty, &feedback));
    assert(!feedback.rtt_valid && feedback.newly_received_bytes == 2048);
    assert(cc.inflight == 6144 && cc.cwnd == 5120 && cc.nscc.saved_cwnd == 8192);
    assert(!mrc_cc_can_send(&cc, 1024));

    struct mrc_cc_tx_sample sample = { .sent_at_ns = 1 };
    assert(mrc_cc_on_sack(&cc, 28, false, false, 1001, &sample, &maximum_penalty, &feedback));
    assert(feedback.rtt_valid && feedback.adjustment == MRC_CC_NO_ADJUSTMENT);
    assert(cc.inflight == 1024 && cc.cwnd == cc.nominal_mtu);
    assert(cc.nscc.saved_cwnd == 8192 && !mrc_cc_can_send(&cc, 1024));

    /* A reordered older SACK cannot restore the saved window. A current
     * SACK can restore it even if it acknowledges no new bytes. */
    assert(mrc_cc_on_sack(&cc, 8, false, false, 1002, NULL, &clear, NULL));
    assert(cc.cwnd == 1024 && cc.nscc.saved_cwnd == 8192);
    assert(mrc_cc_on_sack(&cc, 28, false, false, 1003, NULL, &clear, NULL));
    assert(cc.cwnd == 8192 && cc.nscc.saved_cwnd == 0 && mrc_cc_can_send(&cc, 1024));

    assert(mrc_cc_on_sack(&cc, 32, false, false, 1004, NULL, &penalty, NULL));
    assert(cc.inflight == 0 && cc.cwnd == cc.nominal_mtu);
    assert(mrc_cc_on_sack(&cc, 33, false, false, 1005, NULL, &penalty, NULL));
    assert(cc.inflight == -256 && cc.cwnd == cc.nominal_mtu);
    assert(mrc_cc_apply_config(&cc, NULL));
    const uint32_t fixed_window = cc.cwnd;
    assert(mrc_cc_on_sack(&cc, 33, false, false, 1006, NULL, &maximum_penalty, NULL));
    assert(mrc_cc_on_sack(&cc, 33, false, false, 1007, NULL, &clear, NULL));
    assert(cc.cwnd == fixed_window);

    /* A deadline coinciding with a penalty must not apply eta or earlier
     * increase credits at the expense of the new receiver limit. */
    assert(mrc_cc_init(&cc, 1024, 8192, 1000, 1000));
    cc.nscc.eta = 2048;
    assert(mrc_cc_on_send(&cc, 1024));
    assert(mrc_cc_on_send(&cc, 1024));
    assert(mrc_cc_on_sack(&cc, 4, false, false, 2501, &sample, NULL, NULL));
    assert(cc.nscc.inc_bytes > 0);
    assert(mrc_cc_on_sack(&cc, 8, false, false, 4001, &sample, &maximum_penalty, NULL));
    assert(cc.cwnd == cc.nominal_mtu && cc.nscc.inc_bytes == 0);
    assert(cc.nscc.last_adjust_ns == 4001);

    /* ECN decrease remains active while receiver feedback limits increase. */
    const struct mrc_cc_receiver_flow small_penalty = { .penalty = 1 };
    assert(mrc_cc_init(&cc, 1024, 8192, 1000, 1000));
    for (unsigned int i = 0; i < 8; i++)
        assert(mrc_cc_on_send(&cc, 1024));
    assert(mrc_cc_on_sack(&cc, 0, false, false, 1001, &sample, NULL, NULL));
    assert(mrc_cc_on_sack(&cc, 4, true, false, 5501, &sample, &small_penalty, &feedback));
    assert(feedback.adjustment == MRC_CC_MULTIPLICATIVE_DECREASE);
    assert(cc.cwnd == 3580); /* Half of (7168 inflight - 8 bytes penalty). */
}

static const struct mrc_cc_config qa_config = {
    .adjust_bytes_threshold = 8192, .adjust_period_threshold = 1000,
    .base_rtt = 1000, .target_delay = 1000, .max_cwnd = 16384,
    .gamma = 0.8f, .max_md_jump = 0.5f, .qa_gate = 2, .qa_threshold = 2000,
};

static void init_quick_adapt(struct mrc_cc *cc, const struct mrc_cc_config *config)
{
    assert(mrc_cc_init(cc, 1024, 4096, 12000, 12000));
    assert(mrc_cc_apply_config(cc, config));
    for (unsigned int i = 0; i < 8; i++)
        assert(mrc_cc_on_send(cc, 1024));
}

static void test_quick_adapt_delay(void)
{
    struct mrc_cc cc;
    struct mrc_cc_tx_sample sample = { .sent_at_ns = 1 };
    struct mrc_cc_feedback feedback;
    init_quick_adapt(&cc, &qa_config);

    assert(mrc_cc_on_sack(&cc, 4, false, false, 1001, &sample, NULL, &feedback));
    assert(feedback.rtt_valid && cc.nscc.qa.end_ns == 3001);
    assert(mrc_cc_on_sack(&cc, 8, false, false, 2601, &sample, NULL, NULL));
    assert(cc.nscc.qa.achieved_bytes == 1024 && cc.cwnd == 16384);
    assert(mrc_cc_on_sack(&cc, 12, false, false, 5001, &sample, NULL, &feedback));
    assert(feedback.queue_delay_ns == 4000);
    assert(cc.cwnd == 2048 && cc.inflight == 5120 && !mrc_cc_can_send(&cc, 1024));
    assert(cc.nscc.qa.end_ns == 7001 && cc.nscc.qa.achieved_bytes == 0);
    assert(cc.nscc.qa.bytes_to_ignore == 5120 && cc.nscc.qa.bytes_ignored == 0);
    assert(cc.nscc.received_bytes == 0 && cc.nscc.inc_bytes == 0);

    /* Feedback for the pre-reduction flight cannot cause another marked
     * decrease. Delivery, inferred loss, and trim each retire ignored bytes. */
    assert(mrc_cc_on_sack(&cc, 16, true, false, 5101, &sample, NULL, &feedback));
    assert(feedback.rtt_valid && feedback.adjustment == MRC_CC_NO_ADJUSTMENT);
    assert(cc.nscc.qa.bytes_ignored == 1024 && cc.cwnd == 2048);
    assert(mrc_cc_on_inferred_loss(&cc, 1024));
    assert(cc.nscc.qa.bytes_ignored == 2048 && cc.cwnd == 1024);
    assert(mrc_cc_on_nack(&cc, 1024, true, false, 5201, NULL));
    assert(cc.nscc.qa.bytes_ignored == 3072 && cc.nscc.qa.trim_pending);
    assert(cc.nscc.qa.end_ns == 7001);

    sample.sent_at_ns = 6301;
    assert(mrc_cc_on_sack(&cc, 24, true, false, 7301, &sample, NULL, NULL));
    assert(!cc.nscc.qa.trim_pending && cc.nscc.qa.end_ns == 9301);
    assert(cc.nscc.qa.bytes_to_ignore == 0 && cc.nscc.qa.bytes_ignored == 0);
    assert(cc.inflight == 0 && cc.cwnd == 1024);
}

static void test_quick_adapt_averages_delay(void)
{
    struct mrc_cc cc;
    struct mrc_cc_tx_sample sample = { .sent_at_ns = 1 };
    struct mrc_cc_feedback feedback;
    init_quick_adapt(&cc, &qa_config);
    assert(mrc_cc_on_sack(&cc, 0, false, false, 1001, &sample, NULL, NULL));

    sample.sent_at_ns = 2000;
    assert(mrc_cc_on_sack(&cc, 0, false, false, 3000, &sample, NULL, NULL));
    sample.sent_at_ns = 1;
    assert(mrc_cc_on_sack(&cc, 4, false, false, 3501, &sample, NULL, &feedback));
    assert(feedback.queue_delay_ns == 2500 && cc.nscc.qa.end_ns == 5501);
    assert(cc.cwnd == 16384); /* The low-delay sample keeps the average below 2000. */

    assert(mrc_cc_on_sack(&cc, 8, false, false, 6001, &sample, NULL, NULL));
    assert(cc.cwnd == cc.nominal_mtu);
}

static void test_quick_adapt_gate_and_flow(void)
{
    struct mrc_cc cc;
    struct mrc_cc_tx_sample sample = { .sent_at_ns = 1 };
    init_quick_adapt(&cc, &qa_config);
    assert(mrc_cc_on_sack(&cc, 0, false, false, 1001, &sample, NULL, NULL));
    assert(mrc_cc_on_sack(&cc, 16, false, false, 5001, &sample, NULL, NULL));
    assert(cc.nscc.qa.end_ns == 7001);
    assert(cc.cwnd == 16384); /* Achieved 4096, exactly MaxWnd >> 2. */

    init_quick_adapt(&cc, &qa_config);
    assert(mrc_cc_on_sack(&cc, 0, false, false, 1001, &sample, NULL, NULL));
    /* A penalty limits the window to 1024 here, below the 3840 bytes
     * achieved in the same SACK. Quick Adapt must not widen it. */
    const struct mrc_cc_receiver_flow penalty = { .penalty = 127 };
    assert(mrc_cc_on_sack(&cc, 15, false, false, 5001, &sample, &penalty, NULL));
    assert(cc.cwnd == 1024 && cc.nscc.saved_cwnd == 16384);

    struct mrc_cc_config disabled = qa_config;
    disabled.qa_gate = 32;
    assert(!mrc_cc_config_valid(&disabled, 1024));
    disabled.disable_qa = 1;
    init_quick_adapt(&cc, &disabled);
    assert(mrc_cc_on_sack(&cc, 0, false, false, 1001, &sample, NULL, NULL));
    assert(mrc_cc_on_sack(&cc, 4, false, false, 5001, &sample, NULL, NULL));
    assert(cc.cwnd == 16384 && cc.nscc.qa.end_ns == 0);
}

static void test_quick_adapt_trim(void)
{
    struct mrc_cc cc;
    struct mrc_cc_config config = qa_config;
    config.qa_threshold = UINT32_MAX;
    init_quick_adapt(&cc, &config);
    assert(mrc_cc_on_nack(&cc, 1024, true, false, 1001, NULL));
    assert(cc.nscc.qa.end_ns == 3001 && cc.nscc.qa.trim_pending);
    assert(cc.cwnd == 15360); /* No prior epoch, use the ordinary trim reduction. */

    assert(mrc_cc_on_sack(&cc, 8, false, false, 1501, NULL, NULL, NULL));
    assert(cc.nscc.qa.achieved_bytes == 2048); /* RTT-free SACK still accounts bytes. */
    assert(mrc_cc_on_nack(&cc, 1024, false, false, 2001, NULL));
    assert(cc.cwnd == 15360 && cc.nscc.qa.bytes_ignored == 3072);
    assert(mrc_cc_on_nack(&cc, 1024, true, false, 3501, NULL));
    assert(cc.cwnd == 2048); /* An expired epoch uses Quick Adapt only. */
    assert(!cc.nscc.qa.trim_pending && cc.nscc.qa.bytes_to_ignore == 3072);
}

int main(void)
{
    test_nominal_size_and_receive_counter();
    test_window_and_scheduler();
    test_ack_clock_rounding_and_wrap();
    test_rtt_and_ecn();
    test_loss_and_trim();
    test_nscc_increase_formulas();
    test_nscc_decrease_and_staleness();
    test_nscc_decrease_uses_recent_delay_average();
    test_nscc_link_derived_bounds();
    test_controller_config();
    test_receiver_flow_control();
    test_quick_adapt_delay();
    test_quick_adapt_averages_delay();
    test_quick_adapt_gate_and_flow();
    test_quick_adapt_trim();
    puts("MRC congestion accounting tests passed");
    return 0;
}
