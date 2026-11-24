#include "roce_cm_session.h"

#include <string.h>

static uint8_t timeout_code(uint64_t ns)
{
    uint8_t code = 0;
    for (uint64_t interval = 4096; code < 31 && interval < ns; interval <<= 1)
        ++code;
    return code;
}

static void ipv4_gid(uint8_t gid[ROCE_CM_GID_SIZE], uint32_t ipv4)
{
    memset(gid, 0, ROCE_CM_GID_SIZE);
    gid[10] = 0xff;
    gid[11] = 0xff;
    for (size_t i = 0; i < 4; ++i)
        gid[15 - i] = (uint8_t)(ipv4 >> (8 * i));
}

static bool address_matches(const struct roce_address *expected, const struct roce_address *received)
{
    return expected->ipv4 == received->ipv4 && !memcmp(expected->mac, received->mac, ROCE_MAC_LEN);
}

bool roce_cm_session_init(struct roce_cm_session *session,
                          const struct roce_cm_session_config *config)
{
    if (!session || !config)
        return false;

    size_t capacity = config->initiator ? ROCE_CMA_IPV4_REQ_USER_SIZE : ROCE_CM_REP_PRIVATE_SIZE;
    if (config->connection.local_qpn <= 1 || config->connection.local_qpn >= ROCE_PSN_MASK ||
        config->connection.starting_psn > ROCE_PSN_MASK ||
        config->connection.ece_vendor_id > 0xffffff || config->connection.rnr_retry_count > 7 ||
        !config->service_id || !config->udp_source || !config->retry_ns ||
        (config->initiator && !config->cma_source_port) ||
        config->mtu < 1 || config->mtu > 5 || config->max_retries > 15 ||
        config->private_length > capacity || (config->private_length && !config->private_data))
        return false;

    struct roce_cm_session initialized = {
        .phase = ROCE_CM_PENDING,
        .config = *config,
        .mtu = config->mtu,
        .tid = config->tid,
        .gsi_psn = config->connection.starting_psn,
        .send_pending = config->initiator,
    };
    initialized.config.private_data = NULL;
    if (config->private_length)
        memcpy(initialized.local_private_data, config->private_data, config->private_length);
    *session = initialized;
    return true;
}

static bool retry_due(const struct roce_cm_session *session, uint64_t now)
{
    return session->sends && now - session->last_sent >= session->config.retry_ns;
}

static bool failed(struct roce_cm_session *session, uint64_t now)
{
    if (session->phase == ROCE_CM_PENDING && !(session->config.initiator && session->received_peer) &&
        session->sends > session->config.max_retries && retry_due(session, now))
        session->phase = ROCE_CM_FAILED;
    return session->phase == ROCE_CM_FAILED;
}

static enum roce_cm_type next_message(struct roce_cm_session *session, uint64_t now)
{
    if (failed(session, now))
        return 0;
    if (session->config.initiator && session->received_peer)
        return session->send_pending ? ROCE_CM_RTU : 0;
    if (session->phase == ROCE_CM_ESTABLISHED ||
        (!session->config.initiator && !session->received_peer))
        return 0;

    if (session->sends > session->config.max_retries)
        return 0;
    if (!session->send_pending && !retry_due(session, now))
        return 0;
    return session->config.initiator ? ROCE_CM_REQ : ROCE_CM_REP;
}

static void prepare_req(struct roce_cm_message *message, const struct roce_cm_session_config *config,
                        const uint8_t *user_data, uint8_t private_data[ROCE_CM_REQ_PRIVATE_SIZE])
{
    uint8_t timeout = timeout_code(config->retry_ns);
    message->req.service_id = config->service_id;
    message->req.pkey = config->pkey;
    message->req.mtu = config->mtu;
    message->req.remote_response_timeout = timeout;
    message->req.local_response_timeout = timeout;
    message->req.retry_count = 7;
    message->req.max_cm_retries = config->max_retries;
    message->req.primary.hop_limit = 64;
    message->req.primary.ack_timeout = timeout;
    ipv4_gid(message->req.primary.local_gid, config->local.ipv4);
    ipv4_gid(message->req.primary.remote_gid, config->peer.ipv4);

    struct roce_cma_ipv4 cma = {
        .source_port = config->cma_source_port,
        .source_ipv4 = config->local.ipv4,
        .destination_ipv4 = config->peer.ipv4,
    };
    roce_cma_encode_ipv4(private_data, ROCE_CM_REQ_PRIVATE_SIZE, &cma);
    memcpy(private_data + ROCE_CMA_IPV4_SIZE, user_data, config->private_length);
    message->private_data = private_data;
    message->private_length = ROCE_CMA_IPV4_SIZE + config->private_length;
}

size_t roce_cm_session_prepare(struct roce_cm_session *session, uint64_t now,
                               uint8_t *frame, size_t capacity)
{
    if (!session)
        return 0;
    session->prepared = 0;
    enum roce_cm_type type = next_message(session, now);
    if (!type || !frame || capacity < ROCE_CM_FRAME_SIZE)
        return 0;

    const struct roce_cm_session_config *config = &session->config;
    struct roce_cm_message message = {
        .type = type,
        .tid = session->tid,
        .local_comm_id = config->local_comm_id,
        .remote_comm_id = session->peer_comm_id,
        .connection = config->connection,
    };
    uint8_t private_data[ROCE_CM_REQ_PRIVATE_SIZE];
    if (type == ROCE_CM_REQ) {
        prepare_req(&message, config, session->local_private_data, private_data);
    } else if (type == ROCE_CM_REP) {
        message.rep.target_ack_delay = timeout_code(config->retry_ns);
        message.rep.failover_accepted = 1;
        message.private_data = session->local_private_data;
        message.private_length = config->private_length;
    }

    uint8_t mad[ROCE_CM_MAD_SIZE];
    if (!roce_cm_encode(mad, sizeof(mad), &message))
        return 0;
    const struct roce_cm_frame cm = {
        .src = config->local,
        .dst = config->peer,
        .udp_source = config->udp_source,
        .pkey = config->pkey,
        .psn = session->gsi_psn,
        .mad = mad,
    };
    size_t length = roce_cm_frame_encode(frame, capacity, &cm);
    if (length)
        session->prepared = type;
    return length;
}

void roce_cm_session_sent(struct roce_cm_session *session, uint64_t now)
{
    if (!session || !session->prepared)
        return;

    if (session->prepared == ROCE_CM_RTU) {
        session->phase = ROCE_CM_ESTABLISHED;
    } else {
        ++session->sends;
        session->last_sent = now;
    }
    session->gsi_psn = (session->gsi_psn + 1) & ROCE_PSN_MASK;
    session->send_pending = false;
    session->prepared = 0;
}

static bool peer_matches(const struct roce_cm_session *session, const struct roce_cm_message *message,
                         const uint8_t *private_data, size_t private_length)
{
    return session->tid == message->tid && session->peer_comm_id == message->local_comm_id &&
           session->peer_qpn == message->connection.local_qpn &&
           session->peer_psn == message->connection.starting_psn &&
           session->peer_private_length == private_length &&
           !memcmp(session->peer_private_data, private_data, private_length);
}

static void cache_peer(struct roce_cm_session *session, const struct roce_cm_message *message,
                       const uint8_t *private_data, size_t private_length)
{
    session->tid = message->tid;
    session->peer_comm_id = message->local_comm_id;
    session->peer_qpn = message->connection.local_qpn;
    session->peer_psn = message->connection.starting_psn;
    memcpy(session->peer_private_data, private_data, private_length);
    session->peer_private_length = private_length;
    session->received_peer = true;
}

static bool receive_req(struct roce_cm_session *session, const struct roce_cm_message *message)
{
    struct roce_cm_session_config *config = &session->config;
    struct roce_cma_ipv4 cma;
    uint8_t source_gid[ROCE_CM_GID_SIZE];
    uint8_t destination_gid[ROCE_CM_GID_SIZE];
    ipv4_gid(source_gid, config->peer.ipv4);
    ipv4_gid(destination_gid, config->local.ipv4);

    if (message->req.service_id != config->service_id || message->req.pkey != config->pkey ||
        message->req.mtu > config->mtu ||
        memcmp(source_gid, message->req.primary.local_gid, sizeof(source_gid)) ||
        memcmp(destination_gid, message->req.primary.remote_gid, sizeof(destination_gid)) ||
        !roce_cma_decode_ipv4(&cma, message->private_data, message->private_length) ||
        cma.source_ipv4 != config->peer.ipv4 || cma.destination_ipv4 != config->local.ipv4 ||
        !cma.source_port)
        return false;

    const uint8_t *private_data = message->private_data + ROCE_CMA_IPV4_SIZE;
    size_t private_length = message->private_length - ROCE_CMA_IPV4_SIZE;
    if (session->received_peer) {
        if (session->mtu != message->req.mtu || session->config.cma_source_port != cma.source_port ||
            !peer_matches(session, message, private_data, private_length))
            return false;
    } else {
        cache_peer(session, message, private_data, private_length);
        session->mtu = message->req.mtu;
        config->cma_source_port = cma.source_port;
        if (config->max_retries > message->req.max_cm_retries)
            config->max_retries = message->req.max_cm_retries;
        if (config->connection.responder_resources > message->connection.initiator_depth)
            config->connection.responder_resources = message->connection.initiator_depth;
        if (config->connection.initiator_depth > message->connection.responder_resources)
            config->connection.initiator_depth = message->connection.responder_resources;
    }
    if (session->phase == ROCE_CM_PENDING)
        session->send_pending = true;
    return true;
}

static bool receive_rep(struct roce_cm_session *session, const struct roce_cm_message *message)
{
    if (!session->sends || message->tid != session->tid ||
        message->remote_comm_id != session->config.local_comm_id)
        return false;

    if (session->received_peer) {
        if (!peer_matches(session, message, message->private_data, message->private_length))
            return false;
    } else {
        cache_peer(session, message, message->private_data, message->private_length);
    }
    session->send_pending = true;
    return true;
}

bool roce_cm_session_receive(struct roce_cm_session *session, uint64_t now,
                             const uint8_t *frame, size_t length)
{
    struct roce_cm_frame cm;
    struct roce_cm_message message;
    if (!session || failed(session, now) || !roce_cm_frame_decode(&cm, frame, length) ||
        cm.pkey != session->config.pkey || !address_matches(&session->config.local, &cm.dst) ||
        !address_matches(&session->config.peer, &cm.src) ||
        !roce_cm_decode(&message, cm.mad, ROCE_CM_MAD_SIZE))
        return false;

    if (session->config.initiator)
        return message.type == ROCE_CM_REP && receive_rep(session, &message);
    if (message.type == ROCE_CM_REQ)
        return receive_req(session, &message);
    if (message.type != ROCE_CM_RTU || !session->received_peer || !session->sends ||
        message.tid != session->tid || message.local_comm_id != session->peer_comm_id ||
        message.remote_comm_id != session->config.local_comm_id)
        return false;

    session->phase = ROCE_CM_ESTABLISHED;
    session->send_pending = false;
    return true;
}
