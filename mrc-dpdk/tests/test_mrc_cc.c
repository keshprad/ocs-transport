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
    assert(!mrc_cc_ack_request(&cc, 3000));
    assert(mrc_cc_ack_request(&cc, 3001));
    assert(mrc_cc_scheduler_state(&cc, 1500, true) == MRC_CC_QP_READY);
    assert(mrc_cc_on_send(&cc, 1500));
    assert(!mrc_cc_on_send(&cc, 48));
    assert(cc.inflight == 3000);
    assert(mrc_cc_ack_request(&cc, 1));
    assert(mrc_cc_scheduler_state(&cc, 1500, true) == MRC_CC_QP_ACTIVE);
    assert(mrc_cc_scheduler_state(&cc, 0, true) == MRC_CC_QP_PENDING);

    assert(mrc_cc_on_sack(&cc, 5, false, false, 0, NULL, NULL));
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
    assert(mrc_cc_on_sack(&cc, mrc_cc_rx_wire_count(&rx), false, false, 0, NULL,
                          &feedback));
    assert(feedback.newly_received_bytes == 1280);
    assert(cc.inflight == -240);

    assert(mrc_cc_on_send(&cc, 1040));
    mrc_cc_rx_record(&rx, 1040, true, false);
    assert(mrc_cc_on_sack(&cc, mrc_cc_rx_wire_count(&rx), false, false, 0, NULL,
                          &feedback));
    assert(feedback.newly_received_bytes == 1024);
    assert(cc.inflight == -224);

    cc.prev_rcvd_bytes = MRC_CC_RECVD_MASK - 1u;
    assert(mrc_cc_on_send(&cc, 1040));
    assert(mrc_cc_on_sack(&cc, 1, false, false, 0, NULL, &feedback));
    assert(feedback.newly_received_bytes == 768);
    assert(cc.prev_rcvd_bytes == 1);
    assert(cc.inflight == 48);

    const uint32_t out_of_order_or_duplicate[] = {MRC_CC_RECVD_MASK, 1, UINT32_C(0x800001)};
    for (size_t i = 0; i < sizeof(out_of_order_or_duplicate) /
                           sizeof(out_of_order_or_duplicate[0]); i++) {
        assert(mrc_cc_on_sack(&cc, out_of_order_or_duplicate[i], false, false, 0,
                              NULL, &feedback));
        assert(feedback.newly_received_bytes == 0);
        assert(cc.prev_rcvd_bytes == 1);
        assert(cc.inflight == 48);
    }
    assert(!mrc_cc_on_sack(&cc, MRC_CC_RECVD_MASK + 1u, false, false, 0,
                           NULL, &feedback));

    cc.inflight = INT32_MIN;
    assert(!mrc_cc_on_sack(&cc, 2, false, false, 0, NULL, &feedback));
    assert(cc.inflight == INT32_MIN);
    assert(cc.prev_rcvd_bytes == 1);
}

static void test_rtt_and_ecn(void)
{
    struct mrc_cc cc;
    struct mrc_cc_feedback feedback;
    struct mrc_cc_tx_sample sample = {.sent_at_ns = 10};
    assert(mrc_cc_init(&cc, 1040, 2080, 0, 100));
    assert(mrc_cc_on_sack(&cc, 0, false, false, 110, &sample, &feedback));
    assert(feedback.rtt_valid);
    assert(feedback.rtt_ns == 100);
    assert(cc.base_rtt_ns == 100);
    assert(feedback.queue_delay_ns == 0);
    assert(feedback.adjustment == MRC_CC_PROPORTIONAL_INCREASE);

    assert(mrc_cc_on_sack(&cc, 0, true, false, 209, &sample, &feedback));
    assert(feedback.queue_delay_ns == 99);
    assert(feedback.adjustment == MRC_CC_NO_ADJUSTMENT);
    assert(mrc_cc_on_sack(&cc, 0, true, false, 210, &sample, &feedback));
    assert(feedback.queue_delay_ns == 100);
    assert(feedback.adjustment == MRC_CC_MULTIPLICATIVE_DECREASE);
    assert(mrc_cc_on_sack(&cc, 0, false, false, 210, &sample, &feedback));
    assert(feedback.adjustment == MRC_CC_FAIR_INCREASE);

    assert(mrc_cc_on_sack(&cc, 0, false, true, 91, &sample, &feedback));
    assert(!feedback.rtt_valid);
    sample.retransmissions = 1;
    assert(mrc_cc_on_sack(&cc, 1, false, false, 91, &sample, &feedback));
    assert(!feedback.rtt_valid);
    assert(feedback.newly_received_bytes == 256);
    assert(cc.base_rtt_ns == 100);
    assert(mrc_cc_on_sack(&cc, 1, false, true, 91, &sample, &feedback));
    assert(feedback.rtt_valid);
    assert(cc.base_rtt_ns == 81);
    sample.retransmissions = 2;
    assert(mrc_cc_on_sack(&cc, 1, false, true, 91, &sample, &feedback));
    assert(!feedback.rtt_valid);

    sample.retransmissions = 0;
    assert(mrc_cc_on_sack(&cc, 1, false, false, sample.sent_at_ns, &sample, &feedback));
    assert(!feedback.rtt_valid);
    assert(mrc_cc_on_sack(&cc, 1, false, false, 91, NULL, &feedback));
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

int main(void)
{
    test_nominal_size_and_receive_counter();
    test_window_and_scheduler();
    test_ack_clock_rounding_and_wrap();
    test_rtt_and_ecn();
    test_loss_and_trim();
    puts("MRC congestion accounting tests passed");
    return 0;
}
