#include "mrc_qp.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

static void test_serial_arithmetic(void)
{
    assert(mrc_psn_delta(0, MRC_PSN_MASK) == 1);
    assert(mrc_psn_delta(MRC_PSN_MASK - 1, 1) == -3);
    assert(mrc_psn_delta(MRC_PSN_HALF, 0) == -(int32_t)MRC_PSN_HALF);
    assert(mrc_psn_add(MRC_PSN_MASK, 2) == 1);
    assert(mrc_psn_add(1, -2) == MRC_PSN_MASK);
}

static void test_receive_window_and_ar(void)
{
    uint8_t slots[384];
    struct mrc_rx_qp rx;
    struct mrc_sack sack;
    bool sack_due;
    uint32_t initial_psn = MRC_PSN_MASK - 1;

    assert(!mrc_rx_init(&rx, initial_psn, 129, slots));
    assert(mrc_rx_init(&rx, initial_psn, sizeof(slots), slots));
    assert(rx.cack_psn == MRC_PSN_MASK - 2);
    assert(rx.max_rcv_psn == rx.cack_psn);
    assert(mrc_rx_classify(&rx, rx.cack_psn) == MRC_RX_INVALID_PSN);
    assert(mrc_rx_classify(&rx, mrc_psn_add(initial_psn, 383)) == MRC_RX_NEW);
    assert(mrc_rx_classify(&rx, mrc_psn_add(initial_psn, 384)) == MRC_RX_AHEAD_OF_WINDOW);
    assert(mrc_rx_classify(&rx, mrc_psn_add(initial_psn, MRC_PSN_HALF))
           == MRC_RX_INVALID_PSN);

    /* The AR arrives across wrap, before the first two packets. */
    assert(mrc_rx_record(&rx, 0, 64, MRC_RX_AR, UINT32_MAX, &sack_due) == MRC_RX_NEW);
    assert(sack_due);
    assert(rx.cack_psn == MRC_PSN_MASK - 2 && rx.ooo_count == 1);
    assert(mrc_rx_build_sack(&rx, 0, &sack));
    assert(sack.cack_psn == MRC_PSN_MASK - 2 && sack.ack_psn_offset == 3);
    assert(sack.sack_offset == 0 && sack.bitmap == (UINT64_C(1) << 3));
    assert(sack.ooo_count == 1);
    mrc_rx_sack_sent(&rx, &sack);
    assert(rx.sack_trigger_count == 0);

    assert(mrc_rx_record(&rx, 0, 64, MRC_RX_RTX, UINT32_MAX, &sack_due) == MRC_RX_DUPLICATE);
    assert(sack_due && rx.ooo_count == 1);
    assert(mrc_rx_record(&rx, 2, 64, 0, UINT32_MAX, &sack_due) == MRC_RX_NEW);
    assert(!sack_due && rx.ooo_count == 2);
    assert(mrc_rx_record(&rx, MRC_PSN_MASK, 64, 0, UINT32_MAX, &sack_due) == MRC_RX_NEW);
    assert(!sack_due && rx.ooo_count == 3);
    assert(mrc_rx_record(&rx, initial_psn, 64, 0, UINT32_MAX, &sack_due) == MRC_RX_NEW);
    assert(sack_due && rx.ooo_count == 1);
    assert(rx.cack_psn == 0 && rx.max_rcv_psn == 2);
    assert(mrc_rx_build_sack(&rx, initial_psn, &sack));
    assert(sack.ack_psn_offset == -2 && sack.bitmap == UINT64_C(0x5));
    assert(mrc_rx_classify(&rx, initial_psn) == MRC_RX_DUPLICATE);
    assert(mrc_rx_classify(&rx, mrc_psn_add(initial_psn, -1)) == MRC_RX_INVALID_PSN);
}

static void test_sack_rotation_and_probe(void)
{
    uint8_t slots[384];
    struct mrc_rx_qp rx;
    struct mrc_sack sack;
    bool sack_due;
    assert(mrc_rx_init(&rx, 1000, sizeof(slots), slots));

    assert(mrc_rx_record(&rx, 1200, 1024, 0, 1024, &sack_due) == MRC_RX_NEW);
    assert(!sack_due && rx.sack_trigger_count == 1024);
    assert(mrc_rx_record(&rx, 1180, 2200, 0, 1024, &sack_due) == MRC_RX_NEW);
    assert(sack_due && rx.sack_trigger_count == 3224);
    assert(mrc_rx_build_sack(&rx, 1180, &sack));
    assert(sack.sack_offset == 0 && sack.ack_psn_offset == 181 && sack.bitmap == 0);
    mrc_rx_sack_sent(&rx, &sack);

    assert(mrc_rx_record(&rx, 1040, 64, 0, UINT32_MAX, &sack_due) == MRC_RX_NEW);
    assert(!sack_due);
    mrc_rx_build_probe_sack(&rx, 0xfedc, &sack);
    assert((uint16_t)sack.ack_psn_offset == 0xfedc);
    assert(sack.sack_offset == 41 && sack.bitmap == 1 && sack.ooo_count == 3);
    mrc_rx_sack_sent(&rx, &sack);

    mrc_rx_build_probe_sack(&rx, 1, &sack);
    assert(sack.sack_offset == 105 && sack.bitmap == 0);
    mrc_rx_sack_sent(&rx, &sack);
    mrc_rx_build_probe_sack(&rx, 2, &sack);
    assert(sack.sack_offset == 137);
    assert(sack.bitmap == (UINT64_C(1) << 44));
}

static void test_delivery_and_transport_ack(void)
{
    struct mrc_tx_packet slots[128];
    struct mrc_tx_qp tx;
    assert(mrc_tx_init(&tx, MRC_PSN_MASK, 128, slots));
    assert(!mrc_tx_track(&tx, 0, 1));
    assert(mrc_tx_track(&tx, MRC_PSN_MASK, 1));
    assert(mrc_tx_track(&tx, 0, 2));
    assert(mrc_tx_track(&tx, 1, 3));
    assert(mrc_tx_track(&tx, 2, 4));

    struct mrc_sack sack = {
        .cack_psn = MRC_PSN_MASK - 1,
        .ack_psn_offset = 4,
        .sack_offset = 2,
        .bitmap = 1,
    };
    assert(mrc_tx_apply_sack(&tx, &sack, true));
    assert(mrc_tx_packet(&tx, 0)->received);
    assert(!mrc_tx_packet(&tx, 2)->received);
    assert(!mrc_tx_packet(&tx, MRC_PSN_MASK)->received);
    assert(mrc_tx_apply_sack(&tx, &sack, false));
    assert(mrc_tx_packet(&tx, 2)->received);
    assert(!mrc_tx_packet(&tx, 1)->received);
    assert(tx.first_psn == MRC_PSN_MASK);

    sack.cack_psn = 0;
    sack.ack_psn_offset = 0;
    sack.sack_offset = 0;
    sack.bitmap = 1;
    assert(mrc_tx_apply_sack(&tx, &sack, false));
    assert(mrc_tx_packet(&tx, MRC_PSN_MASK)->received);
    assert(!mrc_tx_packet(&tx, 1)->received);
    assert(tx.first_psn == MRC_PSN_MASK && tx.delivery_cack_psn == 0);
    assert(mrc_tx_note_retransmit(&tx, 1, 10));
    assert(mrc_tx_packet(&tx, 1)->sent_at == 10);
    assert(mrc_tx_packet(&tx, 1)->retransmissions == 1);

    assert(mrc_tx_apply_transport_ack(&tx, 1));
    assert(tx.first_psn == 2 && tx.delivery_cack_psn == 1);
    assert(mrc_tx_packet(&tx, MRC_PSN_MASK) == NULL);
    assert(mrc_tx_packet(&tx, 0) == NULL);
    assert(mrc_tx_packet(&tx, 1) == NULL);
    assert(mrc_tx_packet(&tx, 2)->received);
    assert(!mrc_tx_apply_transport_ack(&tx, 0));
    assert(!mrc_tx_apply_transport_ack(&tx, 3));
    sack.cack_psn = 3;
    assert(!mrc_tx_apply_sack(&tx, &sack, false));
    assert(mrc_tx_apply_transport_ack(&tx, 2));
    assert(tx.first_psn == tx.next_psn);
}

static void test_sender_window(void)
{
    struct mrc_tx_packet slots[384];
    struct mrc_tx_qp tx;
    uint32_t start = MRC_PSN_MASK - 1;
    assert(mrc_tx_init(&tx, start, 384, slots));
    for (int offset = 0; offset < 384; offset++)
        assert(mrc_tx_track(&tx, mrc_psn_add(start, offset), (uint64_t)offset + 1));
    assert(!mrc_tx_track(&tx, mrc_psn_add(start, 384), 385));
    for (int offset = 0; offset < 384; offset++)
        assert(mrc_tx_packet(&tx, mrc_psn_add(start, offset))->sent_at == (uint64_t)offset + 1);
    assert(mrc_tx_apply_transport_ack(&tx, 0));
    assert(mrc_tx_track(&tx, mrc_psn_add(start, 384), 385));
    assert(mrc_tx_packet(&tx, mrc_psn_add(start, 384))->sent_at == 385);
    assert(!mrc_tx_packet(&tx, start));
}

static void test_local_ack_timeout(void)
{
    uint64_t timeout;
    assert(mrc_local_ack_timeout_ns(4, 1, 2, 0, &timeout) && timeout == 16384);
    assert(mrc_local_ack_timeout_ns(4, 1, 2, 1, &timeout) && timeout == 16384);
    assert(mrc_local_ack_timeout_ns(4, 1, 2, 2, &timeout) && timeout == 32768);
    assert(mrc_local_ack_timeout_ns(4, 1, 2, 3, &timeout) && timeout == 65536);
    assert(!mrc_local_ack_timeout_ns(4, 1, 2, 4, &timeout));
    assert(!mrc_local_ack_timeout_ns(4, 8, 2, 0, &timeout));
    assert(mrc_local_ack_timeout_ns(4, 0, 25, UINT32_MAX, &timeout));
    assert(timeout == (UINT64_C(1024) << 24));
}

int main(void)
{
    test_serial_arithmetic();
    test_receive_window_and_ar();
    test_sack_rotation_and_probe();
    test_delivery_and_transport_ack();
    test_sender_window();
    test_local_ack_timeout();
    puts("MRC QP state tests passed");
}
