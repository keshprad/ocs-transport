#include "mrc_connect.h"
#include "mrc_mpr.h"
#include "mrc_qp.h"

#include <assert.h>
#include <stdio.h>

static void test_negotiated_and_static_modes(void)
{
    struct mrc_connect_offer local = {
        .receive = { .qpn = 3, .max_mpr = 8 },
        .pmtu = 1024, .p_key = 0xffff,
        .supported = MRC_CONNECT_DYNAMIC_MPR, .offered = MRC_CONNECT_DYNAMIC_MPR,
    };
    struct mrc_connect_offer peer = {
        .receive = { .qpn = 4, .max_mpr = 4 },
        .pmtu = 1024, .p_key = 0xffff,
        .supported = MRC_CONNECT_DYNAMIC_MPR,
    };
    struct mrc_connect_qp qp, peer_qp;
    struct mrc_mpr requestor, responder;
    assert(mrc_connect_negotiate(&local, &peer, &qp) == 0);
    assert(mrc_mpr_init(&requestor, qp.dynamic_mpr, qp.send.max_mpr));
    assert(mrc_connect_negotiate(&peer, &local, &peer_qp) == 0);
    assert(mrc_mpr_init(&responder, peer_qp.dynamic_mpr, peer_qp.receive.max_mpr));
    assert(mrc_mpr_advertised(&responder) == 0);
    assert(mrc_mpr_update(&requestor, 250, 251, 0) == MRC_MPR_IGNORED);
    assert(mrc_mpr_update(&requestor, 250, 251, 255) == MRC_MPR_IGNORED);
    assert(!mrc_mpr_set_advertised(&responder, 1));
    assert(mrc_mpr_packets(&requestor) == 512);
    assert(mrc_mpr_can_transmit(&requestor, 250, 762));
    assert(!mrc_mpr_can_transmit(&requestor, 250, 763));

    peer.offered = MRC_CONNECT_DYNAMIC_MPR;
    assert(mrc_connect_negotiate(&local, &peer, &qp) == 0);
    assert(mrc_mpr_init(&requestor, qp.dynamic_mpr, qp.send.max_mpr));
    assert(mrc_connect_negotiate(&peer, &local, &peer_qp) == 0);
    assert(mrc_mpr_init(&responder, peer_qp.dynamic_mpr, peer_qp.receive.max_mpr));
    assert(mrc_mpr_advertised(&responder) == 4);
    assert(mrc_mpr_set_advertised(&responder, 2));
    assert(mrc_mpr_advertised(&responder) == 2);
    assert(mrc_mpr_update(&requestor, 250, 251, mrc_mpr_advertised(&responder)) ==
           MRC_MPR_CHANGED);
    assert(mrc_mpr_packets(&requestor) == 256);
}

static void test_feedback_order_and_limits(void)
{
    struct mrc_mpr mpr;
    assert(!mrc_mpr_init(&mpr, true, 0));
    assert(mrc_mpr_init(&mpr, true, 8));

    assert(mrc_mpr_update(&mpr, 100, 101, 8) == MRC_MPR_UNCHANGED);
    assert(mrc_mpr_update(&mpr, 101, 102, 1) == MRC_MPR_CHANGED);
    assert(mrc_mpr_update(&mpr, 102, 102, 8) == MRC_MPR_IGNORED);
    assert(mrc_mpr_update(&mpr, 102, 101, 8) == MRC_MPR_IGNORED);
    assert(mrc_mpr_update(&mpr, 102, 101, 0) == MRC_MPR_ZERO);
    assert(mrc_mpr_update(&mpr, 102, 103, 0) == MRC_MPR_ZERO);
    assert(mrc_mpr_update(&mpr, 102, 103, 9) == MRC_MPR_EXCEEDS_MAX);
    assert(mrc_mpr_packets(&mpr) == 128);
    assert(mrc_mpr_update(&mpr, 103, 104, 8) == MRC_MPR_CHANGED);
    assert(mrc_mpr_packets(&mpr) == 1024);
    assert(!mrc_mpr_set_advertised(&mpr, 0));
    assert(!mrc_mpr_set_advertised(&mpr, 9));
    assert(mrc_mpr_advertised(&mpr) == 8);
}

static void test_shrink_with_outstanding_packets(void)
{
    struct mrc_tx_packet slots[512];
    struct mrc_tx_qp qp;
    struct mrc_mpr mpr;
    assert(mrc_tx_init(&qp, 1, 512, slots));
    assert(mrc_mpr_init(&mpr, true, 4));

    for (uint32_t psn = 1; psn <= 512; psn++) {
        assert(mrc_mpr_can_transmit(&mpr, qp.delivery_cack_psn, psn));
        assert(mrc_tx_track(&qp, psn, 1));
    }

    struct mrc_sack feedback = { .cack_psn = 100 };
    uint32_t previous = qp.delivery_cack_psn;
    assert(mrc_tx_apply_sack(&qp, &feedback, true));
    assert(mrc_mpr_update(&mpr, previous, feedback.cack_psn, 1) == MRC_MPR_CHANGED);
    assert(qp.next_psn == 513);
    assert(mrc_tx_packet(&qp, 512));
    assert(!mrc_mpr_can_transmit(&mpr, qp.delivery_cack_psn, qp.next_psn));
    assert(mrc_mpr_can_transmit(&mpr, qp.delivery_cack_psn, 228));
    assert(!mrc_mpr_can_transmit(&mpr, qp.delivery_cack_psn, 229));
    assert(!mrc_mpr_can_transmit(&mpr, qp.delivery_cack_psn, 512));
    assert(mrc_mpr_can_transmit(&mpr, qp.delivery_cack_psn, 1));

    feedback.cack_psn = 385;
    previous = qp.delivery_cack_psn;
    assert(mrc_tx_apply_sack(&qp, &feedback, true));
    assert(mrc_mpr_update(&mpr, previous, feedback.cack_psn, 1) == MRC_MPR_UNCHANGED);
    assert(mrc_mpr_can_transmit(&mpr, qp.delivery_cack_psn, 512));
    assert(mrc_mpr_can_transmit(&mpr, qp.delivery_cack_psn, qp.next_psn));

    feedback.cack_psn = 600;
    assert(!mrc_tx_apply_sack(&qp, &feedback, true));
    assert(qp.delivery_cack_psn == 385);
    assert(mrc_mpr_packets(&mpr) == 128);
}

static void test_psn_wrap(void)
{
    struct mrc_mpr mpr;
    assert(mrc_mpr_init(&mpr, true, 255));
    assert(mrc_mpr_packets(&mpr) == 32640);
    assert(mrc_mpr_update(&mpr, 0xfffffd, 0xffffff, 1) == MRC_MPR_CHANGED);
    assert(mrc_mpr_can_transmit(&mpr, 0xfffffe, 126));
    assert(!mrc_mpr_can_transmit(&mpr, 0xfffffe, 127));
    assert(mrc_mpr_update(&mpr, 0xffffff, 0, 2) == MRC_MPR_CHANGED);
    assert(mrc_mpr_update(&mpr, 0, 0xffffff, 1) == MRC_MPR_IGNORED);
    assert(mrc_mpr_update(&mpr, 0, MRC_PSN_HALF, 1) == MRC_MPR_IGNORED);
    assert(mrc_mpr_update(&mpr, 0, MRC_PSN_MASK + 1, 1) == MRC_MPR_IGNORED);
    assert(mrc_mpr_can_transmit(&mpr, 0, 256));
    assert(!mrc_mpr_can_transmit(&mpr, 0, 257));
    assert(!mrc_mpr_can_transmit(&mpr, 0, MRC_PSN_HALF));
    assert(!mrc_mpr_can_transmit(&mpr, 0, MRC_PSN_MASK + 1));
}

int main(void)
{
    test_negotiated_and_static_modes();
    test_feedback_order_and_limits();
    test_shrink_with_outstanding_packets();
    test_psn_wrap();
    puts("MRC maximum PSN range tests passed");
}
