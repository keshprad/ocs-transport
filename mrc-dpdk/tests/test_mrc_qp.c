#include "mrc_qp.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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

/* Exercise actual history growth, saturation and a complete PSN reuse cycle;
 * setting the private counter directly would miss off-by-one update bugs. */
static void test_receive_history_half_space(void)
{
    uint8_t slots[128], saved_slots[128];
    struct mrc_rx_qp rx, saved;
    const uint32_t start = MRC_PSN_MASK - 2;
    assert(mrc_rx_init(&rx, start, sizeof(slots), slots));
    for (uint32_t received = 1; received <= MRC_PSN_MASK + 129; received++) {
        const uint32_t current = (start + received - 1) & MRC_PSN_MASK;
        bool due;
        assert(mrc_rx_record(&rx, current, 0, 0, UINT32_MAX, &due) == MRC_RX_NEW);
        assert(rx.cack_psn == current && rx.ooo_count == 0);
        if ((received & 4095) == 0) {
            struct mrc_sack sack;
            mrc_rx_build_probe_sack(&rx, 1, &sack);
            mrc_rx_sack_sent(&rx, &sack);
        }
        if (received != 1 && received != 127 && received != 128 &&
            received != MRC_PSN_HALF - 1 && received != MRC_PSN_HALF &&
            received != MRC_PSN_HALF + 1 && received != MRC_PSN_MASK + 1 &&
            received != MRC_PSN_MASK + 129)
            continue;
        const uint32_t history = received < MRC_PSN_HALF ? received : MRC_PSN_HALF - 1;
        const uint32_t epsn = (current + 1) & MRC_PSN_MASK;
        assert(rx.contiguous_history == history);
        assert(mrc_rx_classify(&rx, current) == MRC_RX_DUPLICATE);
        assert(mrc_rx_classify(&rx, (epsn - history) & MRC_PSN_MASK) == MRC_RX_DUPLICATE);
        assert(mrc_rx_classify(&rx, epsn) == MRC_RX_NEW);
        assert(mrc_rx_classify(&rx, (epsn + 127) & MRC_PSN_MASK) == MRC_RX_NEW);
        const uint32_t invalid[] = {
            (epsn - history - 1) & MRC_PSN_MASK,
            (epsn - MRC_PSN_HALF) & MRC_PSN_MASK,
        };
        memcpy(&saved, &rx, sizeof(rx));
        memcpy(saved_slots, slots, sizeof(slots));
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            assert(mrc_rx_record(&rx, invalid[i], 1, MRC_RX_AR | MRC_RX_RTX,
                                  0, &due) == MRC_RX_INVALID_PSN);
            assert(!due && memcmp(&rx, &saved, sizeof(rx)) == 0);
            assert(memcmp(slots, saved_slots, sizeof(slots)) == 0);
        }
        assert(mrc_rx_record(&rx, (epsn + 128) & MRC_PSN_MASK, 1, MRC_RX_AR,
                              0, &due) == MRC_RX_AHEAD_OF_WINDOW);
        assert(!due && memcmp(&rx, &saved, sizeof(rx)) == 0);
        assert(memcmp(slots, saved_slots, sizeof(slots)) == 0);
        /* The endpoint silently drops both invalid and forward-window PSNs. */
    }
}

/* Both window edges remain side-effect free with feedback-requesting flags,
 * sparse bitmap state, moving ePSN, and wrap at minimum/maximum MPR. */
static void test_outside_window_preserves_state(void)
{
    const uint32_t starts[] = {0, MRC_PSN_HALF - 1, MRC_PSN_MASK - 1};
    const uint32_t windows[] = {128, 384, MRC_MPR_MAX};
    uint8_t slots[MRC_MPR_MAX], saved_slots[MRC_MPR_MAX];
    for (size_t start = 0; start < sizeof(starts) / sizeof(starts[0]); start++) {
        for (size_t size = 0; size < sizeof(windows) / sizeof(windows[0]); size++) {
            struct mrc_rx_qp rx;
            assert(mrc_rx_init(&rx, starts[start], windows[size], slots));
            for (unsigned int step = 0; step < 3; step++) {
                uint32_t epsn = mrc_psn_add(rx.cack_psn, 1);
                bool due;
                /* Last valid PSN is accepted, retaining a gap and AR state. */
                uint32_t last = mrc_psn_add(epsn, windows[size] - 1);
                assert(mrc_rx_record(&rx, last, 256, MRC_RX_AR, UINT32_MAX, &due) == MRC_RX_NEW);
                assert(due && rx.ooo_count > 0);
                struct mrc_rx_qp saved;
                memcpy(&saved, &rx, sizeof(saved));
                memcpy(saved_slots, slots, windows[size]);
                const int32_t offsets[] = {(int32_t)windows[size], (int32_t)windows[size] + 1,
                    MRC_PSN_HALF - 1, -(int32_t)MRC_PSN_HALF};
                for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
                    uint32_t psn = mrc_psn_add(epsn, offsets[i]);
                    enum mrc_rx_disposition expected = offsets[i] < 0 ? MRC_RX_INVALID_PSN : MRC_RX_AHEAD_OF_WINDOW;
                    due = true;
                    assert(mrc_rx_record(&rx, psn, UINT32_MAX, MRC_RX_AR | MRC_RX_RTX | MRC_RX_ECN,
                                         0, &due) == expected);
                    assert(!due && memcmp(&rx, &saved, sizeof(rx)) == 0);
                    assert(memcmp(slots, saved_slots, windows[size]) == 0);
                }
                /* Legitimate traffic still advances ePSN after the drops. */
                assert(mrc_rx_record(&rx, epsn, 256, 0, UINT32_MAX, &due) == MRC_RX_NEW);
                assert(rx.cack_psn == epsn);
            }
        }
    }
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
    /* A future trigger and a future set bit must both leave the QP untouched. */
    struct mrc_sack invalid = { .cack_psn = 0, .ack_psn_offset = 3 };
    assert(!mrc_tx_apply_sack(&tx, &invalid, false));
    invalid.ack_psn_offset = 0;
    invalid.sack_offset = 1;
    invalid.bitmap = UINT64_C(1) << 2;
    assert(!mrc_tx_apply_sack(&tx, &invalid, false));
    assert(tx.delivery_cack_psn == MRC_PSN_MASK - 1);
    assert(!mrc_tx_packet(&tx, MRC_PSN_MASK)->received);
    assert(!mrc_tx_packet(&tx, 0)->received);

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

static void test_unsent_sack_trigger(void)
{
    const uint32_t starts[] = {100, MRC_PSN_MASK - 1};
    for (unsigned int i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        struct mrc_tx_packet slots[128], before_slots[128];
        struct mrc_tx_qp tx;
        assert(mrc_tx_init(&tx, starts[i], 128, slots));
        for (int offset = 0; offset < 4; offset++)
            assert(mrc_tx_track(&tx, mrc_psn_add(starts[i], offset), offset + 1));
        struct mrc_sack sack = {
            .cack_psn = mrc_psn_add(starts[i], 1),
            .sack_offset = 1, .bitmap = 3, .ooo_count = 2,
        };
        struct mrc_tx_qp before;
        memcpy(&before, &tx, sizeof(tx));
        memcpy(before_slots, slots, sizeof(slots));
        const int offsets[] = {3, -2}; /* next unsent PSN and before the first send */
        for (unsigned int j = 0; j < sizeof(offsets) / sizeof(offsets[0]); j++) {
            sack.ack_psn_offset = offsets[j];
            assert(!mrc_tx_apply_sack(&tx, &sack, false));
            assert(memcmp(&tx, &before, sizeof(tx)) == 0);
            assert(memcmp(slots, before_slots, sizeof(slots)) == 0);
        }
        /* A negative offset can identify a real send; do not reject it. */
        sack.ack_psn_offset = -1;
        assert(mrc_tx_apply_sack(&tx, &sack, false));
        assert(tx.delivery_cack_psn == sack.cack_psn);
        for (int offset = 0; offset < 4; offset++)
            assert(mrc_tx_packet(&tx, mrc_psn_add(starts[i], offset))->received);
        /* Probe offsets are opaque identifiers, not data PSNs. */
        sack.ack_psn_offset = -32768;
        assert(mrc_tx_apply_sack(&tx, &sack, true));
    }
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

static void test_maximum_window(void)
{
    const uint32_t start = MRC_PSN_MASK - 1;
    const uint32_t last = mrc_psn_add(start, MRC_MPR_MAX - 1);
    const uint32_t beyond = mrc_psn_add(start, MRC_MPR_MAX);
    uint8_t rx_slots[MRC_MPR_MAX];
    static struct mrc_tx_packet tx_slots[MRC_MPR_MAX];
    struct mrc_rx_qp rx;
    struct mrc_tx_qp tx;
    bool sack_due;
    struct mrc_sack sack;

    assert(mrc_rx_init(&rx, start, MRC_MPR_MAX, rx_slots));
    assert(mrc_tx_init(&tx, start, MRC_MPR_MAX, tx_slots));
    for (uint32_t offset = 0; offset < MRC_MPR_MAX; offset++)
        assert(mrc_tx_track(&tx, mrc_psn_add(start, (int32_t)offset), (uint64_t)offset + 1));
    assert(!mrc_tx_track(&tx, beyond, 1));
    assert(mrc_rx_classify(&rx, beyond) == MRC_RX_AHEAD_OF_WINDOW);
    assert(mrc_rx_record(&rx, mrc_psn_add(last, -1), 0, 0, UINT32_MAX, &sack_due) == MRC_RX_NEW);
    assert(mrc_rx_record(&rx, last, 0, MRC_RX_AR, UINT32_MAX, &sack_due) == MRC_RX_NEW && sack_due);
    assert(mrc_rx_build_sack(&rx, last, &sack) && sack.ack_psn_offset == MRC_MPR_MAX &&
           sack.ooo_count == 2 && sack.bitmap == 0);
    assert(mrc_tx_apply_sack(&tx, &sack, false));
    for (unsigned int offset = MRC_SACK_BITS; offset < MRC_MPR_MAX; offset += MRC_SACK_BITS) {
        mrc_rx_sack_sent(&rx, &sack);
        mrc_rx_build_probe_sack(&rx, 0x8000, &sack);
    }
    assert(sack.sack_offset == MRC_MPR_MAX - MRC_SACK_BITS && sack.bitmap == (UINT64_C(1) << 63));
    assert(mrc_tx_apply_sack(&tx, &sack, true));
    assert(mrc_tx_packet(&tx, last)->received && mrc_tx_packet(&tx, mrc_psn_add(last, -1))->received);
    assert(!mrc_tx_packet(&tx, start)->received);
    assert(mrc_tx_apply_transport_ack(&tx, last));
    assert(mrc_tx_track(&tx, beyond, 2) && !mrc_tx_packet(&tx, beyond)->received);
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

static void test_local_ack_timeout_matrix(void)
{
    const uint64_t cap = UINT64_C(1024) * (UINT64_C(1) << 24);
    for (unsigned int exponent = 0; exponent <= 31; exponent++) {
        const uint64_t base = UINT64_C(1024) * (UINT64_C(1) << exponent);
        for (unsigned int linear = 0; linear <= 7; linear++) {
            for (unsigned int exponential = 0; exponential <= 25; exponential++) {
                unsigned int end = linear + (exponential == 25 ? 40 : exponential + 1);
                uint64_t expected = base;
                for (unsigned int retry = 0; retry <= end; retry++) {
                    uint64_t timeout = UINT64_MAX;
                    bool valid = mrc_local_ack_timeout_ns(exponent, linear, exponential,
                                                           retry, &timeout);
                    if (exponential != 25 && retry > linear + exponential) {
                        assert(!valid && timeout == UINT64_MAX);
                        continue;
                    }
                    /* Table 7-1: each exponential retry doubles the prior
                     * interval, capped at 2^24 * 1.024 us; linear stays base. */
                    if (retry > linear)
                        expected = expected >= cap / 2 ? cap : expected * 2;
                    assert(valid && timeout == expected);
                }
                uint64_t timeout = UINT64_MAX;
                bool valid = mrc_local_ack_timeout_ns(exponent, linear, exponential,
                                                       UINT32_MAX, &timeout);
                assert(valid == (exponential == 25));
                assert(timeout == (valid ? cap : UINT64_MAX));
            }
        }
    }
    const unsigned int invalid[][3] = {
        {32, 0, 0}, {UINT32_MAX, 0, 0}, {0, 8, 0},
        {0, UINT32_MAX, 0}, {0, 0, 26}, {0, 0, UINT32_MAX},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        uint64_t timeout = UINT64_MAX;
        assert(!mrc_local_ack_timeout_ns(invalid[i][0], invalid[i][1], invalid[i][2], 0, &timeout));
        assert(timeout == UINT64_MAX);
    }
    assert(!mrc_local_ack_timeout_ns(0, 0, 0, 0, NULL));
}

int main(void)
{
    test_serial_arithmetic();
    test_receive_window_and_ar();
    test_receive_history_half_space();
    test_outside_window_preserves_state();
    test_sack_rotation_and_probe();
    test_delivery_and_transport_ack();
    test_unsent_sack_trigger();
    test_sender_window();
    test_maximum_window();
    test_local_ack_timeout();
    test_local_ack_timeout_matrix();
    puts("MRC QP state tests passed");
}
