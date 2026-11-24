#include "roce_cm_wire.h"

#include <string.h>

/* IBTA Volume 1, sections 12.6.5–12.6.11. Body offsets
 * start after the common MAD header, as in Linux ibta_vol1_c12.h. */
enum {
    MAD_HEADER_SIZE = 24,
    MAD_BASE_VERSION = 1,
    MAD_CLASS_CM = 7,
    MAD_CM_VERSION = 2,
    MAD_METHOD_SEND = 3,
    REQ_PRIVATE_OFFSET = 140,
    MRA_PRIVATE_OFFSET = 10,
    REJ_PRIVATE_OFFSET = 84,
    REJ_MAX_ARI_LENGTH = 72,
    REP_PRIVATE_OFFSET = 36,
    RTU_PRIVATE_OFFSET = 8,
    DREQ_QPN_OFFSET = 8,
    DREQ_PRIVATE_OFFSET = 12,
    REQ_TRANSPORT_TYPE_MASK = 0x06,
    REQ_RDC_EXISTS = 0x08,
    QPN_MASK = 0xffffff,
};

static uint64_t read_be(const uint8_t *bytes, size_t width)
{
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i)
        value = value << 8 | bytes[i];
    return value;
}

static void write_be(uint8_t *bytes, size_t width, uint64_t value)
{
    while (width) {
        bytes[--width] = (uint8_t)value;
        value >>= 8;
    }
}

static bool valid_connection(const struct roce_cm_connection *connection)
{
    return connection->local_qpn > 1 && connection->local_qpn < QPN_MASK &&
           connection->starting_psn <= QPN_MASK &&
           connection->ece_vendor_id <= 0xffffff && connection->rnr_retry_count <= 7;
}

static bool valid_req(const struct roce_cm_message *message)
{
    const struct roce_cm_path *path = &message->req.primary;

    return message->req.mtu >= 1 && message->req.mtu <= 5 &&
           message->req.remote_response_timeout <= 31 &&
           message->req.local_response_timeout <= 31 && message->req.retry_count <= 7 &&
           message->req.max_cm_retries <= 15 && path->flow_label <= 0xfffff &&
           path->packet_rate <= 63 && path->service_level <= 15 && path->ack_timeout <= 31;
}

static bool valid_rep(const struct roce_cm_message *message)
{
    return message->rep.target_ack_delay <= 31 && message->rep.failover_accepted <= 2;
}

static void encode_req(uint8_t *body, const struct roce_cm_message *message)
{
    const struct roce_cm_connection *connection = &message->connection;
    const struct roce_cm_path *path = &message->req.primary;

    write_be(body + 5, 3, connection->ece_vendor_id);
    write_be(body + 8, 8, message->req.service_id);
    write_be(body + 16, 8, connection->local_ca_guid);
    write_be(body + 32, 3, connection->local_qpn);
    body[35] = connection->responder_resources;
    body[39] = connection->initiator_depth;
    body[43] = (uint8_t)(message->req.remote_response_timeout << 3 | connection->flow_control);
    write_be(body + 44, 3, connection->starting_psn);
    body[47] = (uint8_t)(message->req.local_response_timeout << 3 | message->req.retry_count);
    write_be(body + 48, 2, message->req.pkey);
    body[50] = (uint8_t)(message->req.mtu << 4 | connection->rnr_retry_count);
    body[51] = (uint8_t)(message->req.max_cm_retries << 4 | connection->srq << 3);
    write_be(body + 52, 2, path->local_lid);
    write_be(body + 54, 2, path->remote_lid);
    memcpy(body + 56, path->local_gid, ROCE_CM_GID_SIZE);
    memcpy(body + 72, path->remote_gid, ROCE_CM_GID_SIZE);
    write_be(body + 88, 3, (uint64_t)path->flow_label << 4);
    body[91] = path->packet_rate;
    body[92] = path->traffic_class;
    body[93] = path->hop_limit;
    body[94] = (uint8_t)(path->service_level << 4 | path->subnet_local << 3);
    body[95] = (uint8_t)(path->ack_timeout << 3);
}

static void decode_req(struct roce_cm_message *message, const uint8_t *body)
{
    struct roce_cm_connection *connection = &message->connection;
    struct roce_cm_path *path = &message->req.primary;

    connection->ece_vendor_id = (uint32_t)read_be(body + 5, 3);
    message->req.service_id = read_be(body + 8, 8);
    connection->local_ca_guid = read_be(body + 16, 8);
    connection->local_qpn = (uint32_t)read_be(body + 32, 3);
    connection->responder_resources = body[35];
    connection->initiator_depth = body[39];
    message->req.remote_response_timeout = body[43] >> 3;
    connection->flow_control = body[43] & 1;
    connection->starting_psn = (uint32_t)read_be(body + 44, 3);
    message->req.local_response_timeout = body[47] >> 3;
    message->req.retry_count = body[47] & 7;
    message->req.pkey = (uint16_t)read_be(body + 48, 2);
    message->req.mtu = body[50] >> 4;
    connection->rnr_retry_count = body[50] & 7;
    message->req.max_cm_retries = body[51] >> 4;
    connection->srq = (body[51] & 8) != 0;
    path->local_lid = (uint16_t)read_be(body + 52, 2);
    path->remote_lid = (uint16_t)read_be(body + 54, 2);
    memcpy(path->local_gid, body + 56, ROCE_CM_GID_SIZE);
    memcpy(path->remote_gid, body + 72, ROCE_CM_GID_SIZE);
    path->flow_label = (uint32_t)(read_be(body + 88, 3) >> 4);
    path->packet_rate = body[91] & 0x3f;
    path->traffic_class = body[92];
    path->hop_limit = body[93];
    path->service_level = body[94] >> 4;
    path->subnet_local = (body[94] & 8) != 0;
    path->ack_timeout = body[95] >> 3;
}

static void encode_rep(uint8_t *body, const struct roce_cm_message *message)
{
    const struct roce_cm_connection *connection = &message->connection;

    write_be(body + 12, 3, connection->local_qpn);
    body[15] = (uint8_t)(connection->ece_vendor_id >> 16);
    body[19] = (uint8_t)(connection->ece_vendor_id >> 8);
    write_be(body + 20, 3, connection->starting_psn);
    body[23] = (uint8_t)connection->ece_vendor_id;
    body[24] = connection->responder_resources;
    body[25] = connection->initiator_depth;
    body[26] = (uint8_t)(message->rep.target_ack_delay << 3 |
                         message->rep.failover_accepted << 1 | connection->flow_control);
    body[27] = (uint8_t)(connection->rnr_retry_count << 5 | connection->srq << 4);
    write_be(body + 28, 8, connection->local_ca_guid);
}

static void decode_rep(struct roce_cm_message *message, const uint8_t *body)
{
    struct roce_cm_connection *connection = &message->connection;

    connection->local_qpn = (uint32_t)read_be(body + 12, 3);
    connection->ece_vendor_id = (uint32_t)body[15] << 16 | (uint32_t)body[19] << 8 | body[23];
    connection->starting_psn = (uint32_t)read_be(body + 20, 3);
    connection->responder_resources = body[24];
    connection->initiator_depth = body[25];
    message->rep.target_ack_delay = body[26] >> 3;
    message->rep.failover_accepted = body[26] >> 1 & 3;
    connection->flow_control = body[26] & 1;
    connection->rnr_retry_count = body[27] >> 5;
    connection->srq = (body[27] & 0x10) != 0;
    connection->local_ca_guid = read_be(body + 28, 8);
}

static size_t private_offset(enum roce_cm_type type)
{
    switch (type) {
    case ROCE_CM_REQ:
        return MAD_HEADER_SIZE + REQ_PRIVATE_OFFSET;
    case ROCE_CM_MRA:
        return MAD_HEADER_SIZE + MRA_PRIVATE_OFFSET;
    case ROCE_CM_REJ:
        return MAD_HEADER_SIZE + REJ_PRIVATE_OFFSET;
    case ROCE_CM_REP:
        return MAD_HEADER_SIZE + REP_PRIVATE_OFFSET;
    case ROCE_CM_RTU:
    case ROCE_CM_DREP:
        return MAD_HEADER_SIZE + RTU_PRIVATE_OFFSET;
    case ROCE_CM_DREQ:
        return MAD_HEADER_SIZE + DREQ_PRIVATE_OFFSET;
    }
    return 0;
}

static bool valid_fields(const struct roce_cm_message *message)
{
    switch (message->type) {
    case ROCE_CM_REQ:
        return valid_connection(&message->connection) && valid_req(message);
    case ROCE_CM_MRA:
        return (unsigned)message->mra.message <= ROCE_CM_MRA_LAP && message->mra.service_timeout <= 31;
    case ROCE_CM_REJ:
        return (unsigned)message->rej.message <= ROCE_CM_REJ_OTHER && message->rej.reason &&
               message->remote_comm_id;
    case ROCE_CM_REP:
        return valid_connection(&message->connection) && valid_rep(message);
    case ROCE_CM_RTU:
    case ROCE_CM_DREP:
        return true;
    case ROCE_CM_DREQ:
        return message->dreq.remote_qpn > 1 && message->dreq.remote_qpn < QPN_MASK;
    }
    return false;
}

size_t roce_cm_encode(uint8_t *mad, size_t capacity, const struct roce_cm_message *message)
{
    if (!mad || capacity < ROCE_CM_MAD_SIZE || !message)
        return 0;

    size_t offset = private_offset(message->type);
    if (!offset || message->private_length > ROCE_CM_MAD_SIZE - offset ||
        (message->private_length && !message->private_data) || !valid_fields(message))
        return 0;

    if (message->private_length)
        memmove(mad + offset, message->private_data, message->private_length);
    memset(mad, 0, offset);
    memset(mad + offset + message->private_length, 0,
           ROCE_CM_MAD_SIZE - offset - message->private_length);
    mad[0] = MAD_BASE_VERSION;
    mad[1] = MAD_CLASS_CM;
    mad[2] = MAD_CM_VERSION;
    mad[3] = MAD_METHOD_SEND;
    write_be(mad + 8, 8, message->tid);
    write_be(mad + 16, 2, message->type);

    uint8_t *body = mad + MAD_HEADER_SIZE;
    write_be(body, 4, message->local_comm_id);
    if (message->type != ROCE_CM_REQ)
        write_be(body + 4, 4, message->remote_comm_id);
    if (message->type == ROCE_CM_REQ || message->type == ROCE_CM_REP)
        write_be(mad + 20, 4, message->connection.ece_attr_mod);
    if (message->type == ROCE_CM_MRA) {
        body[8] = (uint8_t)(message->mra.message << 6);
        body[9] = (uint8_t)(message->mra.service_timeout << 3);
    } else if (message->type == ROCE_CM_REJ) {
        body[8] = (uint8_t)(message->rej.message << 6);
        write_be(body + 10, 2, message->rej.reason);
    } else if (message->type == ROCE_CM_REQ) {
        encode_req(body, message);
    } else if (message->type == ROCE_CM_REP) {
        encode_rep(body, message);
    } else if (message->type == ROCE_CM_DREQ) {
        write_be(body + DREQ_QPN_OFFSET, 3, message->dreq.remote_qpn);
    }
    return ROCE_CM_MAD_SIZE;
}

bool roce_cm_decode(struct roce_cm_message *message, const uint8_t *mad, size_t length)
{
    if (!message || !mad || length != ROCE_CM_MAD_SIZE ||
        mad[0] != MAD_BASE_VERSION || mad[1] != MAD_CLASS_CM || mad[2] != MAD_CM_VERSION ||
        mad[3] != MAD_METHOD_SEND || read_be(mad + 4, 4) != 0)
        return false;

    struct roce_cm_message decoded = {
        .type = (enum roce_cm_type)read_be(mad + 16, 2),
        .tid = read_be(mad + 8, 8),
    };
    size_t offset = private_offset(decoded.type);
    if (!offset)
        return false;

    const uint8_t *body = mad + MAD_HEADER_SIZE;
    decoded.local_comm_id = (uint32_t)read_be(body, 4);
    if (decoded.type != ROCE_CM_REQ)
        decoded.remote_comm_id = (uint32_t)read_be(body + 4, 4);
    if (decoded.type == ROCE_CM_REQ || decoded.type == ROCE_CM_REP)
        decoded.connection.ece_attr_mod = (uint32_t)read_be(mad + 20, 4);
    if (decoded.type == ROCE_CM_REQ) {
        if ((body[43] & REQ_TRANSPORT_TYPE_MASK) || (body[50] & REQ_RDC_EXISTS))
            return false;
        decode_req(&decoded, body);
    } else if (decoded.type == ROCE_CM_REP) {
        decode_rep(&decoded, body);
    } else if (decoded.type == ROCE_CM_MRA) {
        decoded.mra.message = (enum roce_cm_mra_message)(body[8] >> 6);
        decoded.mra.service_timeout = body[9] >> 3;
    } else if (decoded.type == ROCE_CM_REJ) {
        if ((body[9] >> 1) > REJ_MAX_ARI_LENGTH)
            return false;
        decoded.rej.message = (enum roce_cm_rej_message)(body[8] >> 6);
        decoded.rej.reason = (uint16_t)read_be(body + 10, 2);
    } else if (decoded.type == ROCE_CM_DREQ) {
        decoded.dreq.remote_qpn = (uint32_t)read_be(body + DREQ_QPN_OFFSET, 3);
    }
    if (!valid_fields(&decoded))
        return false;

    decoded.private_data = mad + offset;
    decoded.private_length = ROCE_CM_MAD_SIZE - offset;
    *message = decoded;
    return true;
}

/* Linux cma.c uses a version byte, IP version nibble, source port and
 * two 16-byte address slots with each IPv4 address in the last four bytes. */
bool roce_cma_encode_ipv4(uint8_t *data, size_t capacity, const struct roce_cma_ipv4 *addresses)
{
    if (!data || capacity < ROCE_CMA_IPV4_SIZE || !addresses)
        return false;

    memset(data, 0, ROCE_CMA_IPV4_SIZE);
    data[1] = 4 << 4;
    write_be(data + 2, 2, addresses->source_port);
    write_be(data + 16, 4, addresses->source_ipv4);
    write_be(data + 32, 4, addresses->destination_ipv4);
    return true;
}

bool roce_cma_decode_ipv4(struct roce_cma_ipv4 *addresses, const uint8_t *data, size_t length)
{
    if (!addresses || !data || length < ROCE_CMA_IPV4_SIZE || data[0] || data[1] >> 4 != 4)
        return false;

    *addresses = (struct roce_cma_ipv4) {
        .source_port = (uint16_t)read_be(data + 2, 2),
        .source_ipv4 = (uint32_t)read_be(data + 16, 4),
        .destination_ipv4 = (uint32_t)read_be(data + 32, 4),
    };
    return true;
}
