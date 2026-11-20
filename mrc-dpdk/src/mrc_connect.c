#include "mrc_connect.h"
#include "mrc_qp.h"
#include "mrc_wire.h"

#include <errno.h>
#include <string.h>

enum {
    RECORD_VERSION = 4,
    RECORD_LENGTH = 6,
    SUPPORTED = 8,
    OFFERED = 12,
    RECEIVE_QPN = 16,
    RECEIVE_PSN = 20,
    RECEIVE_MSN = 24,
    RECEIVE_RQMSN = 28,
    RECEIVE_MAX_MPR = 30,
    FLAGS = 31,
    RECEIVE_MAX_WIMM = 32,
    PATH_MTU = 34,
    P_KEY = 36,
    RESERVED_PARTITION = 38,
    MR_ADDRESS = 40,
    MR_LENGTH = 48,
    MR_KEY = 56,
    RESERVED_TRAILER = 60,
    MR_PRESENT = 1u << 0,
    RESPONDER_CAPS = MRC_CONNECT_TRIM_NACK | MRC_CONNECT_SERVICE_TIME,
    ALL_CAPS = RESPONDER_CAPS | MRC_CONNECT_DYNAMIC_MPR,
};

static const uint8_t magic[] = { 'M', 'R', 'C', 'Q' };

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)read_be16(p) << 16) | read_be16(p + 2);
}

static uint64_t read_be64(const uint8_t *p)
{
    return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
}

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void write_be32(uint8_t *p, uint32_t value)
{
    write_be16(p, (uint16_t)(value >> 16));
    write_be16(p + 2, (uint16_t)value);
}

static void write_be64(uint8_t *p, uint64_t value)
{
    write_be32(p, (uint32_t)(value >> 32));
    write_be32(p + 4, (uint32_t)value);
}

static bool valid_memory(const struct mrc_connect_memory *memory)
{
    if (!memory->present)
        return !memory->address && !memory->length && !memory->r_key;
    return memory->length <= UINT64_MAX - memory->address;
}

static bool valid_offer(const struct mrc_connect_offer *offer)
{
    /* QP 0/1 are reserved by IB, 2 is the MRC endpoint QP, and
     * 0xffffff is the IB multicast QPN. */
    return offer && offer->receive.qpn > 2 && offer->receive.qpn < MRC_PSN_MASK &&
           offer->receive.initial_psn <= MRC_PSN_MASK &&
           offer->receive.initial_msn <= MRC_PSN_MASK && offer->receive.max_mpr &&
           offer->pmtu >= 256 && offer->pmtu <= 4096 &&
           !(offer->pmtu & (offer->pmtu - 1)) && (offer->p_key & MRC_PKEY_PARTITION_MASK) &&
           !(offer->supported & ~(uint32_t)ALL_CAPS) && !(offer->offered & ~offer->supported) &&
           valid_memory(&offer->receive.memory);
}

int mrc_connect_encode(uint8_t *bytes, size_t length, const struct mrc_connect_offer *offer)
{
    if (length != MRC_CONNECT_RECORD_SIZE)
        return -EMSGSIZE;
    if (!bytes || !valid_offer(offer))
        return -EINVAL;

    memset(bytes, 0, length);
    memcpy(bytes, magic, sizeof(magic));
    write_be16(bytes + RECORD_VERSION, MRC_CONNECT_RECORD_VERSION);
    write_be16(bytes + RECORD_LENGTH, MRC_CONNECT_RECORD_SIZE);
    write_be32(bytes + SUPPORTED, offer->supported);
    write_be32(bytes + OFFERED, offer->offered);
    write_be32(bytes + RECEIVE_QPN, offer->receive.qpn);
    write_be32(bytes + RECEIVE_PSN, offer->receive.initial_psn);
    write_be32(bytes + RECEIVE_MSN, offer->receive.initial_msn);
    write_be16(bytes + RECEIVE_RQMSN, offer->receive.initial_rqmsn);
    bytes[RECEIVE_MAX_MPR] = offer->receive.max_mpr;
    bytes[FLAGS] = offer->receive.memory.present ? MR_PRESENT : 0;
    write_be16(bytes + RECEIVE_MAX_WIMM, offer->receive.max_wimm);
    write_be16(bytes + PATH_MTU, offer->pmtu);
    write_be16(bytes + P_KEY, offer->p_key);
    write_be64(bytes + MR_ADDRESS, offer->receive.memory.address);
    write_be64(bytes + MR_LENGTH, offer->receive.memory.length);
    write_be32(bytes + MR_KEY, offer->receive.memory.r_key);
    return 0;
}

int mrc_connect_decode(const uint8_t *bytes, size_t length, struct mrc_connect_offer *offer)
{
    if (length != MRC_CONNECT_RECORD_SIZE)
        return -EMSGSIZE;
    if (!bytes || !offer)
        return -EINVAL;
    if (memcmp(bytes, magic, sizeof(magic)) || read_be16(bytes + RECORD_LENGTH) != length)
        return -EPROTO;
    if (read_be16(bytes + RECORD_VERSION) != MRC_CONNECT_RECORD_VERSION)
        return -EPROTONOSUPPORT;
    if ((bytes[FLAGS] & ~MR_PRESENT) || read_be16(bytes + RESERVED_PARTITION) ||
        read_be32(bytes + RESERVED_TRAILER))
        return -EPROTO;

    struct mrc_connect_offer decoded = {
        .receive = {
            .qpn = read_be32(bytes + RECEIVE_QPN),
            .initial_psn = read_be32(bytes + RECEIVE_PSN),
            .initial_msn = read_be32(bytes + RECEIVE_MSN),
            .initial_rqmsn = read_be16(bytes + RECEIVE_RQMSN),
            .max_mpr = bytes[RECEIVE_MAX_MPR],
            .max_wimm = read_be16(bytes + RECEIVE_MAX_WIMM),
            .memory = {
                .address = read_be64(bytes + MR_ADDRESS),
                .length = read_be64(bytes + MR_LENGTH),
                .r_key = read_be32(bytes + MR_KEY),
                .present = (bytes[FLAGS] & MR_PRESENT) != 0,
            },
        },
        .pmtu = read_be16(bytes + PATH_MTU),
        .p_key = read_be16(bytes + P_KEY),
        .supported = read_be32(bytes + SUPPORTED),
        .offered = read_be32(bytes + OFFERED),
    };
    if (!valid_offer(&decoded))
        return -EPROTO;
    *offer = decoded;
    return 0;
}

int mrc_connect_negotiate(const struct mrc_connect_offer *local,
                          const struct mrc_connect_offer *peer, struct mrc_connect_qp *qp)
{
    if (!qp || !valid_offer(local) || !valid_offer(peer))
        return -EINVAL;

    if (!mrc_wire_pkey_compatible(local->p_key, peer->p_key))
        return -EACCES;

    *qp = (struct mrc_connect_qp) {
        .send = peer->receive,
        .receive = local->receive,
        .pmtu = local->pmtu < peer->pmtu ? local->pmtu : peer->pmtu,
        .local_p_key = local->p_key,
        .peer_p_key = peer->p_key,
        .local_supported = local->supported,
        .peer_supported = peer->supported,
        .local_responder_features = local->offered & RESPONDER_CAPS,
        .peer_responder_features = peer->offered & RESPONDER_CAPS,
        .dynamic_mpr = (local->offered & peer->offered & MRC_CONNECT_DYNAMIC_MPR) != 0,
    };
    return 0;
}

bool mrc_connect_write_range(const struct mrc_connect_receive *destination,
                             uint64_t offset, uint64_t length, uint64_t *address,
                             uint32_t *r_key)
{
    if (!destination)
        return false;

    const struct mrc_connect_memory *memory = &destination->memory;
    if (!memory->present || !valid_memory(memory) || offset > memory->length ||
        length > memory->length - offset)
        return false;
    if (address)
        *address = memory->address + offset;
    if (r_key)
        *r_key = memory->r_key;
    return true;
}
