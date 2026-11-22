#include "mrc_connect.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct mrc_connect_offer sample_offer(void)
{
    return (struct mrc_connect_offer) {
        .receive = {
            .qpn = 0xabcdef, .initial_psn = 0x12345, .initial_msn = 0xfedcba,
            .initial_rqmsn = 0xabcd, .max_mpr = 64, .max_wimm = 0x1234,
            .memory = {
                .present = true,
                .address = UINT64_C(0x0102030405060708),
                .length = UINT64_C(0x0000000102030405),
                .r_key = UINT32_C(0x89abcdef),
            },
        },
        .pmtu = 2048, .p_key = 0x8123,
        .supported = MRC_CONNECT_DYNAMIC_MPR | MRC_CONNECT_TRIM_NACK | MRC_CONNECT_SERVICE_TIME,
        .offered = MRC_CONNECT_DYNAMIC_MPR | MRC_CONNECT_TRIM_NACK,
    };
}

static const uint8_t sample_bytes[MRC_CONNECT_RECORD_SIZE] = {
    0x4d, 0x52, 0x43, 0x51, 0x00, 0x01, 0x00, 0x40,
    0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x03,
    0x00, 0xab, 0xcd, 0xef, 0x00, 0x01, 0x23, 0x45,
    0x00, 0xfe, 0xdc, 0xba, 0xab, 0xcd, 0x40, 0x01,
    0x12, 0x34, 0x08, 0x00, 0x81, 0x23, 0x00, 0x00,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x89, 0xab, 0xcd, 0xef, 0x00, 0x00, 0x00, 0x00,
};

static void test_wire_format(void)
{
    struct mrc_connect_offer offer = sample_offer();
    uint8_t record[MRC_CONNECT_RECORD_SIZE];
    assert(mrc_connect_encode(record, sizeof(record), &offer) == 0);
    assert(memcmp(record, sample_bytes, sizeof(record)) == 0);

    struct mrc_connect_offer decoded = {0};
    assert(mrc_connect_decode(sample_bytes, sizeof(sample_bytes), &decoded) == 0);
    assert(decoded.receive.qpn == 0xabcdef);
    assert(decoded.receive.initial_psn == 0x12345);
    assert(decoded.receive.initial_msn == 0xfedcba);
    assert(decoded.receive.initial_rqmsn == 0xabcd);
    assert(decoded.receive.max_mpr == 64 && decoded.receive.max_wimm == 0x1234);
    assert(decoded.receive.memory.present && decoded.receive.memory.length > UINT32_MAX);
    assert(decoded.receive.memory.address == UINT64_C(0x0102030405060708));
    assert(decoded.receive.memory.r_key == UINT32_C(0x89abcdef));
    assert(decoded.p_key == 0x8123 && decoded.pmtu == 2048);
    assert(decoded.supported == 7 && decoded.offered == 3);
}

static void test_strict_decoder(void)
{
    struct mrc_connect_offer decoded = {0};
    uint8_t record[MRC_CONNECT_RECORD_SIZE + 1];
    memcpy(record, sample_bytes, sizeof(sample_bytes));
    assert(mrc_connect_decode(record, sizeof(record) - 2, &decoded) == -EMSGSIZE);
    assert(mrc_connect_decode(record, sizeof(record), &decoded) == -EMSGSIZE);

    static const struct {
        size_t offset;
        uint8_t value;
        int error;
    } tampering[] = {
        {0, 0, -EPROTO},              /* Wrong magic. */
        {5, 2, -EPROTONOSUPPORT},    /* Unknown version. */
        {7, 63, -EPROTO},             /* Wrong embedded length. */
        {11, 0x87, -EPROTO},          /* Unknown capability. */
        {11, 0x04, -EPROTO},          /* Offers unsupported capabilities. */
        {16, 1, -EPROTO},             /* QPN upper byte. */
        {20, 1, -EPROTO},             /* PSN upper byte. */
        {24, 1, -EPROTO},             /* MSN upper byte. */
        {30, 0, -EPROTO},             /* No receive MPR. */
        {31, 0, -EPROTO},             /* MR fields supplied without presence flag. */
        {31, 3, -EPROTO},             /* Reserved flag. */
        {35, 1, -EPROTO},             /* PMTU does not match a RoCE PMTU. */
        {38, 1, -EPROTO},             /* Reserved bytes. */
        {63, 1, -EPROTO},
    };
    for (size_t i = 0; i < sizeof(tampering) / sizeof(*tampering); i++) {
        memcpy(record, sample_bytes, sizeof(sample_bytes));
        record[tampering[i].offset] = tampering[i].value;
        assert(mrc_connect_decode(record, sizeof(sample_bytes), &decoded) == tampering[i].error);
        assert(decoded.receive.qpn == 0);
    }

    struct mrc_connect_offer offer = sample_offer();
    offer.receive.qpn = 2;
    assert(mrc_connect_encode(record, sizeof(sample_bytes), &offer) == -EINVAL);
    offer = sample_offer();
    offer.receive.memory.address = UINT64_MAX - 2;
    assert(mrc_connect_encode(record, sizeof(sample_bytes), &offer) == -EINVAL);
    offer = sample_offer();
    offer.p_key = 0x8000;
    assert(mrc_connect_encode(record, sizeof(sample_bytes), &offer) == -EINVAL);
}

static void test_directional_negotiation(void)
{
    struct mrc_connect_offer local = sample_offer();
    struct mrc_connect_offer peer = {
        .receive = {
            .qpn = 300, .initial_psn = 0xfffffe, .initial_msn = 1, .initial_rqmsn = 4,
            .max_mpr = 8, .max_wimm = 1,
        },
        .pmtu = 1024, .p_key = 0x0123,
        .supported = MRC_CONNECT_DYNAMIC_MPR | MRC_CONNECT_SERVICE_TIME,
        .offered = MRC_CONNECT_DYNAMIC_MPR | MRC_CONNECT_SERVICE_TIME,
    };
    struct mrc_connect_qp qp;
    assert(mrc_connect_negotiate(&local, &peer, &qp) == 0);
    assert(qp.send.qpn == 300 && qp.receive.qpn == 0xabcdef);
    assert(qp.send.initial_psn == 0xfffffe && qp.receive.initial_psn == 0x12345);
    assert(qp.send.initial_rqmsn == 4 && qp.receive.initial_rqmsn == 0xabcd);
    assert(qp.send.max_mpr == 8 && qp.receive.max_mpr == 64);
    assert(qp.send.max_wimm == 1 && qp.receive.max_wimm == 0x1234);
    assert(!qp.send.memory.present && qp.receive.memory.present);
    assert(qp.pmtu == 1024 && qp.local_p_key == 0x8123 && qp.peer_p_key == 0x0123);
    assert(qp.dynamic_mpr);
    assert(qp.local_responder_features == MRC_CONNECT_TRIM_NACK);
    assert(qp.peer_responder_features == MRC_CONNECT_SERVICE_TIME);
    assert(qp.local_supported & MRC_CONNECT_SERVICE_TIME);
    assert(!(qp.peer_supported & MRC_CONNECT_TRIM_NACK));

    struct mrc_connect_qp inverse;
    assert(mrc_connect_negotiate(&peer, &local, &inverse) == 0);
    assert(inverse.send.qpn == qp.receive.qpn && inverse.receive.qpn == qp.send.qpn);
    assert(inverse.peer_responder_features == qp.local_responder_features);
    assert(inverse.local_responder_features == qp.peer_responder_features);
    assert(inverse.dynamic_mpr && inverse.pmtu == qp.pmtu);

    peer.offered &= ~MRC_CONNECT_DYNAMIC_MPR;
    assert(mrc_connect_negotiate(&local, &peer, &qp) == 0);
    assert(!qp.dynamic_mpr && (qp.peer_supported & MRC_CONNECT_DYNAMIC_MPR));
    peer.supported &= ~MRC_CONNECT_DYNAMIC_MPR;
    assert(mrc_connect_negotiate(&local, &peer, &qp) == 0);
    assert(!qp.dynamic_mpr && !(qp.peer_supported & MRC_CONNECT_DYNAMIC_MPR));

    peer.p_key = 0x8124;
    assert(mrc_connect_negotiate(&local, &peer, &qp) == -EACCES);
    peer.p_key = 0x0123;
    local.p_key = 0x0123;
    assert(mrc_connect_negotiate(&local, &peer, &qp) == -EACCES);
}

static void test_capability_matrix(void)
{
    const unsigned int all = MRC_CONNECT_TRIM_NACK | MRC_CONNECT_DYNAMIC_MPR |
                             MRC_CONNECT_SERVICE_TIME;
    const unsigned int directional = MRC_CONNECT_TRIM_NACK | MRC_CONNECT_SERVICE_TIME;
    unsigned int cases = 0;
    for (unsigned int ls = 0; ls <= all; ls++)
        for (unsigned int lo = 0; lo <= all; lo++) {
            if (lo & ~ls)
                continue;
            for (unsigned int ps = 0; ps <= all; ps++)
                for (unsigned int po = 0; po <= all; po++) {
                    if (po & ~ps)
                        continue;
                    struct mrc_connect_offer local = sample_offer(), peer = sample_offer();
                    local.supported = ls;
                    local.offered = lo;
                    peer.supported = ps;
                    peer.offered = po;
                    peer.receive.qpn = 300;
                    peer.receive.max_mpr = 1;
                    peer.receive.max_wimm = 0;
                    peer.pmtu = 256;
                    struct mrc_connect_qp qp, inverse;
                    assert(mrc_connect_negotiate(&local, &peer, &qp) == 0);
                    assert(mrc_connect_negotiate(&peer, &local, &inverse) == 0);
                    bool dynamic = (lo & po & MRC_CONNECT_DYNAMIC_MPR) != 0;
                    assert(qp.dynamic_mpr == dynamic && inverse.dynamic_mpr == dynamic);
                    assert(qp.local_responder_features == (lo & directional));
                    assert(qp.peer_responder_features == (po & directional));
                    assert(inverse.local_responder_features == qp.peer_responder_features);
                    assert(inverse.peer_responder_features == qp.local_responder_features);
                    assert(qp.local_supported == ls && qp.peer_supported == ps);
                    assert(qp.send.max_mpr == 1 && qp.send.max_wimm == 0);
                    assert(qp.receive.max_mpr == local.receive.max_mpr);
                    assert(qp.receive.max_wimm == local.receive.max_wimm);
                    assert(qp.pmtu == 256 && inverse.pmtu == 256);
                    cases++;
                }
        }
    assert(cases == 729);
}

static void test_write_range(void)
{
    struct mrc_connect_receive peer = {
        .memory = { .present = true, .address = UINT64_MAX - 256, .length = 256, .r_key = 3 },
    };
    uint64_t address = 0;
    uint32_t r_key = 0;
    assert(mrc_connect_write_range(&peer, 128, 128, &address, &r_key));
    assert(address == UINT64_MAX - 128 && r_key == 3);
    assert(!mrc_connect_write_range(&peer, 128, 129, &address, &r_key));
    assert(address == UINT64_MAX - 128 && r_key == 3);
    assert(!mrc_connect_write_range(&peer, UINT64_MAX, 0, NULL, NULL));
    assert(mrc_connect_write_range(&peer, 256, 0, &address, &r_key));
    assert(address == UINT64_MAX);
    peer.memory.length++;
    assert(!mrc_connect_write_range(&peer, 0, 1, NULL, NULL));
    peer.memory = (struct mrc_connect_memory) {0};
    assert(!mrc_connect_write_range(&peer, 0, 0, NULL, NULL));
    peer.memory.present = true;
    assert(mrc_connect_write_range(&peer, 0, 0, &address, &r_key));
    assert(!address && !r_key);
}

int main(void)
{
    test_wire_format();
    test_strict_decoder();
    test_directional_negotiation();
    test_capability_matrix();
    test_write_range();
    puts("MRC out-of-band connection tests passed");
}
