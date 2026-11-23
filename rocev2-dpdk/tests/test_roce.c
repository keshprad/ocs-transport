#include "roce_rc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const struct roce_qp send_qp = {
    .local.ipv4 = 0xc0000201, .peer.ipv4 = 0xc0000202,
    .local_qpn = 37, .peer_qpn = 91, .pmtu = 256,
};

static struct roce_responder responder(uint8_t *destination, struct roce_region region,
                                       uint32_t psn)
{
    return (struct roce_responder) {
        .qp = {
            .local = send_qp.peer, .peer = send_qp.local,
            .local_qpn = send_qp.peer_qpn, .peer_qpn = send_qp.local_qpn, .pmtu = send_qp.pmtu,
        },
        .region = region, .buffer = destination, .expected = psn,
    };
}

static struct roce_packet transmit(struct roce_requester *tx, uint64_t now)
{
    struct roce_packet packet;
    assert(roce_requester_next(tx, 0, &packet));
    roce_requester_sent(tx, 1, now);
    return packet;
}

static void test_sequence_nak_and_lost_final_ack(void)
{
    uint8_t source[769], destination[769] = {0};
    for (size_t i = 0; i < sizeof(source); ++i)
        source[i] = (uint8_t)(i / 3 + 1);
    const uint32_t psn = ROCE_PSN_MASK - 1;
    struct roce_region region = { .address = UINT64_C(0x100100000), .rkey = 0x21, .size = sizeof(source) };
    struct roce_requester tx;
    assert(roce_requester_init(&tx, &send_qp, psn, region, source, 3, ROCE_SEND_WINDOW_DEFAULT, 1));
    struct roce_responder rx = responder(destination, region, psn);
    struct roce_packet first = transmit(&tx, 0), lost = transmit(&tx, 0);
    struct roce_packet third = transmit(&tx, 0), last = transmit(&tx, 0), ack;
    assert(first.opcode == ROCE_WRITE_FIRST && first.psn == psn && !first.ack_requested);
    assert(lost.opcode == ROCE_WRITE_MIDDLE && lost.psn == ROCE_PSN_MASK);
    assert(third.opcode == ROCE_WRITE_MIDDLE && third.psn == 0);
    assert(first.udp_source == lost.udp_source && first.udp_source == third.udp_source);
    assert(first.udp_source == last.udp_source);
    assert(last.opcode == ROCE_WRITE_LAST && last.psn == 1 && last.ack_requested);
    assert(!roce_responder_receive(&rx, &first, &ack) && rx.msn == 0);
    assert(roce_responder_receive(&rx, &third, &ack) && ack.syndrome == ROCE_NAK_SEQUENCE);
    assert(ack.psn == lost.psn && !roce_responder_receive(&rx, &last, &first));
    roce_requester_feedback(&tx, &ack, 1);
    assert(tx.acked == 1 && tx.next == 1 && tx.retries == 1);
    roce_requester_feedback(&tx, &ack, 1);
    assert(tx.retries == 1);
    for (uint32_t i = tx.next; i < tx.total; ++i) {
        struct roce_packet packet = transmit(&tx, 1);
        assert(roce_responder_receive(&rx, &packet, &ack) == (i == tx.total - 1));
    }
    assert(rx.msn == 1 && ack.psn == 1 && ack.msn == 1 && ack.syndrome == ROCE_ACK_NO_CREDIT);
    assert(memcmp(source, destination, sizeof(source)) == 0);
    roce_requester_poll_timer(&tx, 8, 8);
    assert(tx.retries == 1);
    roce_requester_poll_timer(&tx, 9, 8);
    assert(tx.retries == 2);
    struct roce_packet retry = transmit(&tx, 9);
    uint8_t original = destination[256];
    destination[256] ^= 1; /* A duplicate must never write to memory again. */
    assert(roce_responder_receive(&rx, &retry, &ack));
    assert(destination[256] == (uint8_t)(original ^ 1) && rx.msn == 1);
    roce_requester_feedback(&tx, &ack, 9);
    assert(tx.acked == tx.total && !tx.failed);
}

static void test_window_timeout_and_address_validation(void)
{
    uint8_t source[33 * 256] = {1}, dest[sizeof(source)] = {0};
    struct roce_region region = { .address = UINT64_C(0xffffffff00000000), .rkey = 4, .size = sizeof(source) };
    struct roce_requester tx;
    assert(roce_requester_init(&tx, &send_qp, 6, region, source, 1, ROCE_SEND_WINDOW_DEFAULT, 1));
    struct roce_responder rx = responder(dest, region, 6);
    struct roce_packet ack, packet;
    for (unsigned int i = 0; i < 32; ++i) {
        packet = transmit(&tx, 100);
        assert(packet.ack_requested == (i == 31));
        assert(roce_responder_receive(&rx, &packet, &ack) == (i == 31));
    }
    assert(!roce_requester_next(&tx, 0, &packet));
    struct roce_packet invalid = ack;
    invalid.qpn ^= 1;
    roce_requester_feedback(&tx, &invalid, 101);
    invalid = ack;
    invalid.pkey ^= 1;
    roce_requester_feedback(&tx, &invalid, 101);
    invalid = ack;
    invalid.src.ipv4 ^= 1;
    roce_requester_feedback(&tx, &invalid, 101);
    invalid = ack;
    ++invalid.psn;
    roce_requester_feedback(&tx, &invalid, 101);
    assert(tx.acked == 0 && tx.next == 32);
    roce_requester_feedback(&tx, &ack, 101);
    assert(tx.acked == 32 && transmit(&tx, 101).opcode == ROCE_WRITE_LAST);
    roce_requester_feedback(&tx, &ack, 107);
    assert(tx.acked == 32 && tx.last_progress == 101);
    roce_requester_poll_timer(&tx, 111, 10);
    assert(tx.retries == 1 && !tx.failed && tx.next == 32);
    transmit(&tx, 111);
    roce_requester_poll_timer(&tx, 121, 10);
    assert(tx.failed);
}

static void test_single_packet_and_access_errors(void)
{
    const uint8_t source[] = {1, 2, 3};
    uint8_t destination[32] = {0};
    struct roce_region region = { .address = 1000, .rkey = 77, .size = sizeof(destination) };
    struct roce_region remote = { .address = 1007, .rkey = 77, .size = sizeof(source) };
    struct roce_requester tx;
    assert(roce_requester_init(&tx, &send_qp, 2, remote, source, 3, ROCE_SEND_WINDOW_DEFAULT, 1));
    struct roce_responder rx = responder(destination, region, 2);
    struct roce_packet packet = transmit(&tx, 0), ack;
    assert(packet.opcode == ROCE_WRITE_ONLY && packet.payload_length == sizeof(source));
    assert(roce_responder_receive(&rx, &packet, &ack));
    assert(rx.msn == 1 && ack.msn == 1 && ack.psn == 2 && ack.syndrome == ROCE_ACK_NO_CREDIT);
    assert(memcmp(destination + 7, source, sizeof(source)) == 0);

    uint8_t snapshot[sizeof(destination)];
    memcpy(snapshot, destination, sizeof(snapshot));
    for (unsigned int i = 0; i < 4; ++i) {
        rx = responder(destination, region, 2);
        struct roce_packet invalid = packet;
        if (i == 0)
            invalid.rkey ^= 1;
        else if (i == 1)
            invalid.remote_address = region.address - 1;
        else if (i == 2)
            invalid.remote_address = region.address + region.size - 2;
        else
            invalid.write_length += 1;
        assert(roce_responder_receive(&rx, &invalid, &ack));
        assert(ack.syndrome == (i == 3 ? ROCE_NAK_INVALID : ROCE_NAK_ACCESS));
        assert(rx.failed && rx.expected == 2 && rx.msn == 0);
        assert(memcmp(snapshot, destination, sizeof(snapshot)) == 0);
    }
    roce_requester_feedback(&tx, &ack, 0);
    assert(tx.failed && tx.acked == 0);
}

static void test_short_tx_burst(void)
{
    uint8_t source[80 * 256] = {0};
    struct roce_region region = { .address = 1000, .rkey = 77, .size = sizeof(source) };
    struct roce_requester tx;
    assert(roce_requester_init(&tx, &send_qp, 5, region, source, 3, 64, 1));
    struct roce_packet packet;
    assert(roce_requester_next(&tx, 31, &packet) && packet.ack_requested);
    assert(roce_requester_next(&tx, 63, &packet) && packet.ack_requested);
    assert(!roce_requester_next(&tx, 64, &packet));
    assert(tx.next == 0 && tx.sent == 0);
    roce_requester_sent(&tx, 16, 1);
    assert(tx.next == 16 && tx.sent == 16);
    assert(roce_requester_next(&tx, 0, &packet));
    assert(packet.psn == 21 && packet.payload == source + 16 * 256);
    roce_requester_sent(&tx, 0, 2);
    assert(tx.next == 16 && tx.sent == 16 && tx.last_progress == 1);
}

static void test_pipelined_writes(void)
{
    enum { WRITE_SIZE = 259, WRITES = 3 };
    uint8_t source[WRITE_SIZE * WRITES];
    uint8_t destination[sizeof(source)] = {0};
    for (size_t i = 0; i < sizeof(source); ++i)
        source[i] = (uint8_t)((i * 17) ^ (i >> 8));

    struct roce_region region = { .address = 1000, .rkey = 77, .size = WRITE_SIZE };
    uint32_t start = ROCE_PSN_MASK - 1;
    struct roce_requester tx;
    assert(roce_requester_init(&tx, &send_qp, start, region, source, 3, 64, WRITES));
    region.size = sizeof(destination);
    struct roce_responder rx = responder(destination, region, start);
    struct roce_packet packets[2 * WRITES], feedback;
    for (unsigned int i = 0; i < 2 * WRITES; ++i) {
        assert(roce_requester_next(&tx, i, &packets[i]));
        assert(packets[i].psn == ((start + i) & ROCE_PSN_MASK));
        assert(packets[i].opcode == (i % 2 ? ROCE_WRITE_LAST : ROCE_WRITE_FIRST));
        assert(packets[i].remote_address == region.address + (i / 2) * WRITE_SIZE);
    }
    roce_requester_sent(&tx, 2 * WRITES, 1);
    for (unsigned int i = 0; i < 2 * WRITES; ++i) {
        bool last = i % 2 != 0;
        assert(roce_responder_receive(&rx, &packets[i], &feedback) == last);
        if (last) {
            assert(feedback.msn == i / 2 + 1);
            roce_requester_feedback(&tx, &feedback, 2);
            assert(tx.acked == i + 1);
        }
    }
    assert(rx.msn == WRITES && rx.accepted == 2 * WRITES);
    assert(memcmp(source, destination, sizeof(source)) == 0);

    /* Verify cumulative progress after the logical index crosses an entire PSN cycle. */
    tx.acked = ROCE_PSN_MASK + 1u;
    tx.total = tx.next = tx.sent = tx.acked + 5;
    feedback.psn = (tx.initial_psn + tx.acked + 4) & ROCE_PSN_MASK;
    roce_requester_feedback(&tx, &feedback, 3);
    assert(tx.acked == tx.sent && tx.last_progress == 3);
}

int main(void)
{
    test_sequence_nak_and_lost_final_ack();
    test_window_timeout_and_address_validation();
    test_single_packet_and_access_errors();
    test_short_tx_burst();
    test_pipelined_writes();
    puts("RoCEv2 RC state tests passed");
    return 0;
}
