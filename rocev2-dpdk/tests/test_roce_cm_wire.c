#include "roce_cm_wire.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Fixed wire fixtures from the IBTA Chapter 12 tables and Linux CMA IPv4
 * header layout. Indices are absolute MAD byte offsets. */
static const uint8_t req_fixture[256] = {
    [0] = 0x01, 0x07, 0x02, 0x03,
    [8] = 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    [16] = 0x00, 0x10,
    [20] = 0x12, 0x34, 0x56, 0x78,
    [24] = 0x10, 0x20, 0x30, 0x40,
    [29] = 0x00, 0x15, 0xb3,
    [32] = 0x00, 0x00, 0x00, 0x00, 0x01, 0x06, 0xb6, 0xf8,
    [40] = 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    [56] = 0x01, 0x02, 0x03, 0x08,
    [63] = 0x05,
    [67] = 0xa1, 0xab, 0xc1, 0x23, 0x8e,
    [72] = 0xff, 0xff, 0x37, 0xf8, 0xff, 0xff, 0xff, 0xff,
    [90] = 0xff, 0xff, 192, 0, 2, 10,
    [106] = 0xff, 0xff, 198, 51, 100, 31,
    [112] = 0xab, 0xcd, 0xe0, 0x10, 0xa3, 0x40, 0x70, 0xa8,
    [164] = 0x00, 0x40, 0xfa, 0xb1,
    [180] = 192, 0, 2, 10,
    [196] = 198, 51, 100, 31, 'o', 'c', 's',
};

static const uint8_t rep_fixture[256] = {
    [0] = 0x01, 0x07, 0x02, 0x03,
    [8] = 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    [16] = 0x00, 0x13,
    [20] = 0xf1, 0xe2, 0xd3, 0xc4,
    [24] = 0x80, 0x70, 0x60, 0x50, 0x10, 0x20, 0x30, 0x40,
    [36] = 0x55, 0x66, 0x77, 0x01,
    [43] = 0xa2, 0x98, 0x76, 0x54, 0xb3,
    [48] = 0x06, 0x04, 0x8b, 0x90,
    [52] = 0x12, 0x21, 0x34, 0x43, 0x56, 0x65, 0x78, 0x87,
    [60] = 0xaa, 0x00, 0x55,
};

static const uint8_t mra_fixture[256] = {
    [0] = 0x01, 0x07, 0x02, 0x03,
    [8] = 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    [16] = 0x00, 0x11,
    [24] = 0x10, 0x20, 0x30, 0x40, 0x80, 0x70, 0x60, 0x50,
    [32] = 0x40, 0xf8, 0x06, 0xce,
    [255] = 0x43,
};

static const uint8_t rtu_fixture[256] = {
    [0] = 0x01, 0x07, 0x02, 0x03,
    [8] = 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    [16] = 0x00, 0x14,
    [24] = 0x10, 0x20, 0x30, 0x40, 0x80, 0x70, 0x60, 0x50,
    [32] = 0x06, 0xce,
};

static const uint8_t dreq_fixture[256] = {
    [0] = 0x01, 0x07, 0x02, 0x03,
    [8] = 0x20, 0x40, 0x60, 0x80, 0xa0, 0xc0, 0xe0, 0xf0,
    [16] = 0x00, 0x15,
    [24] = 0x80, 0x70, 0x60, 0x50, 0x10, 0x20, 0x30, 0x40,
    [32] = 0x11, 0x23, 0x45,
    [36] = 0xaa, 0xcd,
};

static const uint8_t drep_fixture[256] = {
    [0] = 0x01, 0x07, 0x02, 0x03,
    [8] = 0x20, 0x40, 0x60, 0x80, 0xa0, 0xc0, 0xe0, 0xf0,
    [16] = 0x00, 0x16,
    [24] = 0x10, 0x20, 0x30, 0x40, 0x80, 0x70, 0x60, 0x50,
    [32] = 0x89, 0x01,
};

static void check_fixture(const struct roce_cm_message *message, const uint8_t *fixture,
                          size_t offset, size_t private_size)
{
    struct roce_cm_message decoded;
    uint8_t encoded[257];

    memset(encoded, 0x5a, sizeof(encoded));
    assert(roce_cm_encode(encoded, sizeof(encoded), message) == 256);
    assert(!memcmp(encoded, fixture, 256));
    assert(encoded[256] == 0x5a);

    assert(roce_cm_decode(&decoded, fixture, 256));
    assert(decoded.type == message->type);
    assert(decoded.tid == message->tid);
    assert(decoded.local_comm_id == message->local_comm_id);
    assert(decoded.remote_comm_id == message->remote_comm_id);
    assert(decoded.private_data == fixture + offset);
    assert(decoded.private_length == private_size);
    assert(roce_cm_encode(encoded, sizeof(encoded), &decoded) == 256);
    assert(!memcmp(encoded, fixture, 256));
    assert(roce_cm_decode(&decoded, encoded, 256));
    assert(roce_cm_encode(encoded, sizeof(encoded), &decoded) == 256);
    assert(!memcmp(encoded, fixture, 256));
}

static void check_req(void)
{
    uint8_t data[ROCE_CMA_IPV4_SIZE + 3] = {[36] = 'o', 'c', 's'};
    const struct roce_cma_ipv4 addresses = {
        .source_port = 0xfab1,
        .source_ipv4 = 0xc000020a,
        .destination_ipv4 = 0xc633641f,
    };
    const struct roce_cm_message req = {
        .type = ROCE_CM_REQ,
        .tid = UINT64_C(0x1122334455667788),
        .local_comm_id = 0x10203040,
        .connection = {
            .local_ca_guid = UINT64_C(0x0123456789abcdef),
            .local_qpn = 0x010203,
            .starting_psn = 0xabc123,
            .ece_vendor_id = 0x0015b3,
            .ece_attr_mod = 0x12345678,
            .responder_resources = 8,
            .initiator_depth = 5,
            .rnr_retry_count = 7,
            .flow_control = true,
            .srq = true,
        },
        .req = {
            .service_id = ROCE_CMA_TCP_SERVICE_BASE | 0xb6f8,
            .pkey = 0xffff,
            .mtu = 3,
            .remote_response_timeout = 20,
            .local_response_timeout = 17,
            .retry_count = 6,
            .max_cm_retries = 15,
            .primary = {
                .local_lid = 0xffff,
                .remote_lid = 0xffff,
                .local_gid = {[10] = 0xff, 0xff, 192, 0, 2, 10},
                .remote_gid = {[10] = 0xff, 0xff, 198, 51, 100, 31},
                .flow_label = 0xabcde,
                .packet_rate = 0x10,
                .traffic_class = 0xa3,
                .hop_limit = 64,
                .service_level = 7,
                .ack_timeout = 21,
            },
        },
        .private_data = data,
        .private_length = sizeof(data),
    };
    struct roce_cma_ipv4 decoded_addresses;

    assert(roce_cma_encode_ipv4(data, sizeof(data), &addresses));
    check_fixture(&req, req_fixture, 164, 92);
    assert(roce_cma_decode_ipv4(&decoded_addresses, req_fixture + 164, 92));
    assert(decoded_addresses.source_port == addresses.source_port);
    assert(decoded_addresses.source_ipv4 == addresses.source_ipv4);
    assert(decoded_addresses.destination_ipv4 == addresses.destination_ipv4);
    assert(ROCE_CMA_IPV4_REQ_USER_SIZE == 56);
}

static void check_rep_rtu(void)
{
    const uint8_t rep_data[] = {0xaa, 0x00, 0x55};
    const uint8_t rtu_data[] = {0x06, 0xce};
    const struct roce_cm_message rep = {
        .type = ROCE_CM_REP,
        .tid = UINT64_C(0x1122334455667788),
        .local_comm_id = 0x80706050,
        .remote_comm_id = 0x10203040,
        .connection = {
            .local_ca_guid = UINT64_C(0x1221344356657887),
            .local_qpn = 0x556677,
            .starting_psn = 0x987654,
            .ece_vendor_id = 0x01a2b3,
            .ece_attr_mod = 0xf1e2d3c4,
            .responder_resources = 6,
            .initiator_depth = 4,
            .rnr_retry_count = 4,
            .flow_control = true,
            .srq = true,
        },
        .rep = {.target_ack_delay = 17, .failover_accepted = 1},
        .private_data = rep_data,
        .private_length = sizeof(rep_data),
    };
    const struct roce_cm_message rtu = {
        .type = ROCE_CM_RTU,
        .tid = rep.tid,
        .local_comm_id = rep.remote_comm_id,
        .remote_comm_id = rep.local_comm_id,
        .private_data = rtu_data,
        .private_length = sizeof(rtu_data),
    };

    check_fixture(&rep, rep_fixture, 60, 196);
    check_fixture(&rtu, rtu_fixture, 32, 224);
}

static void check_lengths(void)
{
    const uint8_t *fixtures[] = {req_fixture, rep_fixture, rtu_fixture, dreq_fixture, drep_fixture};
    uint8_t data[225];
    uint8_t mad[257];

    memset(data, 0x5a, sizeof(data));
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        struct roce_cm_message message;

        assert(roce_cm_decode(&message, fixtures[i], 256));
        message.private_data = data;
        assert(roce_cm_encode(mad, 256, &message) == 256);
        assert(mad[255] == 0x5a);
        ++message.private_length;
        assert(!roce_cm_encode(mad, 256, &message));
        message.private_length = 1;
        message.private_data = NULL;
        assert(!roce_cm_encode(mad, 256, &message));
        message.private_length = 0;
        assert(!roce_cm_encode(mad, 255, &message));
        assert(roce_cm_encode(mad, 256, &message) == 256);
        assert(mad[255] == 0);
        assert(!roce_cm_decode(&message, mad, 255));
        assert(!roce_cm_decode(&message, mad, 257));
    }
}

static void check_mra(void)
{
    uint8_t private_data[ROCE_CM_MRA_PRIVATE_SIZE] = {0x06, 0xce, [221] = 0x43};
    struct roce_cm_message message = {
        .type = ROCE_CM_MRA, .tid = UINT64_C(0x1122334455667788),
        .local_comm_id = 0x10203040, .remote_comm_id = 0x80706050,
        .mra = {.message = ROCE_CM_MRA_REP, .service_timeout = 31},
        .private_data = private_data, .private_length = sizeof(private_data),
    };
    check_fixture(&message, mra_fixture, 34, sizeof(private_data));

    uint8_t mad[ROCE_CM_MAD_SIZE];
    memcpy(mad, mra_fixture, sizeof(mad));
    mad[32] |= 0x3f;
    mad[33] |= 7;
    struct roce_cm_message decoded;
    assert(roce_cm_decode(&decoded, mad, sizeof(mad)));
    assert(decoded.mra.message == ROCE_CM_MRA_REP && decoded.mra.service_timeout == 31);
    mad[32] = 0xc0;
    assert(!roce_cm_decode(&decoded, mad, sizeof(mad)));

    message.mra.message = (enum roce_cm_mra_message)3;
    assert(!roce_cm_encode(mad, sizeof(mad), &message));
    message.mra.message = ROCE_CM_MRA_REQ;
    message.mra.service_timeout = 32;
    assert(!roce_cm_encode(mad, sizeof(mad), &message));
    message.mra.service_timeout = 0;
    ++message.private_length;
    assert(!roce_cm_encode(mad, sizeof(mad), &message));
}

static void check_disconnect(void)
{
    const uint8_t request_data[] = {0xaa, 0xcd};
    const uint8_t reply_data[] = {0x89, 0x01};
    struct roce_cm_message request = {
        .type = ROCE_CM_DREQ,
        .tid = UINT64_C(0x20406080a0c0e0f0),
        .local_comm_id = 0x80706050,
        .remote_comm_id = 0x10203040,
        .dreq.remote_qpn = 0x112345,
        .private_data = request_data,
        .private_length = sizeof(request_data),
    };
    const struct roce_cm_message reply = {
        .type = ROCE_CM_DREP,
        .tid = request.tid,
        .local_comm_id = request.remote_comm_id,
        .remote_comm_id = request.local_comm_id,
        .private_data = reply_data,
        .private_length = sizeof(reply_data),
    };
    uint8_t mad[256];

    check_fixture(&request, dreq_fixture, 36, 220);
    check_fixture(&reply, drep_fixture, 32, 224);
    assert(roce_cm_decode(&request, dreq_fixture, sizeof(mad)));
    assert(request.dreq.remote_qpn == 0x112345);
    request.dreq.remote_qpn = 0x1000000;
    assert(!roce_cm_encode(mad, sizeof(mad), &request));
}

static void check_invalid_fields(void)
{
    struct roce_cm_message message;
    struct roce_cma_ipv4 addresses;
    uint8_t mad[256];
    uint8_t data[36];
    const struct {
        size_t offset;
        uint8_t value;
    } invalid_req[] = {
        {0, 0x02}, {1, 0x04}, {2, 0x01}, {3, 0x83}, {5, 0x01}, {17, 0x15},
        {67, 0xa3}, {74, 0x3f}, {74, 0x67},
    };

    for (size_t i = 0; i < sizeof(invalid_req) / sizeof(invalid_req[0]); ++i) {
        memcpy(mad, req_fixture, sizeof(mad));
        mad[invalid_req[i].offset] = invalid_req[i].value;
        assert(!roce_cm_decode(&message, mad, sizeof(mad)));
    }
    assert(roce_cm_decode(&message, req_fixture, 256));
    message.req.primary.flow_label = 0x100000;
    assert(!roce_cm_encode(mad, sizeof(mad), &message));
    assert(roce_cm_decode(&message, rep_fixture, 256));
    message.connection.local_qpn = 1;
    assert(!roce_cm_encode(mad, sizeof(mad), &message));
    memcpy(mad, rep_fixture, sizeof(mad));
    mad[50] = 0x8f;
    assert(!roce_cm_decode(&message, mad, sizeof(mad)));

    memcpy(data, req_fixture + 164, sizeof(data));
    assert(!roce_cma_decode_ipv4(&addresses, data, sizeof(data) - 1));
    assert(!roce_cma_encode_ipv4(data, sizeof(data) - 1, &addresses));
    data[0] = 0x01;
    assert(!roce_cma_decode_ipv4(&addresses, data, sizeof(data)));
    data[0] = 0;
    data[1] = 0x60;
    assert(!roce_cma_decode_ipv4(&addresses, data, sizeof(data)));
}

static void check_cm_frame(void)
{
    /* Ethernet through DETH and the final ICRC, generated independently with
     * Python struct packing, one's-complement IPv4 checksum and binascii.crc32. */
    static const uint8_t header[] = {
        0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x08, 0x00,
        0x45, 0x00, 0x01, 0x34, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11, 0xb5, 0xb5,
        0xc0, 0x00, 0x02, 0x01, 0xc0, 0x00, 0x02, 0x02,
        0xc1, 0x23, 0x12, 0xb7, 0x01, 0x20, 0x00, 0x00,
        0x64, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0xab, 0xcd, 0xef,
        0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    };
    static const uint8_t icrc[] = {0xcb, 0x99, 0xdb, 0x65};
    static const struct {
        size_t offset;
        uint8_t value;
        uint8_t icrc[4];
    } invalid[] = {
        {49, 0x02, {0x17, 0xe1, 0xea, 0x08}}, /* Destination QP. */
        {61, 0x02, {0x10, 0xe1, 0x53, 0x71}}, /* Source QP. */
        {57, 0x01, {0xf1, 0x26, 0xc9, 0x5b}}, /* Q_Key. */
        {43, 0x10, {0x3e, 0x83, 0xb7, 0xe8}}, /* Pad count. */
    };
    static const struct {
        size_t length;
        uint8_t ip_length;
        uint8_t ip_checksum;
        uint8_t udp_length;
        uint8_t icrc[4];
    } invalid_lengths[] = {
        {318, 0x30, 0xb9, 0x1c, {0x7e, 0x57, 0xa5, 0x9c}},
        {326, 0x38, 0xb1, 0x24, {0xfd, 0xb6, 0xaf, 0x21}},
    };
    struct roce_cm_frame source = {
        .src = {.mac = {2, 0, 0, 0, 0, 1}, .ipv4 = 0xc0000201},
        .dst = {.mac = {2, 0, 0, 0, 0, 2}, .ipv4 = 0xc0000202},
        .udp_source = 0xc123,
        .pkey = 0xffff,
        .psn = 0xabcdef,
        .mad = rtu_fixture,
    };
    struct roce_cm_frame decoded;
    struct roce_cm_message message;
    struct roce_packet rc_packet;
    uint8_t expected[322];
    uint8_t frame[326];

    memcpy(expected, header, sizeof(header));
    memcpy(expected + sizeof(header), rtu_fixture, sizeof(rtu_fixture));
    memcpy(expected + 318, icrc, sizeof(icrc));
    assert(ROCE_CM_FRAME_SIZE == sizeof(expected));
    assert(roce_cm_frame_encode(frame, sizeof(frame), &source) == sizeof(expected));
    assert(!memcmp(frame, expected, sizeof(expected)));
    assert(roce_cm_frame_decode(&decoded, expected, sizeof(expected)));
    assert(decoded.mad == expected + sizeof(header));
    assert(decoded.src.ipv4 == source.src.ipv4 && decoded.dst.ipv4 == source.dst.ipv4);
    assert(!memcmp(decoded.src.mac, source.src.mac, ROCE_MAC_LEN));
    assert(!memcmp(decoded.dst.mac, source.dst.mac, ROCE_MAC_LEN));
    assert(decoded.udp_source == source.udp_source && decoded.pkey == source.pkey);
    assert(decoded.psn == source.psn);
    assert(roce_cm_decode(&message, decoded.mad, ROCE_CM_MAD_SIZE));
    assert(message.type == ROCE_CM_RTU && message.remote_comm_id == 0x80706050);
    assert(!roce_decode(&rc_packet, expected, sizeof(expected)));

    /* The UDP checksum is optional for IPv4. This independent value is valid. */
    frame[40] = 0x0f;
    frame[41] = 0xcf;
    assert(roce_cm_frame_decode(&decoded, frame, sizeof(expected)));
    frame[41] ^= 1;
    assert(!roce_cm_frame_decode(&decoded, frame, sizeof(expected)));
    assert(!roce_cm_frame_encode(frame, sizeof(expected) - 1, &source));
    source.psn = 0x1000000;
    assert(!roce_cm_frame_encode(frame, sizeof(frame), &source));

    /* Stock Linux GSI may set AckReq on a QP1 UD packet. */
    static const uint8_t gsi_icrc[] = {0x8b, 0x6f, 0x12, 0x14};
    memcpy(frame, expected, sizeof(expected));
    frame[50] = 0x80;
    assert(!roce_cm_frame_decode(&decoded, frame, sizeof(expected)));
    memcpy(frame + 318, gsi_icrc, sizeof(gsi_icrc));
    assert(roce_cm_frame_decode(&decoded, frame, sizeof(expected)));
    assert(roce_cm_decode(&message, decoded.mad, ROCE_CM_MAD_SIZE) && message.type == ROCE_CM_RTU);

    for (size_t length = 0; length < sizeof(expected); ++length)
        assert(!roce_cm_frame_decode(&decoded, expected, length));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        memcpy(frame, expected, sizeof(expected));
        frame[invalid[i].offset] = invalid[i].value;
        memcpy(frame + 318, invalid[i].icrc, sizeof(icrc));
        assert(!roce_cm_frame_decode(&decoded, frame, sizeof(expected)));
    }
    for (size_t i = 0; i < sizeof(invalid_lengths) / sizeof(invalid_lengths[0]); ++i) {
        memset(frame, 0, sizeof(frame));
        memcpy(frame, expected, 318);
        frame[17] = invalid_lengths[i].ip_length;
        frame[25] = invalid_lengths[i].ip_checksum;
        frame[39] = invalid_lengths[i].udp_length;
        memcpy(frame + invalid_lengths[i].length - sizeof(icrc), invalid_lengths[i].icrc,
               sizeof(icrc));
        assert(!roce_cm_frame_decode(&decoded, frame, invalid_lengths[i].length));
    }
    memcpy(frame, expected, sizeof(expected));
    frame[64] ^= 1;
    assert(!roce_cm_frame_decode(&decoded, frame, sizeof(expected)));
}

int main(void)
{
    check_req();
    check_rep_rtu();
    check_lengths();
    check_mra();
    check_disconnect();
    check_invalid_fields();
    check_cm_frame();
    puts("RoCEv2 CM MAD wire tests passed");
    return 0;
}
