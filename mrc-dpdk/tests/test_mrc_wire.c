#include "../src/mrc_wire.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * Independent known answers assembled with Python struct.pack and zlib.crc32,
 * using OCP MRC 1.0 Tables 6-2 through 6-9 and 7-9 through 7-19.
 * The iCRC input prepends eight 0xff bytes and masks IPv4 ToS, TTL, checksum,
 * UDP checksum, and the BTH variant reserved byte per IBTA RoCEv2.
 */
static const uint8_t write_frame[] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00, 0x45, 0x46, 0x00, 0x50, 0x00, 0x00, 0x40, 0x00, 0x1f, 0x11,
    0x6f, 0x20, 0xc0, 0x00, 0x02, 0x01, 0xc6, 0x33, 0x64, 0x02, 0xc1, 0x23,
    0x12, 0xb7, 0x00, 0x3c, 0x00, 0x00, 0xcb, 0x30, 0xff, 0xff, 0x00, 0xc0,
    0xff, 0xee, 0xb0, 0x00, 0xff, 0x01, 0x24, 0x68, 0xac, 0xe0, 0x45, 0x67,
    0x80, 0x01, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33, 0x44, 0x44, 0x78, 0x9a,
    0xbc, 0xde, 0x00, 0x00, 0x00, 0x05, 0xf0, 0x0d, 0xca, 0xfe, 0x4d, 0x52,
    0x43, 0x21, 0x01, 0x00, 0x00, 0x00, 0x09, 0x78, 0x68, 0x3d,
};

static const uint8_t ack_frame[] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00, 0x45, 0x46, 0x00, 0x30, 0x00, 0x00, 0x40, 0x00, 0x1f, 0x11,
    0x6f, 0x40, 0xc0, 0x00, 0x02, 0x01, 0xc6, 0x33, 0x64, 0x02, 0xc1, 0x23,
    0x12, 0xb7, 0x00, 0x1c, 0x00, 0x00, 0xd1, 0x00, 0xab, 0xcd, 0x00, 0xc0,
    0xff, 0xee, 0x00, 0x00, 0xff, 0x01, 0x1f, 0x01, 0xa2, 0xb3, 0xc7, 0x3f,
    0x36, 0x36,
};

static const uint8_t sack_frame[] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00, 0x45, 0x46, 0x00, 0x50, 0x00, 0x00, 0x40, 0x00, 0x1f, 0x11,
    0x6f, 0x20, 0xc0, 0x00, 0x02, 0x01, 0xc6, 0x33, 0x64, 0x02, 0xc1, 0x23,
    0x12, 0xb7, 0x00, 0x3c, 0x00, 0x00, 0xdc, 0x00, 0xab, 0xcd, 0x00, 0xc0,
    0xff, 0xee, 0x20, 0xff, 0xff, 0xfe, 0x00, 0x42, 0xff, 0x85, 0x12, 0x34,
    0xc0, 0xff, 0x1a, 0x1b, 0x2a, 0x2b, 0x00, 0xff, 0xff, 0xfe, 0x00, 0xff,
    0x00, 0x40, 0x80, 0x01, 0xde, 0xad, 0xbe, 0xef, 0x00, 0x03, 0xfe, 0xdc,
    0x5a, 0xa5, 0xd5, 0x09, 0x87, 0x65, 0x05, 0xee, 0x46, 0x5d,
};

static const uint8_t nack_frame[] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00, 0x45, 0x46, 0x00, 0x40, 0x00, 0x00, 0x40, 0x00, 0x1f, 0x11,
    0x6f, 0x30, 0xc0, 0x00, 0x02, 0x01, 0xc6, 0x33, 0x64, 0x02, 0xc1, 0x23,
    0x12, 0xb7, 0x04, 0x2c, 0x00, 0x00, 0xdd, 0x00, 0xab, 0xcd, 0x00, 0xc0,
    0xff, 0xee, 0x20, 0x00, 0x12, 0x34, 0x00, 0x00, 0x02, 0xe3, 0x00, 0x00,
    0xc1, 0x23, 0x1a, 0x1b, 0x2a, 0x2b, 0x00, 0x00, 0x12, 0x34, 0x20, 0x00,
    0x31, 0xa2, 0x51, 0xfd, 0x37, 0x9f,
};

static const uint8_t probe_frame[] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00, 0x45, 0x46, 0x00, 0x3c, 0x00, 0x00, 0x40, 0x00, 0x1f, 0x11,
    0x6f, 0x34, 0xc0, 0x00, 0x02, 0x01, 0xc6, 0x33, 0x64, 0x02, 0xc1, 0x23,
    0x13, 0x6b, 0x00, 0x28, 0x00, 0x00, 0xde, 0x00, 0xab, 0xcd, 0x00, 0xc0,
    0xff, 0xee, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x26, 0x11, 0x19,
    0x00, 0x00, 0x1a, 0x1b, 0x2a, 0x2b, 0x12, 0x34, 0x00, 0x01, 0x24, 0x6b,
    0x27, 0x91,
};

static const uint8_t erth_frame[] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00, 0x45, 0x46, 0x00, 0x3c, 0x00, 0x00, 0x40, 0x00, 0x1f, 0x11,
    0x6f, 0x34, 0xc0, 0x00, 0x02, 0x01, 0xc6, 0x33, 0x64, 0x02, 0xc1, 0x23,
    0x12, 0xb7, 0x00, 0x28, 0x00, 0x00, 0xd8, 0x00, 0xab, 0xcd, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x72, 0x34, 0x00, 0x01, 0x00, 0x5a, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x45, 0x67, 0x80, 0x01, 0x5b, 0x50,
    0x92, 0xa1,
};

static const uint8_t eeth_frame[] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00, 0x45, 0x46, 0x00, 0x50, 0x00, 0x00, 0x40, 0x00, 0x1f, 0x11,
    0x6f, 0x20, 0xc0, 0x00, 0x02, 0x01, 0xc6, 0x33, 0x64, 0x02, 0xc1, 0x23,
    0x12, 0xb7, 0x00, 0x3c, 0x00, 0x00, 0xd9, 0x00, 0xab, 0xcd, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x72, 0x34, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56, 0x78,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x67, 0x05, 0x92,
};

static const uint8_t trim_frame[] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00, 0x45, 0xbe, 0x00, 0x30, 0x00, 0x00, 0x40, 0x00, 0x1f, 0x11,
    0x6e, 0xc8, 0xc0, 0x00, 0x02, 0x01, 0xc6, 0x33, 0x64, 0x02, 0xc1, 0x23,
    0x12, 0xb7, 0x00, 0x3c, 0x00, 0x00, 0xcb, 0x30, 0xff, 0xff, 0x00, 0xc0,
    0xff, 0xee, 0xb0, 0x00, 0xff, 0x01, 0x24, 0x68, 0xac, 0xe0, 0x45, 0x67,
    0x80, 0x01,
};

static const uint8_t unknown_frame[] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00, 0x45, 0x46, 0x00, 0x2c, 0x00, 0x00, 0x40, 0x00, 0x1f, 0x11,
    0x6f, 0x44, 0xc0, 0x00, 0x02, 0x01, 0xc6, 0x33, 0x64, 0x02, 0xc1, 0x23,
    0x12, 0xb7, 0x00, 0x18, 0x00, 0x00, 0xc5, 0x00, 0xab, 0xcd, 0x00, 0xc0,
    0xff, 0xee, 0x80, 0x01, 0x23, 0x45, 0x7f, 0x0c, 0x95, 0xde,
};

static struct mrc_wire_packet check_fixture(const uint8_t *frame, size_t len,
                                            const struct mrc_wire_packet *expected)
{
    struct mrc_wire_packet decoded;
    uint8_t encoded[128];
    size_t encoded_len;
    assert(mrc_wire_encode(encoded, sizeof(encoded), expected, &encoded_len) == 0);
    assert(encoded_len == len);
    assert(memcmp(encoded, frame, len) == 0);
    assert(mrc_wire_decode(frame, len, &decoded) == 0);
    assert(mrc_wire_encode(encoded, sizeof(encoded), &decoded, &encoded_len) == 0);
    assert(encoded_len == len);
    assert(memcmp(encoded, frame, len) == 0);
    return decoded;
}

static struct mrc_wire_packet packet_for(uint8_t opcode)
{
    return (struct mrc_wire_packet) {
        .ipv4 = {
            .src_mac = {2, 0, 0, 0, 0, 1}, .dst_mac = {2, 0, 0, 0, 0, 2},
            .src_ip = 0xc0000201, .dst_ip = 0xc6336402, .src_port = 0xc123,
            .dscp = 17, .ecn = 2, .ttl = 31,
        },
        .bth = {.opcode = opcode, .p_key = 0xabcd, .dqp = 0xc0ffee},
    };
}

static void test_write(void)
{
    const uint8_t data[] = {'M', 'R', 'C', '!', 1};
    struct mrc_wire_packet packet = packet_for(MRC_WRITE_ONLY_IMM);
    packet.bth.p_key = 0xffff;
    packet.bth.psn = 0x00ff01;
    packet.bth.ack_request = true;
    packet.bth.retransmit = true;
    packet.bth.timestamp = true;
    packet.body.write = (struct mrc_wire_write) {
        .rqmsn = 0x2468, .msn = 0xace0, .tx_timestamp = 0x4567, .tsr = true,
        .va = UINT64_C(0x1111222233334444), .r_key = 0x789abcde, .dma_len = sizeof(data),
        .imm_data = 0xf00dcafe, .data = data, .data_len = sizeof(data),
    };
    struct mrc_wire_packet decoded = check_fixture(write_frame, sizeof(write_frame), &packet);
    assert(decoded.ipv4.dst_port == MRC_UDP_DEFAULT_PORT);
    assert(decoded.bth.ack_request && decoded.bth.retransmit && decoded.bth.timestamp);
    assert(decoded.body.write.data == write_frame + 82);
    assert(decoded.body.write.data_len == sizeof(data));
    assert(decoded.body.write.dma_len == sizeof(data));
    assert(decoded.body.write.imm_data == 0xf00dcafe);
    assert(memcmp(decoded.body.write.data, data, sizeof(data)) == 0);

    uint8_t encoded[128];
    size_t len;
    packet.bth.opcode = MRC_WRITE_ONLY;
    packet.bth.timestamp = false;
    packet.body.write.data = NULL;
    packet.body.write.data_len = 0;
    packet.body.write.dma_len = 0;
    assert(mrc_wire_encode(encoded, sizeof(encoded), &packet, &len) == 0);
    assert(len == MRC_WIRE_IPV4_HEADER_LEN + MRC_BTH_LEN + MRC_METH_LEN +
                  MRC_RETH_LEN + MRC_ICRC_LEN);
    assert(mrc_wire_decode(encoded, len, &decoded) == 0);
    assert(decoded.body.write.data_len == 0);
}

static void test_ack(void)
{
    struct mrc_wire_packet packet = packet_for(MRC_TRANSPORT_ACK);
    packet.bth.psn = 0x00ff01;
    packet.body.aeth = (struct mrc_wire_aeth){.syndrome = 0x1f, .msn = 0x01a2b3};
    check_fixture(ack_frame, sizeof(ack_frame), &packet);
}

static void test_sack(void)
{
    struct mrc_wire_packet packet = packet_for(MRC_RELIABILITY_SACK);
    packet.bth.psn = 0xfffffe;
    packet.bth.retransmit = true;
    packet.body.sack = (struct mrc_wire_sack) {
        .mark = MRC_SACK_ALWAYS_SKIP, .probe_response = true, .ack_psn_offset = -123,
        .entropy = 0x1234c0ff, .spdcid = 0x1a1b, .dpdcid = 0x2a2b, .cack_psn = 0xfffffe,
        .mpr = 255, .sack_offset = 64, .bitmap = UINT64_C(0x8001deadbeef0003),
        .tx_timestamp = 0xfedc, .ooo_count = 0x5aa5, .restore_cwnd = true,
        .rcv_cwnd_pen = 0x55, .rcvd_bytes = 0x098765,
    };
    struct mrc_wire_packet decoded = check_fixture(sack_frame, sizeof(sack_frame), &packet);
    assert(decoded.body.sack.ack_psn_offset == -123);
    assert(decoded.body.sack.bitmap == UINT64_C(0x8001deadbeef0003));
}

static void test_nack(void)
{
    struct mrc_wire_packet packet = packet_for(MRC_RELIABILITY_NACK);
    packet.bth.psn = 0x001234;
    packet.bth.retransmit = true;
    packet.body.nack = (struct mrc_wire_nack) {
        .reason = MRC_NACK_TRIMMED_LASTHOP, .vendor_info = 0xe3, .entropy = 0xc123,
        .spdcid = 0x1a1b, .dpdcid = 0x2a2b, .nack_psn = 0x001234, .tx_timestamp = 0x31a2,
        .original_udp_len = 1068,
    };
    struct mrc_wire_packet decoded = check_fixture(nack_frame, sizeof(nack_frame), &packet);
    assert(decoded.body.nack.original_udp_len == 1068);
}

static void test_probe(void)
{
    struct mrc_wire_packet packet = packet_for(MRC_RELIABILITY_PROBE);
    packet.ipv4.dst_port = 4971;
    packet.body.probe = (struct mrc_wire_probe) {
        .vendor_info = 0x26, .probe_id = 0x1119, .spdcid = 0x1a1b, .dpdcid = 0x2a2b,
        .timestamp = true, .tx_timestamp = 0x1234,
    };
    struct mrc_wire_packet decoded = check_fixture(probe_frame, sizeof(probe_frame), &packet);
    assert(decoded.ipv4.dst_port == 4971);
    assert(decoded.body.probe.timestamp);
}

static void test_endpoint(void)
{
    struct mrc_wire_packet request = packet_for(MRC_ENDPOINT_REQUEST);
    request.bth.dqp = MRC_ENDPOINT_QPN;
    request.bth.psn = 0x7234;
    request.body.endpoint_request = (struct mrc_wire_endpoint_request) {
        .op = MRC_ENDPOINT_EV_PROBE, .vendor_info = 0x5a, .tx_timestamp = 0x4567,
        .timestamp = true, .tsr = true,
    };
    check_fixture(erth_frame, sizeof(erth_frame), &request);

    struct mrc_wire_packet response = packet_for(MRC_ENDPOINT_RESPONSE);
    response.bth.dqp = MRC_ENDPOINT_QPN;
    response.bth.psn = 0x7234;
    response.body.endpoint_response = (struct mrc_wire_endpoint_response) {
        .op = MRC_ENDPOINT_EV_PROBE, .tx_timestamp = 0x5678,
    };
    check_fixture(eeth_frame, sizeof(eeth_frame), &response);

    request.body.endpoint_request.op = MRC_ENDPOINT_PORT_STATUS;
    request.body.endpoint_request.port_status_mask = 0x80001001;
    struct mrc_wire_packet decoded;
    uint8_t frame[128];
    size_t len;
    assert(mrc_wire_encode(frame, sizeof(frame), &request, &len) == 0);
    assert(frame[58] == 0x80 && frame[59] == 0 && frame[60] == 0x10 && frame[61] == 1);
    assert(mrc_wire_decode(frame, len, &decoded) == 0);
    assert(decoded.body.endpoint_request.port_status_mask == 0x80001001);

    request.bth.dqp++;
    assert(mrc_wire_encode(frame, sizeof(frame), &request, &len) == -EINVAL);
}

static void test_trimmed_request(void)
{
    struct mrc_wire_packet decoded;
    assert(mrc_wire_decode(trim_frame, sizeof(trim_frame), &decoded) == -EBADMSG);

    struct mrc_wire_trimmed trim;
    assert(mrc_wire_decode_trimmed(trim_frame, sizeof(trim_frame), 46, &trim) ==
           -EPROTONOSUPPORT);
    assert(mrc_wire_decode_trimmed(trim_frame, sizeof(trim_frame), 47, &trim) == 0);
    assert(trim.ipv4.dscp == 47 && trim.ipv4.src_port == 0xc123);
    assert(trim.bth.opcode == MRC_WRITE_ONLY_IMM);
    assert(trim.bth.psn == 0x00ff01 && trim.bth.dqp == 0xc0ffee && trim.bth.retransmit);
    assert(trim.original_udp_len == 60);
    assert(trim.timestamp && trim.tsr && trim.tx_timestamp == 0x4567);

    size_t prefix_len = MRC_WIRE_IPV4_HEADER_LEN + MRC_BTH_LEN;
    /* Some switches leave the IPv4 total length untouched in a trim. */
    assert(mrc_wire_decode_trimmed(trim_frame, prefix_len, 47, &trim) == 0);
    assert(!trim.timestamp && trim.original_udp_len == 60);
    assert(mrc_wire_decode_trimmed(trim_frame, prefix_len - 1, 47, &trim) == -EBADMSG);
    assert(mrc_wire_decode_trimmed(ack_frame, sizeof(ack_frame), 17, &trim) == -EBADMSG);
    /* A short packet may be marked even though it remains physically intact. */
    assert(mrc_wire_decode_trimmed(write_frame, sizeof(write_frame), 17, &trim) == 0);
}

static void test_unknown_opcode(void)
{
    struct mrc_wire_packet decoded;
    struct mrc_wire_ipv4 ipv4;
    struct mrc_wire_bth bth;
    assert(mrc_wire_decode(unknown_frame, sizeof(unknown_frame), &decoded) == -EPROTONOSUPPORT);
    assert(mrc_wire_decode_header(unknown_frame, sizeof(unknown_frame), &ipv4, &bth) == 0);
    assert(ipv4.src_ip == 0xc0000201 && ipv4.dst_port == MRC_UDP_DEFAULT_PORT);
    assert(bth.opcode == 0xc5 && bth.psn == 0x012345 && bth.dqp == 0xc0ffee);
    uint8_t corrupted[sizeof(unknown_frame)];
    memcpy(corrupted, unknown_frame, sizeof(corrupted));
    corrupted[sizeof(corrupted) - 1] ^= 1;
    assert(mrc_wire_decode_header(corrupted, sizeof(corrupted), &ipv4, &bth) == -EBADMSG);
    assert(mrc_wire_decode_header(nack_frame, sizeof(nack_frame), &ipv4, &bth) == -EBADMSG);
}

static void test_bad_frames(void)
{
    struct mrc_wire_packet decoded;
    assert(mrc_wire_decode(write_frame, sizeof(write_frame) - 1, &decoded) == -EBADMSG);
    assert(mrc_wire_decode(write_frame, 14, &decoded) == -EBADMSG);

    uint8_t frame[sizeof(write_frame)];
    memcpy(frame, write_frame, sizeof(frame));
    frame[82] ^= 1; /* Corrupted payload. */
    assert(mrc_wire_decode(frame, sizeof(frame), &decoded) == -EBADMSG);
    memcpy(frame, write_frame, sizeof(frame));
    frame[sizeof(frame) - 1] ^= 1; /* Corrupted RoCE iCRC trailer. */
    assert(mrc_wire_decode(frame, sizeof(frame), &decoded) == -EBADMSG);

    memcpy(frame, write_frame, sizeof(frame));
    frame[22]--; /* Router decrements TTL and updates the IPv4 checksum. */
    frame[24]++;
    assert(mrc_wire_decode(frame, sizeof(frame), &decoded) == 0);
    assert(decoded.ipv4.ttl == 30);
    frame[24]--; /* Reject the same packet with an invalid IPv4 checksum. */
    assert(mrc_wire_decode(frame, sizeof(frame), &decoded) == -EBADMSG);

    struct mrc_wire_packet packet = packet_for(MRC_TRANSPORT_ACK);
    size_t len;
    assert(mrc_wire_encode(frame, 32, &packet, &len) == -EMSGSIZE);
    packet.bth.dqp = MRC_WIRE_PSN_MASK + 1;
    assert(mrc_wire_encode(frame, sizeof(frame), &packet, &len) == -EINVAL);
    packet.bth.dqp = 1;
    packet.bth.opcode = 0xff;
    assert(mrc_wire_encode(frame, sizeof(frame), &packet, &len) == -EPROTONOSUPPORT);
}

int main(void)
{
    test_write();
    test_ack();
    test_sack();
    test_nack();
    test_probe();
    test_endpoint();
    test_trimmed_request();
    test_unknown_opcode();
    test_bad_frames();
    puts("MRC wire codec tests passed");
    return 0;
}
