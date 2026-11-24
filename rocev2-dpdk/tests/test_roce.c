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
    assert(roce_requester_stage(tx, 0, &packet) != UINT32_MAX);
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
    assert(roce_responder_can_establish(&rx, &first));
    assert(!roce_responder_can_establish(&rx, &lost));
    struct roce_packet invalid = first;
    ++invalid.rkey;
    assert(!roce_responder_can_establish(&rx, &invalid));
    invalid = first;
    ++invalid.psn;
    assert(!roce_responder_can_establish(&rx, &invalid));
    invalid = first;
    --invalid.payload_length;
    assert(!roce_responder_can_establish(&rx, &invalid));
    assert(!rx.accepted && destination[0] == 0);
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
    assert(tx.acked == tx.total && tx.failure == ROCE_REQUESTER_OK);
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
    assert(roce_requester_stage(&tx, 0, &packet) == UINT32_MAX);
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
    assert(tx.retries == 1 && tx.failure == ROCE_REQUESTER_OK && tx.next == 32);
    transmit(&tx, 111);
    roce_requester_poll_timer(&tx, 121, 10);
    assert(tx.failure == ROCE_REQUESTER_RETRY_EXCEEDED && tx.acked == 32);
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
    assert(tx.failure == ROCE_REQUESTER_REMOTE_INVALID && tx.acked == 0);
}

static void test_fatal_nak_progress(void)
{
    enum { WRITE_SIZE = 259, WRITES = 3 };
    const uint8_t source[WRITE_SIZE * WRITES] = {0};
    const struct roce_region remote = { .address = 1000, .rkey = 77, .size = WRITE_SIZE };
    const uint32_t start = ROCE_PSN_MASK - 1;
    const enum roce_requester_failure errors[] = {
        ROCE_REQUESTER_REMOTE_INVALID, ROCE_REQUESTER_REMOTE_ACCESS, ROCE_REQUESTER_REMOTE_OPERATION,
    };
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        struct roce_requester tx;
        assert(roce_requester_init(&tx, &send_qp, start, remote, source, 2, ROCE_SEND_WINDOW_DEFAULT, WRITES));
        while (tx.next < tx.total)
            transmit(&tx, 0);
        struct roce_packet feedback = {
            .src = send_qp.peer, .dst = send_qp.local, .qpn = send_qp.local_qpn,
            .opcode = ROCE_ACK, .pkey = ROCE_PKEY, .syndrome = (uint8_t)errors[i],
            .psn = (start + tx.total) & ROCE_PSN_MASK,
        };
        roce_requester_feedback(&tx, &feedback, 1);
        assert(tx.failure == ROCE_REQUESTER_OK && tx.acked == 0);

        /* The second WRITE failed after its first segment. */
        feedback.psn = (start + 3) & ROCE_PSN_MASK;
        roce_requester_feedback(&tx, &feedback, 2);
        assert(tx.failure == errors[i] && tx.acked == 3);
        assert(tx.acked / tx.packets_per_write == 1);
        struct roce_packet packet;
        assert(roce_requester_stage(&tx, 0, &packet) == UINT32_MAX);
        feedback.psn = (start + tx.total - 1) & ROCE_PSN_MASK;
        feedback.syndrome = ROCE_ACK_NO_CREDIT;
        roce_requester_feedback(&tx, &feedback, 3);
        assert(tx.failure == errors[i] && tx.acked == 3);
    }
}

static void test_short_tx_burst(void)
{
    uint8_t source[80 * 256] = {0};
    struct roce_region region = { .address = 1000, .rkey = 77, .size = sizeof(source) };
    struct roce_requester tx;
    assert(roce_requester_init(&tx, &send_qp, 5, region, source, 3, 64, 1));
    struct roce_packet packet;
    assert(roce_requester_stage(&tx, 31, &packet) == 0 && packet.ack_requested);
    assert(roce_requester_stage(&tx, 63, &packet) == 0 && packet.ack_requested);
    assert(roce_requester_stage(&tx, 64, &packet) == UINT32_MAX);
    assert(tx.next == 0 && tx.sent == 0);
    roce_requester_sent(&tx, 16, 1);
    assert(tx.next == 16 && tx.sent == 16);
    assert(roce_requester_stage(&tx, 0, &packet) == 0);
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
        assert(roce_requester_stage(&tx, i, &packets[i]) == i / 2);
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

static void test_variable_write_packets(void)
{
    const uint32_t lengths[] = {255, 515, 256};
    const enum roce_opcode opcodes[] = {
        ROCE_WRITE_ONLY, ROCE_WRITE_FIRST, ROCE_WRITE_MIDDLE, ROCE_WRITE_LAST, ROCE_WRITE_ONLY,
    };
    uint8_t source[1026], destination[sizeof(source)] = {0};
    for (size_t i = 0; i < sizeof(source); ++i)
        source[i] = (uint8_t)(i ^ (i >> 5));

    struct roce_region region = { .address = 1000, .rkey = 77, .size = 342 };
    const uint32_t start = ROCE_PSN_MASK - 1;
    struct roce_requester tx;
    assert(roce_requester_init(&tx, &send_qp, start, region, source, 3, 64, 3));
    tx.total = 5;
    region.size = sizeof(destination);
    struct roce_responder rx = responder(destination, region, start);
    size_t offset = 0;
    for (size_t wr = 0; wr < sizeof(lengths) / sizeof(lengths[0]); ++wr) {
        uint32_t count = 1 + (lengths[wr] - 1) / send_qp.pmtu;
        for (uint32_t segment = 0; segment < count; ++segment) {
            struct roce_packet packet, ack;
            uint32_t index = tx.next;
            assert(roce_requester_can_stage(&tx, 0));
            roce_requester_write_packet(&tx, index, segment, source + offset + segment * send_qp.pmtu,
                                        region.address + offset, lengths[wr], &packet);
            assert(packet.opcode == opcodes[index] && packet.psn == ((start + index) & ROCE_PSN_MASK));
            assert(packet.write_length == lengths[wr] && packet.remote_address == region.address + offset);
            roce_requester_sent(&tx, 1, 1);
            bool last = segment + 1 == count;
            assert(roce_responder_receive(&rx, &packet, &ack) == last);
            if (last)
                roce_requester_feedback(&tx, &ack, 2);
        }
        offset += lengths[wr];
    }
    assert(tx.acked == tx.total && rx.accepted == tx.total && rx.bytes_received == sizeof(source));
    assert(rx.msn == 3 && memcmp(source, destination, sizeof(source)) == 0);
}

static void test_two_payload_spans(void)
{
    uint8_t first[27], second[232], contiguous[sizeof(first) + sizeof(second)];
    for (size_t i = 0; i < sizeof(first); ++i)
        first[i] = (uint8_t)(i * 17);
    for (size_t i = 0; i < sizeof(second); ++i)
        second[i] = (uint8_t)(255 - i);
    memcpy(contiguous, first, sizeof(first));
    memcpy(contiguous + sizeof(first), second, sizeof(second));
    struct roce_packet packet = {
        .src = send_qp.local, .dst = send_qp.peer, .qpn = send_qp.peer_qpn, .psn = 123,
        .opcode = ROCE_WRITE_ONLY, .pkey = ROCE_PKEY, .udp_source = 0xc321, .ack_requested = true,
        .remote_address = 0x100000, .rkey = 0x1234, .write_length = sizeof(contiguous),
        .payload = contiguous, .payload_length = sizeof(contiguous),
    };
    uint8_t reference[ROCE_MAX_FRAME], encoded[ROCE_MAX_FRAME];
    size_t length = roce_encode(reference, sizeof(reference), &packet);
    assert(length > sizeof(contiguous));

    packet.payload = first;
    packet.payload_tail = second;
    packet.payload_tail_length = sizeof(second);
    assert(roce_encode(encoded, sizeof(encoded), &packet) == length && !memcmp(encoded, reference, length));

    uint16_t header, trailer;
    uint8_t footer[ROCE_MAX_TRAILER];
    assert(roce_encode_split(encoded, footer, &packet, &header, &trailer));
    assert(trailer == 5 && header + sizeof(contiguous) + trailer == length);
    memcpy(encoded + header, first, sizeof(first));
    memcpy(encoded + header + sizeof(first), second, sizeof(second));
    memcpy(encoded + length - trailer, footer, trailer);
    assert(!memcmp(encoded, reference, length));

    packet.payload_tail_length = sizeof(contiguous);
    assert(!roce_encode_split(encoded, footer, &packet, &header, &trailer));
    packet.payload_tail_length = sizeof(second);
    packet.payload_tail = NULL;
    assert(!roce_encode(encoded, sizeof(encoded), &packet));
}

int main(void)
{
    test_two_payload_spans();
    test_sequence_nak_and_lost_final_ack();
    test_window_timeout_and_address_validation();
    test_single_packet_and_access_errors();
    test_fatal_nak_progress();
    test_short_tx_burst();
    test_pipelined_writes();
    test_variable_write_packets();
    puts("RoCEv2 RC state tests passed");
    return 0;
}
