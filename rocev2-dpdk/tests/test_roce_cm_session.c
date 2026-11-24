#include "roce_cm_session.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { RETRY_NS = 1000000 };

struct test_pair {
    struct roce_cm_session active;
    struct roce_cm_session passive;
    uint8_t req[ROCE_CM_FRAME_SIZE];
    uint8_t rep[ROCE_CM_FRAME_SIZE];
    uint8_t rtu[ROCE_CM_FRAME_SIZE];
};

static void test_configs(struct roce_cm_session_config *active,
                         struct roce_cm_session_config *passive)
{
    static const uint8_t request[] = {0x14, 0, 0x27};
    static const uint8_t reply[] = {0xa2, 0, 0x81};
    *active = (struct roce_cm_session_config) {
        .initiator = true,
        .tid = UINT64_C(0x0102030405060708),
        .local_comm_id = 0x12345678,
        .local = {.mac = {2, 0, 0, 0, 0, 1}, .ipv4 = 0xc0000201},
        .peer = {.mac = {2, 0, 0, 0, 0, 2}, .ipv4 = 0xc0000202},
        .connection = {.local_ca_guid = 2, .local_qpn = 0x80401, .starting_psn = 0xffffff},
        .service_id = ROCE_CMA_TCP_SERVICE_BASE | 18515,
        .cma_source_port = 30001,
        .udp_source = 0xc001,
        .pkey = ROCE_PKEY,
        .mtu = 3,
        .private_data = request,
        .private_length = sizeof(request),
        .retry_ns = RETRY_NS,
        .max_retries = 4,
    };
    *passive = *active;
    passive->initiator = false;
    passive->tid = 0;
    passive->local_comm_id = 0x96325874;
    passive->local = active->peer;
    passive->peer = active->local;
    passive->connection = (struct roce_cm_connection) {
        .local_ca_guid = 1, .local_qpn = 0x80402, .starting_psn = 0x123456,
    };
    passive->udp_source = 0xc002;
    passive->cma_source_port = 0;
    passive->private_data = reply;
    passive->private_length = sizeof(reply);
    passive->mtu = 4;
}

static void init_pair(struct test_pair *pair)
{
    struct roce_cm_session_config active;
    struct roce_cm_session_config passive;
    test_configs(&active, &passive);
    assert(roce_cm_session_init(&pair->active, &active));
    assert(roce_cm_session_init(&pair->passive, &passive));
}

static void send_at(struct roce_cm_session *session, uint64_t now, uint8_t *frame)
{
    assert(roce_cm_session_prepare(session, now, frame, ROCE_CM_FRAME_SIZE) == ROCE_CM_FRAME_SIZE);
    roce_cm_session_sent(session, now);
}

static struct roce_cm_message unpack(const uint8_t *frame, struct roce_cm_frame *outer)
{
    struct roce_cm_message message;
    assert(roce_cm_frame_decode(outer, frame, ROCE_CM_FRAME_SIZE));
    assert(roce_cm_decode(&message, outer->mad, ROCE_CM_MAD_SIZE));
    return message;
}

static void pack(uint8_t *frame, struct roce_cm_frame outer, const struct roce_cm_message *message)
{
    uint8_t mad[ROCE_CM_MAD_SIZE];
    assert(roce_cm_encode(mad, sizeof(mad), message) == sizeof(mad));
    outer.mad = mad;
    assert(roce_cm_frame_encode(frame, ROCE_CM_FRAME_SIZE, &outer) == ROCE_CM_FRAME_SIZE);
}

static bool receive_at(struct roce_cm_session *session, uint64_t now, const uint8_t *frame)
{
    return roce_cm_session_receive(session, now, frame, ROCE_CM_FRAME_SIZE);
}

static void check_loss(void)
{
    struct test_pair pair;
    struct roce_cm_frame outer;
    struct roce_cm_message request;
    struct roce_cma_ipv4 cma;
    uint8_t queued[ROCE_CM_FRAME_SIZE];
    init_pair(&pair);

    assert(!roce_cm_session_prepare(&pair.passive, 0, pair.rep, sizeof(pair.rep)));
    assert(!roce_cm_session_prepare(&pair.active, 0, pair.req, sizeof(pair.req) - 1));
    assert(roce_cm_session_prepare(&pair.active, 0, queued, sizeof(queued)) == sizeof(queued));
    assert(roce_cm_session_prepare(&pair.active, RETRY_NS / 2, pair.req, sizeof(pair.req)) == sizeof(pair.req));
    assert(!memcmp(queued, pair.req, sizeof(queued)));
    roce_cm_session_sent(&pair.active, RETRY_NS / 2);
    assert(!roce_cm_session_prepare(&pair.active, RETRY_NS, pair.req, sizeof(pair.req)));

    /* The first REQ and first REP were accepted by TX and dropped on the link. */
    uint64_t now = RETRY_NS + RETRY_NS / 2;
    send_at(&pair.active, now, pair.req);
    request = unpack(pair.req, &outer);
    assert(outer.psn == 0 && request.type == ROCE_CM_REQ && request.req.mtu == 3);
    assert(!request.req.primary.local_lid && !request.req.primary.remote_lid);
    assert(roce_cma_decode_ipv4(&cma, request.private_data, request.private_length));
    assert(cma.source_port == 30001 && cma.source_ipv4 == 0xc0000201 && cma.destination_ipv4 == 0xc0000202);
    assert(receive_at(&pair.passive, now, pair.req));
    assert(pair.passive.peer_qpn == 0x80401 && pair.passive.peer_psn == 0xffffff);
    assert(pair.passive.mtu == 3 && pair.passive.peer_private_length == 56);
    assert(!memcmp(pair.passive.peer_private_data, "\x14\0\x27", 3));
    assert(!pair.passive.peer_private_data[3] && !pair.passive.peer_private_data[55]);
    send_at(&pair.passive, now, pair.rep);

    now += RETRY_NS;
    send_at(&pair.active, now, pair.req);
    assert(receive_at(&pair.passive, now, pair.req));
    send_at(&pair.passive, now, pair.rep);
    assert(receive_at(&pair.active, now, pair.rep));
    assert(pair.active.peer_qpn == 0x80402 && pair.active.peer_psn == 0x123456);
    assert(pair.active.mtu == 3 && pair.active.peer_private_length == 196);
    assert(!memcmp(pair.active.peer_private_data, "\xa2\0\x81", 3));
    assert(pair.active.phase == ROCE_CM_PENDING && pair.passive.phase == ROCE_CM_PENDING);

    /* The active QP gets ready only after TX accepts RTU. Drop that RTU. */
    assert(roce_cm_session_prepare(&pair.active, now, pair.rtu, sizeof(pair.rtu)) == sizeof(pair.rtu));
    assert(pair.active.phase == ROCE_CM_PENDING);
    assert(!roce_cm_session_prepare(&pair.passive, now, pair.rep, sizeof(pair.rep)));
    roce_cm_session_sent(&pair.active, now);
    assert(pair.active.phase == ROCE_CM_ESTABLISHED && pair.passive.phase == ROCE_CM_PENDING);

    now += RETRY_NS;
    send_at(&pair.passive, now, pair.rep);
    assert(receive_at(&pair.active, now, pair.rep));
    send_at(&pair.active, now, pair.rtu);
    assert(receive_at(&pair.passive, now, pair.rtu));
    assert(pair.passive.phase == ROCE_CM_ESTABLISHED);
    assert(receive_at(&pair.passive, now, pair.rtu));
    assert(!roce_cm_session_prepare(&pair.passive, now, pair.rep, sizeof(pair.rep)));
    assert(!roce_cm_session_prepare(&pair.active, now, pair.rtu, sizeof(pair.rtu)));
}

static void check_identity(void)
{
    struct test_pair pair;
    struct roce_cm_frame outer;
    uint8_t wrong[ROCE_CM_FRAME_SIZE];
    init_pair(&pair);
    send_at(&pair.active, 0, pair.req);
    struct roce_cm_message request = unpack(pair.req, &outer);
    ++request.req.service_id;
    pack(wrong, outer, &request);
    assert(!receive_at(&pair.passive, 0, wrong));
    --request.req.service_id;
    request.req.mtu = 5;
    pack(wrong, outer, &request);
    assert(!receive_at(&pair.passive, 0, wrong));
    request.req.mtu = 3;
    ++request.req.primary.remote_gid[15];
    pack(wrong, outer, &request);
    assert(!receive_at(&pair.passive, 0, wrong));
    --request.req.primary.remote_gid[15];
    ++outer.src.mac[5];
    pack(wrong, outer, &request);
    assert(!receive_at(&pair.passive, 0, wrong));
    assert(!pair.passive.peer_qpn);
    assert(receive_at(&pair.passive, 0, pair.req));
    send_at(&pair.passive, 0, pair.rep);

    struct roce_cm_message reply = unpack(pair.rep, &outer);
    ++reply.tid;
    pack(wrong, outer, &reply);
    assert(!receive_at(&pair.active, 0, wrong));
    --reply.tid;
    ++reply.remote_comm_id;
    pack(wrong, outer, &reply);
    assert(!receive_at(&pair.active, 0, wrong));
    --reply.remote_comm_id;
    assert(!pair.active.peer_qpn);
    assert(receive_at(&pair.active, 0, pair.rep));
    send_at(&pair.active, 0, pair.rtu);
    ++reply.connection.local_qpn;
    pack(wrong, outer, &reply);
    assert(!receive_at(&pair.active, 0, wrong));
    assert(!roce_cm_session_prepare(&pair.active, 0, wrong, sizeof(wrong)));

    struct roce_cm_message rtu = unpack(pair.rtu, &outer);
    ++rtu.local_comm_id;
    pack(wrong, outer, &rtu);
    assert(!receive_at(&pair.passive, 0, wrong));
    --rtu.local_comm_id;
    ++rtu.remote_comm_id;
    pack(wrong, outer, &rtu);
    assert(!receive_at(&pair.passive, 0, wrong));
    assert(pair.passive.phase == ROCE_CM_PENDING);
    assert(receive_at(&pair.passive, 0, pair.rtu));
}

static void check_retry_limit(void)
{
    struct test_pair pair;
    struct roce_cm_session_config active;
    struct roce_cm_session_config passive;
    test_configs(&active, &passive);
    active.max_retries = 1;
    assert(roce_cm_session_init(&pair.active, &active));
    assert(roce_cm_session_init(&pair.passive, &passive));
    send_at(&pair.active, 0, pair.req);
    assert(receive_at(&pair.passive, 0, pair.req));
    send_at(&pair.passive, 0, pair.rep);
    send_at(&pair.active, RETRY_NS, pair.req);
    send_at(&pair.passive, RETRY_NS, pair.rep);
    assert(!roce_cm_session_prepare(&pair.active, 2 * RETRY_NS - 1, pair.req, sizeof(pair.req)));
    assert(pair.active.phase == ROCE_CM_PENDING);
    assert(!roce_cm_session_prepare(&pair.active, 2 * RETRY_NS, pair.req, sizeof(pair.req)));
    assert(!roce_cm_session_prepare(&pair.passive, 2 * RETRY_NS, pair.rep, sizeof(pair.rep)));
    assert(pair.active.phase == ROCE_CM_FAILED && pair.passive.phase == ROCE_CM_FAILED);
    assert(!receive_at(&pair.active, 2 * RETRY_NS, pair.rep));

    active.private_length = ROCE_CMA_IPV4_REQ_USER_SIZE + 1;
    assert(!roce_cm_session_init(&pair.active, &active));
    passive.private_length = ROCE_CM_REP_PRIVATE_SIZE + 1;
    assert(!roce_cm_session_init(&pair.passive, &passive));
}

int main(void)
{
    check_loss();
    check_identity();
    check_retry_limit();
    puts("RoCEv2 CM session tests passed");
    return 0;
}
