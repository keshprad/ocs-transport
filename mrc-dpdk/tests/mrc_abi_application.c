#include "dpdk_io.h"
#include "mrc_dpdk.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mrc_ctl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_pause.h>

enum {
    FIRST_BYTES = 13,
    IMMEDIATE_BYTES = 1025,
    IMMEDIATE_FIRST_SGE_BYTES = 607,
    TOTAL_BYTES = FIRST_BYTES + IMMEDIATE_BYTES,
    SEND_PSN = 0xfffffe,
    FIRST_IMMEDIATE = 0x01020304,
    SECOND_IMMEDIATE = 0x12345678,
    DUPLEX_QPS = 2,
    DUPLEX_RAILS = 2,
    COUNTED_RAILS = 8,
    COUNTED_BYTES = COUNTED_RAILS * 1024 + 1,
    WIRE_TRANSPORT_ACK = 0xd1,
    WIRE_NACK = 0xdd,
    WIRE_PROBE_REQUEST = 0xde,
    WIRE_SACK = 0xdc,
    WIRE_WRITE_FIRST = 0xc6,
    WIRE_WRITE_MIDDLE = 0xc7,
    WIRE_WRITE_LAST_IMM = 0xc9,
    WIRE_WRITE_ONLY_IMM = 0xcb,
    WIRE_BTH_LEN = 12,
    WIRE_METH_LEN = 4,
    WIRE_TSETH_LEN = 4,
    WIRE_RETH_LEN = 16,
    WIRE_IMM_LEN = 4,
    WIRE_ICRC_LEN = 4,
    WIRE_PETH_LEN = 16,
    WIRE_NETH_LEN = 20,
    WIRE_SETH_LEN = 28,
    WIRE_CC_STATE_LEN = 8,
    WIRE_IPV6_LEN = 40,
    WIRE_SINGLE_SRH_LEN = 24,
    WIRE_UDP_LEN = 8,
    DUPLEX_BYTES = 2049,
    RX_FLOW_ROUNDS = 4,
    QA_PRIMER_BYTES = 192,
    QA_TRIGGER_BYTES = 160,
    QA_BYTES = 2 * DUPLEX_BYTES + QA_PRIMER_BYTES + QA_TRIGGER_BYTES,
    RX_FLOW_RESTORE = 0x80,
    RX_FLOW_PENALTY_MASK = 0x7f,
    BLACKOUT_TIMEOUT_EXPONENT = 16,
    BLACKOUT_FINITE_RETRIES = 3,
    LAST_HOP_TRIM_DSCP = 51,
    LAST_HOP_TRIM_REASON = 0x02,
    SINGLE_WRITE_UDP_LEN = 64,
    MAPPED_SERVICE_LEVEL = 7,
    SERVICE_DATA_DSCP = 26,
    SERVICE_RETRY_DSCP = 34,
    CONTROL_DSCP = 48,
    MPR_TEST_MAX = 3,
    MPR_TEST_ROUNDS = 3,
};

struct address {
    union ibv_gid gid;
    uint8_t port;
    uint8_t gid_index;
};

struct connection {
    union ibv_gid gid;
    uint32_t qpn;
    uint32_t psn;
    uint64_t remote_address;
    uint32_t rkey;
};

enum simplex_error {
    SIMPLEX_NO_ERROR,
    SIMPLEX_CQ_OVERRUN,
    SIMPLEX_REQUEST_ERROR,
    SIMPLEX_ACCESS_ERROR,
};

enum single_write_case {
    BLACKOUT_FINITE,
    BLACKOUT_INFINITE,
    LAST_HOP_TRIM,
    SERVICE_LEVEL_DSCP,
    DYNAMIC_MPR,
    DYNAMIC_MPR_RTS,
    STATIC_MPR,
};

static struct ibv_context *open_verbs(struct address *address, bool require_ipv6)
{
    struct ibv_device **devices = ibv_get_device_list(NULL);
    assert(devices);
    struct ibv_context *chosen = NULL;
    const union ibv_gid zero = {0};
    for (struct ibv_device **device = devices; *device && !chosen; device++) {
        struct ibv_context *context = ibv_open_device(*device);
        if (!context)
            continue;
        struct ibv_device_attr attr;
        if (ibv_query_device(context, &attr) == 0) {
            for (unsigned int port = 1; port <= attr.phys_port_cnt && !chosen; port++) {
                struct ibv_port_attr port_attr;
                uint16_t pkey;
                if (ibv_query_port(context, port, &port_attr) ||
                    port_attr.link_layer != IBV_LINK_LAYER_ETHERNET ||
                    ibv_query_pkey(context, port, 0, &pkey) || !pkey)
                    continue;
                for (int index = 0; index < port_attr.gid_tbl_len && index <= UINT8_MAX; index++) {
                    union ibv_gid gid;
                    if (ibv_query_gid(context, port, index, &gid) ||
                        memcmp(gid.raw, zero.raw, sizeof(gid.raw)) == 0)
                        continue;
                    if (require_ipv6) {
                        struct in6_addr ip;
                        memcpy(&ip, gid.raw, sizeof(ip));
                        if (IN6_IS_ADDR_V4MAPPED(&ip))
                            continue;
                    }
                    *address = (struct address){ .gid = gid, .port = port, .gid_index = index };
                    chosen = context;
                    break;
                }
            }
        }
        if (!chosen)
            ibv_close_device(context);
    }
    ibv_free_device_list(devices);
    return chosen;
}

static void path_for(char path[PATH_MAX], const char *directory, const char *name)
{
    int length = snprintf(path, PATH_MAX, "%s/%s", directory, name);
    assert(length > 0 && length < PATH_MAX);
}

static void publish(const char *directory, const char *name, const void *record, size_t length)
{
    char path[PATH_MAX], temporary[PATH_MAX];
    path_for(path, directory, name);
    int count = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    assert(count > 0 && (size_t)count < sizeof(temporary));
    FILE *file = fopen(temporary, "wb");
    assert(file);
    if (length)
        assert(fwrite(record, 1, length, file) == length);
    assert(fclose(file) == 0);
    assert(rename(temporary, path) == 0);
}

static void wait_for(const char *path)
{
    uint64_t deadline = rte_get_timer_cycles() + 4 * rte_get_timer_hz();
    while (access(path, F_OK) != 0) {
        assert(rte_get_timer_cycles() < deadline);
        rte_delay_us_sleep(1000);
    }
}

static void read_peer_connections(const char *directory, bool client,
                                  struct connection *peers, size_t count)
{
    char path[PATH_MAX];
    path_for(path, directory, client ? "rx.connection" : "tx.connection");
    wait_for(path);
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fread(peers, sizeof(*peers), count, file) == count);
    assert(fclose(file) == 0);
}

static void configure_qp(struct mrc_qp *qp, const struct address *address,
                          const struct connection *peer, uint32_t send_psn,
                          bool send, bool receive, uint64_t ev_profile_id, uint64_t cc_profile_id,
                          unsigned int qp_caps, uint8_t service_level, enum ibv_mtu path_mtu)
{
    struct ibv_qp_attr verbs = {
        .qp_state = IBV_QPS_INIT,
        .qp_access_flags = receive ? IBV_ACCESS_REMOTE_WRITE : 0,
        .pkey_index = 0,
    };
    struct mrc_qp_attr attrs = { .profile = { .ev_profile_id = ev_profile_id, .cc_profile_id = cc_profile_id } };
    int profiles = (ev_profile_id ? MRC_QP_EV_PROFILE_ID : 0) | (cc_profile_id ? MRC_QP_CC_PROFILE_ID : 0);
    assert(mrc_modify_qp(qp, &verbs, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_ACCESS_FLAGS,
                         profiles ? &attrs : NULL, profiles) == 0);

    verbs = (struct ibv_qp_attr){
        .qp_state = IBV_QPS_RTR, .path_mtu = path_mtu,
        .dest_qp_num = peer->qpn, .rq_psn = peer->psn,
        .ah_attr = {
            .is_global = 1, .port_num = address->port, .sl = service_level,
            .grh = { .dgid = peer->gid, .sgid_index = address->gid_index, .hop_limit = 64 },
        },
    };
    attrs = (struct mrc_qp_attr){
        .mpr.mpr_dest = 1, .wimm.max_wimm_dest = receive ? 1 : 0, .cap = qp_caps,
    };
    const int rtr_verbs = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN;
    const int rtr_attrs = MRC_QP_MPR_DEST | MRC_QP_MAX_WIMM_DEST | (qp_caps ? MRC_QP_CAP : 0);
    if (service_level) {
        verbs.ah_attr.sl = 16;
        assert(mrc_modify_qp(qp, &verbs, rtr_verbs, &attrs, rtr_attrs) == EINVAL);
        verbs.ah_attr.sl = service_level;
    }
    assert(mrc_modify_qp(qp, &verbs, rtr_verbs, &attrs, rtr_attrs) == 0);
    if (!send)
        return;

    verbs.qp_state = IBV_QPS_RTS;
    verbs.sq_psn = send_psn;
    attrs = (struct mrc_qp_attr){
        .mpr.mpr = 1, .wimm.max_wimm = 1, .timeout = 16,
        .retry = { .linear_cnt = 3, .exp_cnt = 4 },
    };
    const int rts_attrs = MRC_QP_MPR | MRC_QP_MAX_WIMM | MRC_QP_TIMEOUT | MRC_QP_RETRY_CNT;
    assert(mrc_modify_qp(qp, &verbs, IBV_QP_STATE | IBV_QP_SQ_PSN, &attrs, rts_attrs) == 0);
}

static void post_writes(struct mrc_qp *qp, const struct connection *peer,
                        struct ibv_sge sg[3], bool inline_mode)
{
    unsigned int inline_flag = inline_mode ? IBV_SEND_INLINE : 0;
    struct ibv_send_wr wr[] = {
        {
            .wr_id = 70, .opcode = IBV_WR_RDMA_WRITE, .num_sge = 1, .sg_list = &sg[0],
            .send_flags = inline_flag,
            .wr.rdma = { .remote_addr = peer->remote_address, .rkey = peer->rkey },
        },
        {
            .wr_id = 71, .opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
            .send_flags = IBV_SEND_SIGNALED | IBV_SEND_SOLICITED | inline_flag,
            .num_sge = 2, .sg_list = &sg[1], .imm_data = htonl(FIRST_IMMEDIATE),
            .wr.rdma = { .remote_addr = peer->remote_address + FIRST_BYTES, .rkey = peer->rkey },
        },
        {
            .wr_id = 72, .opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
            .send_flags = IBV_SEND_SIGNALED | inline_flag,
            .imm_data = htonl(SECOND_IMMEDIATE),
            .wr.rdma = { .remote_addr = peer->remote_address + TOTAL_BYTES, .rkey = peer->rkey },
        },
    };
    wr[0].next = &wr[1];
    wr[1].next = &wr[2];
    struct ibv_send_wr *bad = NULL;
    if (inline_mode) {
        /* A failed later WR leaves the successful prefix on the SQ. */
        struct ibv_sge too_long = { .addr = sg[0].addr, .length = IMMEDIATE_BYTES + 1 };
        struct ibv_send_wr oversized = wr[0];
        oversized.sg_list = &too_long;
        oversized.next = NULL;
        wr[0].next = &oversized;
        assert(mrc_post_send(qp, wr, &bad) == EMSGSIZE && bad == &oversized);
    }
    assert(mrc_post_send(qp, inline_mode ? &wr[1] : wr, &bad) == 0 && !bad);
}

static void post_receives(struct mrc_qp *qp, struct ibv_sge *sge)
{
    struct ibv_recv_wr credits[] = {
        { .wr_id = 81, .sg_list = sge, .num_sge = 1 },
        { .wr_id = 82 },
    };
    credits[0].next = &credits[1];
    struct ibv_recv_wr *bad = NULL;
    assert(mrc_post_recv(qp, credits, &bad) == 0 && !bad);
}

static int completion_fd(struct mrc_comp_channel *channel)
{
    int fd;
    assert(mrc_get_comp_channel_fd(channel, &fd) == 0);
    return fd;
}

static void acknowledge_notification(struct mrc_comp_channel *channel, struct mrc_cq *cq)
{
    struct pollfd wait = { .fd = completion_fd(channel), .events = POLLIN };
    assert(poll(&wait, 1, 0) == 1 && (wait.revents & POLLIN));
    struct mrc_cq *event = NULL;
    void *event_context = NULL;
    assert(mrc_get_cq_event(channel, &event, &event_context) == 0 && event == cq);
    mrc_ack_cq_events(cq, 1);
}

static void complete(struct mrc_qp *qp, struct mrc_cq *cq, struct mrc_comp_channel *channel,
                     const char *directory, bool requester, uint32_t qpn)
{
    uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
    char done[PATH_MAX];
    path_for(done, directory, "tx.done");
    unsigned int completed = 0;
    while (completed < 2 || (!requester && access(done, F_OK) != 0)) {
        assert(rte_get_timer_cycles() < deadline);
        int progress = mrc_dpdk_progress_qp(qp);
        if (progress) {
            fprintf(stderr, "QP progress failed: %s (%d)\n", strerror(progress), progress);
            abort();
        }
        struct ibv_wc wc[2];
        int entries = mrc_poll_cq(cq, 2, wc);
        assert(entries >= 0 && completed + (unsigned int)entries <= 2);
        for (int entry = 0; entry < entries; entry++, completed++) {
            assert(wc[entry].status == IBV_WC_SUCCESS && wc[entry].qp_num == qpn);
            assert(wc[entry].wr_id == (requester ? 71 : 81) + completed);
            if (requester) {
                assert(wc[entry].opcode == IBV_WC_RDMA_WRITE);
            } else {
                assert(wc[entry].opcode == IBV_WC_RECV_RDMA_WITH_IMM);
                assert(wc[entry].wc_flags & IBV_WC_WITH_IMM);
                assert(wc[entry].byte_len == (completed == 0 ? IMMEDIATE_BYTES : 0));
                assert(ntohl(wc[entry].imm_data) == (completed == 0 ? FIRST_IMMEDIATE : SECOND_IMMEDIATE));
                if (completed == 0) {
                    acknowledge_notification(channel, cq);
                    assert(mrc_req_notify_cq(cq, 1) == 0);
                }
            }
        }
        rte_pause();
    }
    struct ibv_wc unexpected;
    assert(mrc_poll_cq(cq, 1, &unexpected) == 0);
}

static uint8_t pattern(size_t offset)
{
    return (uint8_t)(offset ^ (offset >> 8) ^ 0xa7);
}

static void post_inline_writes(struct mrc_qp *qp, const struct connection *peer)
{
    const size_t lengths[] = { FIRST_BYTES, IMMEDIATE_FIRST_SGE_BYTES,
                               IMMEDIATE_BYTES - IMMEDIATE_FIRST_SGE_BYTES };
    struct ibv_sge sg[3];
    size_t offset = 0;
    for (unsigned int i = 0; i < 3; i++) {
        size_t allocation = i ? lengths[i] : IMMEDIATE_BYTES + 1;
        uint8_t *source = malloc(allocation);
        assert(source);
        for (size_t j = 0; j < lengths[i]; j++)
            source[j] = pattern(offset++);
        sg[i] = (struct ibv_sge){ .addr = (uintptr_t)source, .length = lengths[i] };
    }
    post_writes(qp, peer, sg, true);
    for (unsigned int i = 0; i < 3; i++) {
        void *source = (void *)(uintptr_t)sg[i].addr;
        explicit_bzero(source, lengths[i]);
        free(source);
    }
}

static void post_registered_writes(struct mrc_qp *qp, const struct connection *peer,
                                   const struct ibv_mr *first, const struct ibv_mr *last)
{
    struct ibv_sge sg[] = {
        { .addr = (uintptr_t)first->addr, .length = FIRST_BYTES, .lkey = first->lkey },
        {
            .addr = (uintptr_t)first->addr + FIRST_BYTES,
            .length = IMMEDIATE_FIRST_SGE_BYTES, .lkey = first->lkey,
        },
        {
            .addr = (uintptr_t)last->addr,
            .length = IMMEDIATE_BYTES - IMMEDIATE_FIRST_SGE_BYTES, .lkey = last->lkey,
        },
    };
    post_writes(qp, peer, sg, false);
}

static void post_single_write_imm(struct mrc_qp *qp, const struct connection *peer,
                                   const struct ibv_mr *source, unsigned int round)
{
    size_t offset = round * FIRST_BYTES;
    struct ibv_sge sg = {
        .addr = (uintptr_t)source->addr + offset, .length = FIRST_BYTES, .lkey = source->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id = 70 + round, .opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
        .send_flags = IBV_SEND_SIGNALED, .num_sge = 1, .sg_list = &sg,
        .wr.rdma = { .remote_addr = peer->remote_address + offset, .rkey = peer->rkey },
        .imm_data = htonl(FIRST_IMMEDIATE + round),
    };
    struct ibv_send_wr *bad = NULL;
    assert(mrc_post_send(qp, &wr, &bad) == 0 && !bad);
}


static struct ibv_mr *register_memory(struct mrc_context *context, struct ibv_pd *pd,
                                      uint8_t *memory, size_t length, int access)
{
    struct ibv_mr *mr = ibv_reg_mr(pd, memory, length, access);
    assert(mr && mrc_dpdk_bind_mr(context, mr, (uintptr_t)memory, access) == 0);
    return mr;
}

static void deregister_memory(struct mrc_context *context, struct ibv_mr *mr)
{
    assert(mrc_dpdk_unbind_mr(context, mr) == 0);
    assert(ibv_dereg_mr(mr) == 0);
}

static struct mrc_dpdk_route path_route(struct rte_mempool *pool, bool client)
{
    struct mrc_dpdk_route route = { .ports = {0, 1}, .pool = pool };
    uint8_t peer_mac_end = client ? 0x10 : 0x20;
    for (unsigned int port = 0; port < 2; port++) {
        route.peer_macs[port][0] = 2;
        route.peer_macs[port][5] = peer_mac_end + port;
    }
    return route;
}

static void attach_qp(struct mrc_qp *qp, const struct mrc_dpdk_route *route)
{
    int attached = mrc_dpdk_attach_qp(qp, route);
    if (attached) {
        fprintf(stderr, "Failed to attach QP: %s (%d)\n", strerror(attached), attached);
        abort();
    }
}

static void skip_local_qp_numbers(struct mrc_context *context, struct mrc_qp_init_attr *init,
                                  unsigned int count)
{
    /* Make the two independent processes allocate disjoint QPNs. */
    for (unsigned int i = 0; i < count; i++) {
        struct mrc_qp *unused = mrc_create_qp(context, init);
        assert(unused && mrc_destroy_qp(unused) == 0);
    }
}

static void complete_async_error(struct mrc_context *context, struct mrc_qp *qp,
                                 struct mrc_cq *cq, bool requester, uint32_t qpn,
                                 enum simplex_error error_mode)
{
    const bool cq_overrun = error_mode == SIMPLEX_CQ_OVERRUN;
    const bool request_error = error_mode == SIMPLEX_REQUEST_ERROR;
    int fd;
    assert(mrc_dpdk_get_async_fd(context, &fd) == 0);
    int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();

    /* In the overrun case, leaving both completions in the single-slot
     * receive CQ causes UNEXP_EVENT on the second WRITEIMM. */
    int result;
    do {
        assert(rte_get_timer_cycles() < deadline);
        result = mrc_dpdk_progress_qp(qp);
        rte_pause();
    } while (result == 0);
    assert(result == (requester && cq_overrun ? EREMOTEIO : EPROTO));

    struct pollfd ready = { .fd = fd, .events = POLLIN };
    assert(poll(&ready, 1, 0) == 1 && (ready.revents & POLLIN));
    struct mrc_async_event event;
    if (!requester && cq_overrun) {
        assert(mrc_get_async_event(context, &event) == 0);
        assert(event.event_type == IBV_EVENT_CQ_ERR && event.element.cq == cq);
        mrc_ack_async_event(&event);
    }
    assert(mrc_get_async_event(context, &event) == 0);
    enum ibv_event_type expected = IBV_EVENT_QP_FATAL;
    if (!requester && !cq_overrun)
        expected = request_error ? IBV_EVENT_QP_REQ_ERR : IBV_EVENT_QP_ACCESS_ERR;
    assert(event.event_type == expected && event.element.qp == qp);
    mrc_ack_async_event(&event);
    assert(poll(&ready, 1, 0) == 0);
    assert(mrc_get_async_event(context, &event) == -1 && errno == EAGAIN);

    struct ibv_qp_attr observed;
    assert(mrc_query_qp(qp, &observed, IBV_QP_STATE, NULL, 0, NULL) == 0);
    assert(observed.qp_state == IBV_QPS_ERR);
    struct ibv_wc entries[2];
    if (requester && cq_overrun) {
        assert(mrc_poll_cq(cq, 2, entries) == 2);
        assert(entries[0].status == IBV_WC_SUCCESS && entries[0].wr_id == 71 &&
               entries[0].qp_num == qpn);
        assert(entries[1].status == IBV_WC_REM_OP_ERR && entries[1].wr_id == 72 &&
               entries[1].qp_num == qpn);
    } else if (cq_overrun) {
        assert(mrc_poll_cq(cq, 2, entries) == -1 && errno == EOVERFLOW);
    } else if (requester) {
        enum ibv_wc_status status = request_error ? IBV_WC_REM_INV_REQ_ERR : IBV_WC_REM_ACCESS_ERR;
        assert(mrc_poll_cq(cq, 2, entries) == 1);
        assert(entries[0].status == status && entries[0].wr_id == 70 && entries[0].qp_num == qpn);
    } else {
        assert(mrc_poll_cq(cq, 2, entries) == 2);
        for (unsigned int i = 0; i < 2; i++)
            assert(entries[i].status == IBV_WC_WR_FLUSH_ERR && entries[i].wr_id == 81 + i &&
                   entries[i].qp_num == qpn);
    }
    if (!cq_overrun)
        assert(mrc_poll_cq(cq, 1, entries) == 0);
    assert(mrc_get_async_event(context, &event) == -1 && errno == EAGAIN);
}

static void check_blocked_profile(struct mrc_qp *qp, struct mrc_cq *cq, const char *store,
                                  bool requester, const uint8_t received[TOTAL_BYTES])
{
    uint64_t hz = rte_get_timer_hz();
    uint64_t deadline = rte_get_timer_cycles() + (requester ? hz / 50 : 4 * hz);
    char sender_blocked[PATH_MAX];
    path_for(sender_blocked, store, "tx.profile-blocked");
    do {
        struct ibv_wc wc;
        assert(mrc_dpdk_progress_qp(qp) == 0);
        assert(mrc_poll_cq(cq, 1, &wc) == 0);
        rte_pause();
    } while (rte_get_timer_cycles() < deadline &&
             (requester || access(sender_blocked, F_OK) != 0));

    if (requester) {
        publish(store, "tx.profile-blocked", NULL, 0);
    } else {
        assert(access(sender_blocked, F_OK) == 0);
        for (size_t offset = 0; offset < TOTAL_BYTES; offset++)
            assert(received[offset] == 0);
        publish(store, "rx.profile-blocked", NULL, 0);
    }
}

static uint16_t wire_be16(const uint8_t *bytes)
{
    return ((uint16_t)bytes[0] << 8) | bytes[1];
}

static uint32_t wire_be24(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 16) | wire_be16(bytes + 1);
}

static uint32_t wire_be32(const uint8_t *bytes)
{
    return ((uint32_t)wire_be16(bytes) << 16) | wire_be16(bytes + 2);
}

struct observed_udp {
    const uint8_t *header;
    size_t bytes;
    uint32_t entropy;
    uint8_t traffic_class;
};

static struct observed_udp read_udp(struct rte_mbuf *packet, uint8_t *prefix, size_t capacity)
{
    size_t length = rte_pktmbuf_pkt_len(packet);
    if (length > capacity)
        length = capacity;
    const uint8_t *frame = rte_pktmbuf_read(packet, 0, length, prefix);
    assert(frame && length >= 14 + 20 + 8 + WIRE_BTH_LEN);
    uint16_t ether_type = wire_be16(frame + 12);
    assert(ether_type == RTE_ETHER_TYPE_IPV4 || ether_type == RTE_ETHER_TYPE_IPV6);
    bool ipv6 = ether_type == RTE_ETHER_TYPE_IPV6;
    size_t udp = 14 + (ipv6 ? 40 : (frame[14] & 0x0f) * 4);
    assert(udp + 8 + WIRE_BTH_LEN <= length);
    uint32_t entropy = wire_be16(frame + udp);
    if (ipv6)
        entropy |= (uint32_t)wire_be16(frame + 16) << 16;
    uint8_t traffic_class = ipv6 ? (uint8_t)(wire_be16(frame + 14) >> 4) : frame[15];
    return (struct observed_udp){
        .header = frame + udp, .bytes = length - udp, .entropy = entropy, .traffic_class = traffic_class,
    };
}

static void prepare_memif_rails(unsigned int rail_count)
{
    uint64_t deadline = rte_get_timer_cycles() + 4 * rte_get_timer_hz();
    for (uint16_t port = 0; port < rail_count; port++) {
        struct rte_eth_link link;
        do {
            assert(rte_get_timer_cycles() < deadline);
            assert(rte_eth_link_get_nowait(port, &link) == 0);
            if (!link.link_status)
                rte_delay_us_sleep(1000);
        } while (!link.link_status);

        /* Memif publishes server-to-client ring credits on the client's first
         * receive poll. Neither process can send before the round barrier. */
        struct rte_mbuf *packet;
        assert(rte_eth_rx_burst(port, 0, &packet, 1) == 0);
    }
}

static void prepare_memif_links(void)
{
    prepare_memif_rails(DUPLEX_RAILS);
}

enum simplex_wire_mode {
    SIMPLEX_WIRE_NONE,
    SIMPLEX_INLINE,
    SIMPLEX_PMTU_256,
    SIMPLEX_PMTU_512,
    SIMPLEX_GENERATED_EV,
    SIMPLEX_SRV6,
    SIMPLEX_SRV6_TRANSIT,
    SIMPLEX_SRV6_EXPLICIT_EV,
    SIMPLEX_SRV6_GENERATED_EV,
    SIMPLEX_SRV6_MAPPED_EV,
};

static void test_sid(uint8_t sid[16], uint16_t port, uint8_t endpoint)
{
    const uint8_t value[16] = {0x20, 0x01, 0x0d, 0xb8, 0, port + 1, [15] = endpoint};
    memcpy(sid, value, sizeof(value));
}

enum endpoint_case {
    EP_HELD,
    EP_FIRST_REPLY,
    EP_PARTIAL_REPLY,
    EP_STALE,
    EP_RETRY,
    EP_STATUS_FIRST,
    EP_STATUS_SECOND,
    EP_CASES,
};

static const struct {
    uint16_t token;
    uint8_t rail;
    uint32_t port_status;
} endpoint_cases[EP_CASES] = {
    [EP_HELD] = { 0xffff, 0, 0 },
    [EP_FIRST_REPLY] = { 0x001b, 1, 0 },
    [EP_PARTIAL_REPLY] = { 0x8246, 0, 0 },
    [EP_STALE] = { 0, 1, 0 },
    [EP_RETRY] = { 0x8011, 1, 0 },
    [EP_STATUS_FIRST] = { 0x50f1, 0, 3 },
    [EP_STATUS_SECOND] = { 0x10d3, 1, 2 },
};

enum {
    EP_REQUEST_OPCODE = 0xd8,
    EP_RESPONSE_OPCODE = 0xd9,
    EP_REQUEST_HEADER_LEN = 16,
    EP_RESPONSE_HEADER_LEN = 36,
    EP_PROBE_DSCP = 27,
    EP_CONTROL_DSCP = 48,
    EP_UDP_PORT = 4810,
    EP_TX_PKEY = 0x9912,
    EP_RX_PKEY = 0xa932,
    EP_ALL_CASES = (1 << EP_CASES) - 1,
    COUNTED_EP_PROBE = 0xaf18,
    COUNTED_EP_STATUS = 0xaf19,
    COUNTED_EP_MASK = (1 << COUNTED_RAILS) | (1 << 1),
};

struct endpoint_witness {
    bool client;
    enum mrc_ctl_ev_fmt_mode mode;
    const char *directory;
    char release[PATH_MAX];
    struct mrc_dpdk_endpoint_route route;
    struct mrc_dpdk_srv6_path paths[DUPLEX_RAILS];
    struct rte_mbuf *held;
    struct rte_mbuf *stale;
    enum endpoint_case replies[EP_CASES];
    unsigned int reply_count;
    unsigned int seen;
    unsigned int stamped;
    unsigned int adjusted_rtts;
};

static uint32_t probe_entropy(enum mrc_ctl_ev_fmt_mode mode, uint16_t rail)
{
    const uint32_t ports[] = { 0x3197, 0x427c };
    const uint32_t labels[] = { 0x16db, 0x259c };
    return ports[rail] | (mode == MRC_CTL_EV_FMT_MODE_STEV ? labels[rail] << 16 : 0);
}

static enum endpoint_case observe_endpoint_packet(struct endpoint_witness *test, uint16_t rail,
                                                  struct rte_mbuf *packet)
{
    uint8_t scratch[192];
    size_t len = rte_pktmbuf_pkt_len(packet);
    assert(rail < DUPLEX_RAILS && len <= sizeof(scratch) && len >= RTE_ETHER_HDR_LEN + 20 + 8 + 12);
    const uint8_t *frame = rte_pktmbuf_read(packet, 0, len, scratch);
    assert(frame && !memcmp(frame + RTE_ETHER_ADDR_LEN, test->route.peer_macs[rail], RTE_ETHER_ADDR_LEN));
    bool ipv6 = test->mode != MRC_CTL_EV_FMT_MODE_UDP;
    assert(wire_be16(frame + 12) == (ipv6 ? RTE_ETHER_TYPE_IPV6 : RTE_ETHER_TYPE_IPV4));
    size_t offset = RTE_ETHER_HDR_LEN;
    unsigned int outer_dscp = 0;
    if (test->mode == MRC_CTL_EV_FMT_MODE_SRV6) {
        const uint8_t *outer = frame + offset;
        const struct mrc_dpdk_srv6_path *path = &test->paths[rail];
        bool transit = !test->client && rail == 1;
        assert(len >= offset + WIRE_IPV6_LEN);
        assert((outer[0] >> 4) == 6 && wire_be16(outer + 4) == len - offset - WIRE_IPV6_LEN);
        outer_dscp = (wire_be16(outer) >> 6) & 63;
        assert(!memcmp(outer + 8, path->peer_sid, 16));
        offset += WIRE_IPV6_LEN;
        if (transit) {
            uint8_t initial[16];
            test_sid(initial, rail, 0x33);
            const uint8_t *srh = frame + offset;
            assert(len >= offset + WIRE_SINGLE_SRH_LEN);
            assert(outer[6] == IPPROTO_ROUTING && !memcmp(outer + 24, initial, 16));
            assert(srh[0] == IPPROTO_IPV6 && srh[1] == 2 && srh[2] == 4 && srh[3] == 1 && srh[4] == 0);
            assert(!memcmp(srh + 8, path->local_sid, 16));
            /* Model the last SRv6 hop before the peer's endpoint dispatch. */
            uint8_t *data = rte_pktmbuf_mtod(packet, uint8_t *);
            assert(rte_pktmbuf_is_contiguous(packet));
            memcpy(data + RTE_ETHER_HDR_LEN + 24, path->local_sid, 16);
            data[offset + 3] = 0;
            offset += WIRE_SINGLE_SRH_LEN;
        } else {
            assert(outer[6] == IPPROTO_IPV6 && !memcmp(outer + 24, path->local_sid, 16));
        }
    }
    size_t ip_len = ipv6 ? WIRE_IPV6_LEN : 20;
    assert(len >= offset + ip_len + WIRE_UDP_LEN + WIRE_BTH_LEN);
    const uint8_t *ip = frame + offset;
    assert((ip[0] >> 4) == (ipv6 ? 6 : 4) && ip[ipv6 ? 6 : 9] == IPPROTO_UDP);
    assert(!memcmp(ip + (ipv6 ? 8 : 12), test->route.peer_gid.raw + (ipv6 ? 0 : 12), ipv6 ? 16 : 4));
    assert(!memcmp(ip + (ipv6 ? 24 : 16), test->route.local_gid.raw + (ipv6 ? 0 : 12), ipv6 ? 16 : 4));
    const uint8_t *udp = ip + ip_len;
    const uint8_t *bth = udp + WIRE_UDP_LEN;
    assert(wire_be16(udp + 2) == EP_UDP_PORT);
    assert(bth[0] == (test->client ? EP_RESPONSE_OPCODE : EP_REQUEST_OPCODE));
    assert(wire_be16(bth + 2) == test->route.peer_pkey && wire_be24(bth + 5) == 2);
    uint32_t token = wire_be24(bth + 9);
    enum endpoint_case entry;
    for (entry = 0; entry < EP_CASES; entry++) {
        if (token == endpoint_cases[entry].token)
            break;
    }
    assert(entry < EP_CASES && endpoint_cases[entry].rail == rail && !(test->seen & (1u << entry)));
    test->seen |= 1u << entry;
    const uint8_t *extension = bth + WIRE_BTH_LEN;
    unsigned int dscp = ipv6 ? (wire_be16(ip) >> 6) & 63 : ip[1] >> 2;
    uint32_t port_status = endpoint_cases[entry].port_status;
    assert(dscp == (test->client || port_status ? EP_CONTROL_DSCP : EP_PROBE_DSCP));
    if (test->mode == MRC_CTL_EV_FMT_MODE_SRV6)
        assert(outer_dscp == dscp);
    size_t udp_len = WIRE_UDP_LEN + WIRE_BTH_LEN + 4 +
        (test->client ? EP_RESPONSE_HEADER_LEN : EP_REQUEST_HEADER_LEN);
    assert(wire_be16(udp + 4) == udp_len && len == offset + ip_len + udp_len);
    if (test->client) {
        assert(extension[1] == (port_status ? 0 : 4));
        if (wire_be16(extension + 28))
            test->stamped |= 1u << entry;
    } else {
        assert(extension[1] == (port_status ? 0 : 1));
        assert(wire_be32(extension + 4) == port_status);
        assert((extension[15] & 15) == 0);
        if (test->mode != MRC_CTL_EV_FMT_MODE_SRV6) {
            uint32_t entropy = wire_be16(udp);
            if (ipv6)
                entropy |= (uint32_t)wire_be16(ip + 2) << 16;
            assert(entropy == probe_entropy(test->mode, rail));
        }
    }
    return entry;
}

static uint16_t observe_endpoint_batch(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                        uint16_t count, uint16_t capacity, void *context)
{
    (void)queue;
    struct endpoint_witness *test = context;
    uint16_t ready = 0;
    uint16_t retry = UINT16_MAX;
    for (uint16_t i = 0; i < count; i++) {
        struct rte_mbuf *packet = packets[i];
        enum endpoint_case entry = observe_endpoint_packet(test, port, packet);
        if (test->client) {
            assert(test->reply_count < EP_CASES);
            test->replies[test->reply_count++] = entry;
            if (entry == EP_FIRST_REPLY)
                publish(test->directory, "tx.first-reply", NULL, 0);
        } else if (entry == EP_HELD) {
            test->held = packet;
            continue;
        } else if (entry == EP_STALE) {
            test->stale = packet;
            continue;
        } else if (entry == EP_RETRY) {
            retry = ready;
        }
        packets[ready++] = packet;
    }
    if (retry != UINT16_MAX) {
        assert(ready < capacity && test->stale);
        /* Deliver the old, valid EETH before the current reply. */
        memmove(packets + retry + 1, packets + retry, (ready - retry) * sizeof(*packets));
        packets[retry] = test->stale;
        test->stale = NULL;
        ready++;
    }
    if (!test->client && port == 0 && test->held && ready < capacity && access(test->release, F_OK) == 0) {
        packets[ready++] = test->held;
        test->held = NULL;
    }
    return ready;
}

static struct mrc_ctl_ep_req endpoint_request(const struct endpoint_witness *test, enum endpoint_case entry)
{
    uint16_t rail = endpoint_cases[entry].rail;
    struct mrc_ctl_ep_req request = {
        .req_id = endpoint_cases[entry].token, .sgid = test->route.local_gid,
        .dgid = test->route.peer_gid, .ev_fmt_mode = test->mode, .req_ev.port = rail + 1,
        .op.port_status_update.port_status = endpoint_cases[entry].port_status << 1,
    };
    if (test->mode == MRC_CTL_EV_FMT_MODE_SRV6) {
        if (rail == 0) {
            memcpy(request.req_ev.val, test->paths[rail].peer_sid, 16);
        } else {
            test_sid(request.req_ev.val, rail, 0x33);
            memcpy(request.req_ev.val + 16, test->paths[rail].peer_sid, 16);
        }
    } else {
        uint32_t encoded = htonl(probe_entropy(test->mode, rail));
        memcpy(request.req_ev.val, &encoded, sizeof(encoded));
    }
    return request;
}

static void check_endpoint_reply(struct endpoint_witness *test, const struct mrc_ctl_ep_rsp *reply,
                                  enum endpoint_case entry)
{
    assert(reply->req_id == endpoint_cases[entry].token && reply->port == endpoint_cases[entry].rail + 1);
    assert(reply->rtt > 0 && reply->adj_svc_time == !!(test->stamped & (1u << entry)));
    test->adjusted_rtts += reply->adj_svc_time;
}

static void run_endpoint_batches(struct mrc_context *context, struct endpoint_witness *test)
{
    char ready_path[PATH_MAX];
    path_for(ready_path, test->directory, "rx.endpoint-ready");
    wait_for(ready_path);
    struct mrc_ctl_ep_rsp replies[DUPLEX_RAILS];
    int returned = -1;
    struct mrc_ctl_ep_req requests[DUPLEX_RAILS] = {
        endpoint_request(test, EP_HELD), endpoint_request(test, EP_HELD),
    };
    assert(mrc_ctl_ep_batch_send_wait(context, EP_PROBE_DSCP, MRC_CTL_EP_OP_EV_PROBE, requests, 2,
                                      1000000000, replies, &returned) == -1);
    assert(errno == EINVAL && returned == 0);
    requests[1] = endpoint_request(test, EP_FIRST_REPLY);
    assert(mrc_ctl_ep_batch_send_wait(context, 64, MRC_CTL_EP_OP_EV_PROBE, requests, 2,
                                      1000000000, replies, &returned) == -1);
    assert(errno == EINVAL && returned == 0);
    requests[1].dgid.raw[15] ^= 1;
    assert(mrc_ctl_ep_batch_send_wait(context, EP_PROBE_DSCP, MRC_CTL_EP_OP_EV_PROBE, requests, 2,
                                      1000000000, replies, &returned) == -1);
    assert(errno == EINVAL && returned == 0);
    requests[1] = endpoint_request(test, EP_FIRST_REPLY);
    assert(mrc_ctl_ep_batch_send_wait(context, EP_PROBE_DSCP, MRC_CTL_EP_OP_EV_PROBE, requests, 2,
                                      1000000000, replies, &returned) == 0);
    assert(returned == 2);
    check_endpoint_reply(test, &replies[0], EP_FIRST_REPLY);
    check_endpoint_reply(test, &replies[1], EP_HELD);

    requests[0] = endpoint_request(test, EP_PARTIAL_REPLY);
    requests[1] = endpoint_request(test, EP_STALE);
    assert(mrc_ctl_ep_batch_send_wait(context, EP_PROBE_DSCP, MRC_CTL_EP_OP_EV_PROBE, requests, 2,
                                      100000000, replies, &returned) == -1);
    assert(errno == ETIMEDOUT && returned == 1);
    check_endpoint_reply(test, &replies[0], EP_PARTIAL_REPLY);

    requests[0] = endpoint_request(test, EP_RETRY);
    assert(mrc_ctl_ep_batch_send_wait(context, EP_PROBE_DSCP, MRC_CTL_EP_OP_EV_PROBE, requests, 1,
                                      1000000000, replies, &returned) == 0);
    assert(returned == 1);
    check_endpoint_reply(test, &replies[0], EP_RETRY);
    assert(test->reply_count == 5 && test->replies[3] == EP_STALE && test->replies[4] == EP_RETRY);

    requests[0] = endpoint_request(test, EP_STATUS_FIRST);
    requests[1] = endpoint_request(test, EP_STATUS_SECOND);
    requests[1].op.port_status_update.port_status |= 1;
    assert(mrc_ctl_ep_batch_send_wait(context, EP_PROBE_DSCP, MRC_CTL_EP_OP_PORT_STATUS_UPDATE, requests, 2,
                                      1000000000, replies, &returned) == -1);
    assert(errno == EINVAL && returned == 0);
    requests[1] = endpoint_request(test, EP_STATUS_SECOND);
    assert(mrc_ctl_ep_batch_send_wait(context, EP_PROBE_DSCP, MRC_CTL_EP_OP_PORT_STATUS_UPDATE, requests, 2,
                                      1000000000, replies, &returned) == 0);
    assert(returned == 2 && test->reply_count == EP_CASES);
    check_endpoint_reply(test, &replies[0], test->replies[5]);
    check_endpoint_reply(test, &replies[1], test->replies[6]);
    publish(test->directory, "tx.endpoint-done", NULL, 0);
}

static void run_endpoint_ops(bool client, const char *directory, const char *format,
                             const struct address *address, struct ibv_context *verbs,
                             struct mrc_context *context, struct ibv_pd *pd, struct rte_mempool *pool)
{
    bool shared = !strcmp(format, "shared");
    enum mrc_ctl_ev_fmt_mode mode = MRC_CTL_EV_FMT_MODE_UDP;
    if (shared || !strcmp(format, "stev"))
        mode = MRC_CTL_EV_FMT_MODE_STEV;
    else if (!strcmp(format, "srv6"))
        mode = MRC_CTL_EV_FMT_MODE_SRV6;
    else
        assert(!strcmp(format, "udp"));
    struct endpoint_witness test = {
        .client = client, .mode = mode, .directory = directory,
        .route = {
            .ports = {0, 1}, .pool = pool, .udp_port = EP_UDP_PORT,
            .pkey = client ? EP_TX_PKEY : EP_RX_PKEY, .peer_pkey = client ? EP_RX_PKEY : EP_TX_PKEY,
        },
    };
    const char *tx_gid = mode == MRC_CTL_EV_FMT_MODE_UDP ? "::ffff:10.9.0.1" : "2001:db8::11";
    const char *rx_gid = mode == MRC_CTL_EV_FMT_MODE_UDP ? "::ffff:10.9.0.2" : "2001:db8::22";
    assert(inet_pton(AF_INET6, client ? tx_gid : rx_gid, test.route.local_gid.raw) == 1);
    assert(inet_pton(AF_INET6, client ? rx_gid : tx_gid, test.route.peer_gid.raw) == 1);
    if (shared) {
        uint16_t pkey;
        assert(ibv_query_pkey(verbs, address->port, 0, &pkey) == 0);
        const struct connection local = { .gid = address->gid };
        struct connection peer;
        publish(directory, client ? "tx.connection" : "rx.connection", &local, sizeof(local));
        read_peer_connections(directory, client, &peer, 1);
        test.route.local_gid = local.gid;
        test.route.peer_gid = peer.gid;
        test.route.pkey = test.route.peer_pkey = ntohs(pkey);
    }
    for (uint16_t rail = 0; rail < DUPLEX_RAILS; rail++) {
        test.route.peer_macs[rail][0] = 2;
        test.route.peer_macs[rail][5] = (client ? 0x10 : 0x20) + rail;
        if (mode == MRC_CTL_EV_FMT_MODE_SRV6) {
            test_sid(test.paths[rail].local_sid, rail, client ? 0x11 : 0x22);
            test_sid(test.paths[rail].peer_sid, rail, client ? 0x22 : 0x11);
        }
    }
    path_for(test.release, directory, "tx.first-reply");
    const struct mrc_dpdk_srv6_path *paths = mode == MRC_CTL_EV_FMT_MODE_SRV6 ? test.paths : NULL;
    struct mrc_ctl_device_attr capability;
    int supported;
    assert(mrc_ctl_query_device_ex(NULL, context, &capability, &supported) == 0 && supported);
    assert(capability.opt_attr & MRC_CTL_OPT_CAP_EP_OP_EV_PROBE);
    assert(mrc_dpdk_attach_endpoint(context, &test.route, paths) == 0);
    struct mrc_cq *idle_cq = NULL;
    struct mrc_qp *idle_qp = NULL;
    if (shared && !client) {
        idle_cq = mrc_create_cq(context, 1, NULL, NULL, 0);
        assert(idle_cq);
        struct mrc_qp_init_attr init = {
            .pd = pd, .protocol_version = MRC_PROTOCOL_VERSION_1, .recv_cq = idle_cq,
            .cap.max_recv_wr = 1,
        };
        idle_qp = mrc_create_qp(context, &init);
        assert(idle_qp);
        const struct connection peer = { .gid = test.route.peer_gid, .qpn = 13 };
        configure_qp(idle_qp, address, &peer, 0, false, true, 0, 0, 0, 0, IBV_MTU_1024);
        struct mrc_dpdk_route route = path_route(pool, false);
        route.peer_pkey = test.route.peer_pkey;
        route.udp_port = test.route.udp_port;
        attach_qp(idle_qp, &route);
    }
    prepare_memif_links();
    const struct rte_eth_rxtx_callback *callbacks[DUPLEX_RAILS];
    for (uint16_t rail = 0; rail < DUPLEX_RAILS; rail++) {
        callbacks[rail] = rte_eth_add_rx_callback(rail, 0, observe_endpoint_batch, &test);
        assert(callbacks[rail]);
    }
    if (client) {
        run_endpoint_batches(context, &test);
    } else {
        char done[PATH_MAX];
        path_for(done, directory, "tx.endpoint-done");
        publish(directory, "rx.endpoint-ready", NULL, 0);
        uint64_t deadline = rte_get_timer_cycles() + 4 * rte_get_timer_hz();
        while (access(done, F_OK) != 0) {
            assert(rte_get_timer_cycles() < deadline);
            assert(mrc_dpdk_progress_endpoint(context) == 0);
        }
    }
    assert(test.seen == EP_ALL_CASES && !test.held && !test.stale);
    for (uint16_t rail = 0; rail < DUPLEX_RAILS; rail++) {
        assert(rte_eth_remove_rx_callback(rail, 0, callbacks[rail]) == 0);
        rte_free((void *)(uintptr_t)callbacks[rail]);
    }
    assert(mrc_dpdk_detach_endpoint(context) == 0);
    if (idle_qp) {
        assert(mrc_destroy_qp(idle_qp) == 0);
        assert(mrc_destroy_cq(idle_cq) == 0);
    }
    printf("{\"role\":\"%s\",\"format\":\"%s\",\"endpoint_packets\":%u,\"adjusted_rtts\":%u}\n",
           client ? "tx" : "rx", format, (unsigned int)__builtin_popcount(test.seen), test.adjusted_rtts);
}

static uint16_t observe_counted_endpoint(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                          uint16_t count, uint16_t capacity, void *argument)
{
    (void)queue;
    (void)capacity;
    unsigned int *seen = argument;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t prefix[14 + 40 + WIRE_UDP_LEN + WIRE_BTH_LEN + EP_REQUEST_HEADER_LEN];
        struct observed_udp udp = read_udp(packets[i], prefix, sizeof(prefix));
        const uint8_t *bth = udp.header + WIRE_UDP_LEN;
        assert(port == COUNTED_RAILS - 1 && bth[0] == EP_REQUEST_OPCODE && wire_be24(bth + 5) == 2);
        assert(udp.bytes >= WIRE_UDP_LEN + WIRE_BTH_LEN + EP_REQUEST_HEADER_LEN);
        const uint8_t *erth = bth + WIRE_BTH_LEN;
        uint32_t token = wire_be24(bth + 9);
        if (token == COUNTED_EP_PROBE) {
            assert(!(*seen & 1) && erth[1] == 1 && (udp.traffic_class >> 2) == EP_PROBE_DSCP);
            *seen |= 1;
        } else {
            assert(token == COUNTED_EP_STATUS && !(*seen & 2) && erth[1] == 0);
            assert(wire_be32(erth + 4) == (COUNTED_EP_MASK >> 1) && (udp.traffic_class >> 2) == EP_CONTROL_DSCP);
            *seen |= 2;
        }
    }
    return count;
}

static void run_counted_endpoint(bool client, const char *directory,
                                  struct mrc_context *context, struct rte_mempool *pool)
{
    struct mrc_dpdk_rail rails[COUNTED_RAILS];
    for (uint16_t i = 0; i < COUNTED_RAILS; i++)
        rails[i] = (struct mrc_dpdk_rail){ .port = i, .peer_mac = { 2, 0, 0, 0, 0, (client ? 0x10 : 0x20) + i } };
    struct mrc_dpdk_endpoint_route_config route = {
        .rails = rails, .rail_count = 32, .pool = pool, .udp_port = EP_UDP_PORT,
    };
    assert(inet_pton(AF_INET6, client ? "::ffff:10.9.0.1" : "::ffff:10.9.0.2", route.local_gid.raw) == 1);
    assert(inet_pton(AF_INET6, client ? "::ffff:10.9.0.2" : "::ffff:10.9.0.1", route.peer_gid.raw) == 1);
    assert(mrc_dpdk_attach_endpoint_rails(context, &route) == EINVAL);
    route.rail_count = COUNTED_RAILS;
    assert(mrc_dpdk_attach_endpoint_rails(context, &route) == 0);
    prepare_memif_rails(COUNTED_RAILS);

    char marker[PATH_MAX];
    if (client) {
        path_for(marker, directory, "rx.endpoint-ready");
        wait_for(marker);
        struct mrc_ctl_ep_req request = {
            .req_id = COUNTED_EP_PROBE, .sgid = route.local_gid, .dgid = route.peer_gid,
            .ev_fmt_mode = MRC_CTL_EV_FMT_MODE_UDP,
            .req_ev = { .port = COUNTED_RAILS + 1, .val = { 0, 0, 0xd1, 0x08 } },
        };
        struct mrc_ctl_ep_rsp reply;
        int returned;
        assert(mrc_ctl_ep_batch_send_wait(context, EP_PROBE_DSCP, MRC_CTL_EP_OP_EV_PROBE,
                                         &request, 1, 1000000000, &reply, &returned) == -1);
        assert(errno == EINVAL && returned == 0);
        request.req_ev.port = COUNTED_RAILS;
        assert(mrc_ctl_ep_batch_send_wait(context, EP_PROBE_DSCP, MRC_CTL_EP_OP_EV_PROBE,
                                         &request, 1, 1000000000, &reply, &returned) == 0);
        assert(returned == 1 && reply.req_id == COUNTED_EP_PROBE && reply.port == COUNTED_RAILS);
        request.req_id = COUNTED_EP_STATUS;
        request.op.port_status_update.port_status = COUNTED_EP_MASK << 1;
        assert(mrc_ctl_ep_batch_send_wait(context, 0, MRC_CTL_EP_OP_PORT_STATUS_UPDATE,
                                         &request, 1, 1000000000, &reply, &returned) == -1);
        assert(errno == EINVAL && returned == 0);
        request.op.port_status_update.port_status = COUNTED_EP_MASK;
        assert(mrc_ctl_ep_batch_send_wait(context, 0, MRC_CTL_EP_OP_PORT_STATUS_UPDATE,
                                         &request, 1, 1000000000, &reply, &returned) == 0);
        assert(returned == 1 && reply.req_id == COUNTED_EP_STATUS && reply.port == COUNTED_RAILS);
        publish(directory, "tx.endpoint-done", NULL, 0);
    } else {
        const uint16_t port = COUNTED_RAILS - 1;
        unsigned int seen = 0;
        const struct rte_eth_rxtx_callback *callback = rte_eth_add_rx_callback(port, 0, observe_counted_endpoint, &seen);
        assert(callback);
        path_for(marker, directory, "tx.endpoint-done");
        publish(directory, "rx.endpoint-ready", NULL, 0);
        uint64_t deadline = rte_get_timer_cycles() + 4 * rte_get_timer_hz();
        while (access(marker, F_OK) != 0) {
            assert(rte_get_timer_cycles() < deadline);
            assert(mrc_dpdk_progress_endpoint(context) == 0);
        }
        assert(seen == 3 && rte_eth_remove_rx_callback(port, 0, callback) == 0);
        rte_free((void *)callback);
    }
    assert(mrc_dpdk_detach_endpoint(context) == 0);
    printf("{\"role\":\"%s\",\"format\":\"counted\",\"endpoint_packets\":2,\"port\":%u}\n",
           client ? "tx" : "rx", COUNTED_RAILS);
}

static void attach_srv6_qp(struct mrc_qp *qp, const struct mrc_dpdk_route *route,
                           struct mrc_dpdk_srv6_path paths[DUPLEX_RAILS], bool transit)
{
    const uint8_t local = route->requester ? 0x11 : 0x22;
    const uint8_t peer = route->requester ? 0x22 : 0x11;
    for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
        test_sid(paths[port].local_sid, port, local);
        test_sid(paths[port].peer_sid, port, peer);
    }
    memcpy(paths[1].srh_segment, paths[1].peer_sid, sizeof(paths[1].srh_segment));
    if (route->requester && transit) {
        test_sid(paths[1].initial_dst, 1, 0x33);
        paths[1].segments_left = 1;
    }

    struct mrc_dpdk_srv6_path invalid[DUPLEX_RAILS];
    memcpy(invalid, paths, sizeof(invalid));
    memset(invalid[0].local_sid, 0, sizeof(invalid[0].local_sid));
    assert(mrc_dpdk_attach_srv6_qp(qp, route, invalid) == EINVAL);
    memcpy(invalid, paths, sizeof(invalid));
    invalid[0].segments_left = 1;
    assert(mrc_dpdk_attach_srv6_qp(qp, route, invalid) == EINVAL);

    int attached = mrc_dpdk_attach_srv6_qp(qp, route, paths);
    if (attached) {
        fprintf(stderr, "Failed to attach SRv6 QP: %s (%d)\n", strerror(attached), attached);
        abort();
    }
}

struct simplex_wire_witness {
    bool requester;
    enum simplex_wire_mode mode;
    const struct connection *local;
    const struct connection *peer;
    const struct mrc_dpdk_srv6_path *srv6;
    uint32_t entropy[DUPLEX_RAILS];
    unsigned int probe_ports;
    unsigned int data_ports;
    unsigned int decapped_ports;
    unsigned int advanced_ports;
    unsigned int probe_evs;
    unsigned int data_evs;
    uint16_t first_probe_token;
    bool wrong_sid_injected;
};

enum srv6_ingress_action {
    SRV6_PRESERVE,
    SRV6_DECAPSULATE,
    SRV6_ADVANCE,
};

struct srv6_wire_path {
    uint8_t destination[16];
    uint8_t segment[16];
    bool has_srh;
    enum srv6_ingress_action action;
};

static struct srv6_wire_path expected_srv6_path(const struct simplex_wire_witness *observed,
                                                 uint16_t port)
{
    const uint8_t *local = observed->srv6[port].local_sid;
    struct srv6_wire_path path = {.has_srh = port == 1};
    memcpy(path.destination, local, sizeof(path.destination));
    memcpy(path.segment, local, sizeof(path.segment));

    if (observed->requester) {
        /* Replies use the responder's own route, independent of the request profile. */
        path.action = port == 1 ? SRV6_DECAPSULATE : SRV6_PRESERVE;
    } else if (observed->mode == SIMPLEX_SRV6_EXPLICIT_EV ||
               observed->mode == SIMPLEX_SRV6_GENERATED_EV ||
               observed->mode == SIMPLEX_SRV6_MAPPED_EV) {
        /* The controller fixture seeds these containers. Attach inputs specify
         * different destinations and an SRH only on rail 1. */
        test_sid(path.destination, 0, port + 1);
        test_sid(path.segment, 1, 0x42);
        path.has_srh = observed->mode == SIMPLEX_SRV6_GENERATED_EV;
        path.action = SRV6_ADVANCE;
    } else if (observed->mode == SIMPLEX_SRV6_TRANSIT) {
        if (port == 1) {
            test_sid(path.destination, port, 0x33);
            path.action = SRV6_ADVANCE;
        }
    } else {
        path.action = SRV6_DECAPSULATE;
    }
    return path;
}

static struct observed_udp read_srv6_udp(struct rte_mbuf *packet, uint8_t *prefix, size_t capacity,
                                          const struct mrc_dpdk_srv6_path *endpoint,
                                          const struct srv6_wire_path *path,
                                          const struct simplex_wire_witness *observed)
{
    const size_t packet_length = rte_pktmbuf_pkt_len(packet);
    const size_t length = packet_length < capacity ? packet_length : capacity;
    const size_t encapsulation = WIRE_IPV6_LEN + (path->has_srh ? WIRE_SINGLE_SRH_LEN : 0);
    const size_t udp_offset = RTE_ETHER_HDR_LEN + encapsulation + WIRE_IPV6_LEN;
    assert(length >= udp_offset + WIRE_UDP_LEN + WIRE_BTH_LEN);
    const uint8_t *frame = rte_pktmbuf_read(packet, 0, length, prefix);
    assert(frame && wire_be16(frame + 12) == RTE_ETHER_TYPE_IPV6);
    const uint8_t *outer = frame + RTE_ETHER_HDR_LEN;
    assert((outer[0] >> 4) == 6 && outer[7] > 0);
    assert(wire_be16(outer + 4) == packet_length - RTE_ETHER_HDR_LEN - WIRE_IPV6_LEN);
    assert(memcmp(outer + 8, endpoint->peer_sid, 16) == 0);
    assert(memcmp(outer + 24, path->destination, 16) == 0);
    if (!path->has_srh) {
        assert(outer[6] == IPPROTO_IPV6);
    } else {
        const uint8_t *srh = outer + WIRE_IPV6_LEN;
        assert(outer[6] == IPPROTO_ROUTING);
        assert(srh[0] == IPPROTO_IPV6 && srh[1] == 2 && srh[2] == 4 &&
               srh[3] == (path->action == SRV6_ADVANCE) && srh[4] == 0);
        assert(memcmp(srh + 8, path->segment, 16) == 0);
    }

    const uint8_t *inner = outer + encapsulation;
    const uint8_t *udp = inner + WIRE_IPV6_LEN;
    assert((inner[0] >> 4) == 6 && inner[6] == IPPROTO_UDP);
    assert(wire_be16(inner + 4) == packet_length - udp_offset);
    assert(wire_be16(udp + 4) == packet_length - udp_offset);
    assert(memcmp(inner + 8, observed->peer->gid.raw, 16) == 0);
    assert(memcmp(inner + 24, observed->local->gid.raw, 16) == 0);
    uint32_t entropy = (uint32_t)wire_be16(inner + 2) << 16 | wire_be16(udp);
    return (struct observed_udp){.header = udp, .bytes = length - udp_offset, .entropy = entropy};
}

static void finish_srv6_receive(struct rte_mbuf *packet, uint16_t port, const struct srv6_wire_path *path,
                                 struct simplex_wire_witness *observed)
{
    if (path->action == SRV6_PRESERVE)
        return;
    if (path->action == SRV6_ADVANCE) {
        /* Emulate the fabric's terminal delivery before the public RX path. */
        uint8_t *outer = rte_pktmbuf_mtod_offset(packet, uint8_t *, RTE_ETHER_HDR_LEN);
        memcpy(outer + 24, observed->srv6[port].local_sid, 16);
        if (path->has_srh)
            outer[WIRE_IPV6_LEN + 3] = 0;
        observed->advanced_ports |= 1u << port;
        return;
    }

    /* Independently remove the final SRH and outer IPv6 header while
     * preserving Ethernet and the intact inner iCRC-protected packet. */
    struct rte_ether_hdr ethernet;
    memcpy(&ethernet, rte_pktmbuf_mtod(packet, const uint8_t *), sizeof(ethernet));
    uint16_t envelope = WIRE_IPV6_LEN + (path->has_srh ? WIRE_SINGLE_SRH_LEN : 0);
    char *inner_frame = rte_pktmbuf_adj(packet, envelope);
    assert(inner_frame);
    memcpy(inner_frame, &ethernet, sizeof(ethernet));
    observed->decapped_ports |= 1u << port;
}

static void observe_simplex_header(struct simplex_wire_witness *observed, uint16_t port,
                                    const struct observed_udp *udp)
{
    const uint8_t *bth = udp->header + WIRE_UDP_LEN;
    unsigned int bit = 1u << port;
    uint32_t entropy = udp->entropy;
    bool probe;
    if (observed->requester) {
        if (bth[0] != WIRE_SACK)
            return;
        assert(udp->bytes >= WIRE_UDP_LEN + WIRE_BTH_LEN + WIRE_SETH_LEN);
        const uint8_t *seth = bth + WIRE_BTH_LEN;
        probe = (seth[1] & 0x02) != 0;
        entropy = wire_be32(seth + 4);
        if (observed->srv6 && probe && port == 0) {
            uint16_t token = wire_be16(seth + 2);
            if (observed->probe_ports & bit)
                assert(token != observed->first_probe_token);
            else
                observed->first_probe_token = token;
        }
    } else {
        probe = bth[0] == WIRE_PROBE_REQUEST;
        if (!probe && (bth[0] < WIRE_WRITE_FIRST || bth[0] > 0xcb))
            return;
    }
    assert(wire_be24(bth + 5) == observed->local->qpn);
    if (observed->mode == SIMPLEX_SRV6_MAPPED_EV) {
        const unsigned int entry = (uint16_t)entropy - 0xc000u;
        assert(entry < 4 && entry != 2 && port == 1 - entry % 2);
        assert((entropy >> 16) == 0x100 + entry);
        if (probe)
            observed->probe_evs |= 1u << entry;
        else
            observed->data_evs |= 1u << entry;
    } else if ((observed->probe_ports | observed->data_ports) & bit) {
        assert(observed->entropy[port] == entropy);
    } else {
        observed->entropy[port] = entropy;
    }
    if (probe)
        observed->probe_ports |= bit;
    else
        observed->data_ports |= bit;
}

static uint16_t observe_simplex_wire(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                     uint16_t count, uint16_t capacity, void *argument)
{
    (void)queue;
    struct simplex_wire_witness *observed = argument;
    struct srv6_wire_path path;
    if (observed->srv6)
        path = expected_srv6_path(observed, port);
    struct rte_mbuf *foreign = NULL;
    for (uint16_t i = 0; i < count; i++) {
        if (!observed->requester && observed->mode == SIMPLEX_SRV6_MAPPED_EV) {
            uint8_t suffix;
            const uint8_t *last = rte_pktmbuf_read(packets[i], RTE_ETHER_HDR_LEN + 39, 1, &suffix);
            assert(last && *last >= 1 && *last <= 4 && *last != 3 && port == 1 - (*last - 1) % 2);
            test_sid(path.destination, 0, *last);
        }
        uint8_t prefix[RTE_ETHER_HDR_LEN + 2 * WIRE_IPV6_LEN + WIRE_SINGLE_SRH_LEN +
                       WIRE_UDP_LEN + WIRE_BTH_LEN + WIRE_SETH_LEN];
        struct observed_udp udp = observed->srv6 ?
            read_srv6_udp(packets[i], prefix, sizeof(prefix), &observed->srv6[port], &path, observed) :
            read_udp(packets[i], prefix, sizeof(prefix));
        observe_simplex_header(observed, port, &udp);
        if (!observed->srv6)
            continue;

        const uint8_t *bth = udp.header + WIRE_UDP_LEN;
        if (!observed->requester && port == 0 && bth[0] == WIRE_PROBE_REQUEST &&
            !observed->wrong_sid_injected && count < capacity) {
            /* Keep the inner packet and iCRC unchanged. A correct endpoint
             * replies once even after seeing this other rail's SID. */
            foreign = rte_pktmbuf_copy(packets[i], packets[i]->pool, 0, UINT32_MAX);
            assert(foreign);
            uint8_t *outer = rte_pktmbuf_mtod_offset(foreign, uint8_t *, RTE_ETHER_HDR_LEN);
            memcpy(outer + 24, observed->srv6[1].local_sid, 16);
            if (path.has_srh)
                outer[WIRE_IPV6_LEN + 3] = 0;
            observed->wrong_sid_injected = true;
        }
        finish_srv6_receive(packets[i], port, &path, observed);
    }
    if (foreign) {
        memmove(packets + 1, packets, count * sizeof(*packets));
        packets[0] = foreign;
        count++;
    }
    return count;
}

struct inline_wire_witness {
    bool dropped;
    bool retransmitted;
};

static uint16_t drop_inline_fragment(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                     uint16_t count, uint16_t capacity, void *argument)
{
    (void)port;
    (void)queue;
    (void)capacity;
    struct inline_wire_witness *observed = argument;
    uint16_t kept = 0;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t prefix[RTE_ETHER_HDR_LEN + WIRE_IPV6_LEN + WIRE_UDP_LEN + WIRE_BTH_LEN];
        struct observed_udp udp = read_udp(packets[i], prefix, sizeof(prefix));
        const uint8_t *bth = udp.header + WIRE_UDP_LEN;
        if (bth[0] == WIRE_WRITE_FIRST && wire_be24(bth + 9) == SEND_PSN + 1) {
            if (!observed->dropped) {
                assert(!(bth[8] & 0x20));
                rte_pktmbuf_free(packets[i]);
                observed->dropped = true;
                continue;
            }
            assert(bth[8] & 0x20);
            observed->retransmitted = true;
        }
        packets[kept++] = packets[i];
    }
    return kept;
}

struct pmtu_wire_witness {
    uint64_t address;
    unsigned int pmtu;
    unsigned int fragments;
    uint32_t seen;
};

static uint16_t observe_pmtu_write(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                  uint16_t count, uint16_t capacity, void *argument)
{
    (void)port;
    (void)queue;
    (void)capacity;
    struct pmtu_wire_witness *observed = argument;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t prefix[RTE_ETHER_HDR_LEN + WIRE_IPV6_LEN + WIRE_UDP_LEN + WIRE_BTH_LEN +
                       WIRE_METH_LEN + WIRE_TSETH_LEN + WIRE_RETH_LEN];
        struct observed_udp udp = read_udp(packets[i], prefix, sizeof(prefix));
        const uint8_t *bth = udp.header + WIRE_UDP_LEN;
        if (bth[0] < WIRE_WRITE_FIRST || bth[0] > WIRE_WRITE_ONLY_IMM)
            continue;
        /* The large WRITEIMM starts after the short WRITE, then wraps through PSN 0. */
        uint32_t fragment = (wire_be24(bth + 9) - (SEND_PSN + 1)) & 0xffffff;
        if (fragment >= observed->fragments)
            continue;
        bool last = fragment + 1 == observed->fragments;
        unsigned int offset = fragment * observed->pmtu;
        unsigned int payload = last ? IMMEDIATE_BYTES - offset : observed->pmtu;
        assert(bth[0] == (last ? WIRE_WRITE_LAST_IMM : fragment ? WIRE_WRITE_MIDDLE : WIRE_WRITE_FIRST));
        unsigned int timestamp = bth[8] & 0x10 ? WIRE_TSETH_LEN : 0;
        const uint8_t *reth = bth + WIRE_BTH_LEN + WIRE_METH_LEN + timestamp;
        assert(udp.bytes >= (size_t)(reth + WIRE_RETH_LEN - udp.header));
        uint64_t address = (uint64_t)wire_be32(reth) << 32 | wire_be32(reth + 4);
        assert(address == observed->address + offset && wire_be32(reth + 12) == IMMEDIATE_BYTES);
        unsigned int header = WIRE_UDP_LEN + WIRE_BTH_LEN + WIRE_METH_LEN + timestamp + WIRE_RETH_LEN +
                              (last ? WIRE_IMM_LEN : 0);
        unsigned int pad = (bth[1] >> 4) & 3;
        assert(wire_be16(udp.header + 4) == header + payload + pad + WIRE_ICRC_LEN);
        observed->seen |= UINT32_C(1) << fragment;
    }
    return count;
}

static void check_jumbo_attachment(struct mrc_qp *qp, const struct mrc_dpdk_route *route, enum ibv_mtu restore)
{
    struct ibv_qp_attr verbs = { .path_mtu = IBV_MTU_4096 };
    assert(mrc_modify_qp(qp, &verbs, IBV_QP_PATH_MTU, NULL, 0) == 0);
    assert(mrc_dpdk_attach_qp(qp, route) == EMSGSIZE);

    struct mrc_dpdk_route jumbo = *route;
    jumbo.pool = rte_pktmbuf_pool_create("jumbo_attach_test", 31, 0, 0,
                                         RTE_PKTMBUF_HEADROOM + 4608, SOCKET_ID_ANY);
    assert(jumbo.pool);
    assert(mrc_dpdk_attach_qp(qp, &jumbo) == EMSGSIZE);
    rte_mempool_free(jumbo.pool);
    assert(mrc_dpdk_progress_qp(qp) == ENOTCONN);

    verbs.path_mtu = restore;
    assert(mrc_modify_qp(qp, &verbs, IBV_QP_PATH_MTU, NULL, 0) == 0);
}

static void run_simplex(bool requester, const char *directory, const struct address *address,
                       struct mrc_context *context, struct ibv_pd *pd, struct rte_mempool *pool,
                       enum simplex_error error_mode, const char *ev_store, uint64_t ev_profile_id,
                       enum simplex_wire_mode wire_mode)
{
    const bool wire_error = error_mode != SIMPLEX_NO_ERROR;
    const bool cq_overrun = error_mode == SIMPLEX_CQ_OVERRUN;
    const bool rejected_write = error_mode == SIMPLEX_REQUEST_ERROR ||
                                error_mode == SIMPLEX_ACCESS_ERROR;
    const bool inline_mode = wire_mode == SIMPLEX_INLINE;
    const bool generated_ev = wire_mode == SIMPLEX_GENERATED_EV;
    const bool mapped_srv6 = wire_mode == SIMPLEX_SRV6_MAPPED_EV;
    const bool srv6_ev = wire_mode == SIMPLEX_SRV6_EXPLICIT_EV ||
                         wire_mode == SIMPLEX_SRV6_GENERATED_EV || mapped_srv6;
    const bool srv6 = srv6_ev || wire_mode == SIMPLEX_SRV6 || wire_mode == SIMPLEX_SRV6_TRANSIT;
    const bool transit = wire_mode == SIMPLEX_SRV6_TRANSIT;
    const unsigned int pmtu = wire_mode == SIMPLEX_PMTU_256 ? 256 : wire_mode == SIMPLEX_PMTU_512 ? 512 : 0;
    const enum ibv_mtu path_mtu = pmtu == 256 ? IBV_MTU_256 : pmtu == 512 ? IBV_MTU_512 : IBV_MTU_1024;
    const bool inspect_wire = wire_mode != SIMPLEX_WIRE_NONE && !inline_mode && !pmtu;
    if (requester && ev_profile_id)
        assert(mrc_dpdk_set_ev_store(context, ev_store) == 0);
    struct mrc_comp_channel *channel = NULL;
    if (!requester && !wire_error)
        assert(mrc_create_comp_channel(context, &channel) == 0);
    struct mrc_cq *cq = mrc_create_cq(context, !requester && cq_overrun ? 1 : 4, NULL, channel, 0);
    assert(cq);

    uint8_t first_data[TOTAL_BYTES], last_data[TOTAL_BYTES];
    uint8_t recv_buffer = 0x5a;
    int access_flags = 0;
    if (!requester) {
        access_flags = IBV_ACCESS_LOCAL_WRITE;
        if (error_mode != SIMPLEX_ACCESS_ERROR)
            access_flags |= IBV_ACCESS_REMOTE_WRITE;
    }
    size_t first_length = requester ? FIRST_BYTES + IMMEDIATE_FIRST_SGE_BYTES : TOTAL_BYTES;
    struct ibv_mr *first = NULL;
    struct ibv_mr *last = NULL;
    struct ibv_mr *receive = NULL;
    if (!requester || !inline_mode) {
        for (size_t i = 0; i < first_length; i++)
            first_data[i] = requester ? pattern(i) : 0;
        first = register_memory(context, pd, first_data, first_length, access_flags);
    }
    if (requester && !inline_mode && !rejected_write) {
        size_t length = TOTAL_BYTES - first_length;
        for (size_t i = 0; i < length; i++)
            last_data[i] = pattern(first_length + i);
        last = register_memory(context, pd, last_data, length, 0);
    }
    if (!requester)
        receive = register_memory(context, pd, &recv_buffer, sizeof(recv_buffer), IBV_ACCESS_LOCAL_WRITE);

    struct mrc_qp_init_attr init = {
        .pd = pd, .protocol_version = MRC_PROTOCOL_VERSION_1,
        .send_cq = requester ? cq : NULL, .recv_cq = requester ? NULL : cq,
        .send_ops_flags = requester ? IBV_QP_EX_WITH_RDMA_WRITE | IBV_QP_EX_WITH_RDMA_WRITE_WITH_IMM : 0,
        .cap = {
            .max_send_wr = requester ? 3 : 0,
            .max_send_sge = requester ? 2 : 0,
            .max_recv_wr = requester ? 0 : 2,
            .max_recv_sge = requester ? 0 : 1,
            .max_inline_data = requester && inline_mode ? IMMEDIATE_BYTES : 0,
        },
    };
    if (!requester)
        skip_local_qp_numbers(context, &init, 1);
    struct mrc_qp *qp = mrc_create_qp(context, &init);
    struct connection local = {
        .gid = address->gid, .psn = requester ? SEND_PSN : 135,
        .remote_address = requester ? 0 : (uintptr_t)first_data,
        .rkey = requester ? 0 : first->rkey,
    };
    assert(qp && mrc_get_qpn(qp, &local.qpn) == 0);
    if (requester && inline_mode) {
        struct ibv_qp_attr attrs;
        assert(mrc_query_qp(qp, &attrs, IBV_QP_CAP, NULL, 0, NULL) == 0);
        assert(attrs.cap.max_inline_data == IMMEDIATE_BYTES);
    }
    publish(directory, requester ? "tx.connection" : "rx.connection", &local, sizeof(local));
    struct connection peer;
    read_peer_connections(directory, requester, &peer, 1);
    assert(local.qpn != peer.qpn);
    configure_qp(qp, address, &peer, local.psn, requester, !requester,
                 requester ? ev_profile_id : 0, 0, 0, 0, path_mtu);
    if (pmtu) {
        struct ibv_qp_attr verbs;
        assert(mrc_query_qp(qp, &verbs, IBV_QP_PATH_MTU, NULL, 0, NULL) == 0);
        assert(verbs.path_mtu == path_mtu);
    }
    if (!requester && error_mode == SIMPLEX_REQUEST_ERROR) {
        /* The peer offers one WRITEIMM at a time. This responder has
         * no slots for immediate data, exercising an OOB mismatch. */
        struct mrc_qp_attr attrs = { .wimm.max_wimm_dest = 0 };
        assert(mrc_modify_qp(qp, NULL, 0, &attrs, MRC_QP_MAX_WIMM_DEST) == 0);
    }

    struct mrc_dpdk_route route = path_route(pool, requester);
    route.requester = requester;
    if (pmtu == 256 && !requester)
        check_jumbo_attachment(qp, &route, path_mtu);
    struct mrc_dpdk_srv6_path paths[DUPLEX_RAILS] = {0};
    if (requester && srv6_ev)
        assert(mrc_dpdk_attach_qp(qp, &route) == EINVAL);
    if (srv6)
        attach_srv6_qp(qp, &route, paths, transit);
    else
        attach_qp(qp, &route);

    struct simplex_wire_witness observed = {
        .requester = requester, .mode = wire_mode, .local = &local, .peer = &peer,
        .srv6 = srv6 ? paths : NULL,
    };
    struct inline_wire_witness observed_inline = {0};
    struct pmtu_wire_witness observed_pmtu = {
        .address = local.remote_address + FIRST_BYTES,
        .pmtu = pmtu,
        .fragments = pmtu ? (IMMEDIATE_BYTES + pmtu - 1) / pmtu : 0,
    };
    const struct rte_eth_rxtx_callback *callbacks[DUPLEX_RAILS] = {0};
    if (wire_mode != SIMPLEX_WIRE_NONE) {
        prepare_memif_links();
        if (requester) {
            publish(directory, "tx.links-ready", NULL, 0);
        } else {
            char ready[PATH_MAX];
            path_for(ready, directory, "tx.links-ready");
            wait_for(ready);
        }
        for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
            if (inline_mode && !requester)
                callbacks[port] = rte_eth_add_rx_callback(port, 0, drop_inline_fragment, &observed_inline);
            else if (pmtu && !requester)
                callbacks[port] = rte_eth_add_rx_callback(port, 0, observe_pmtu_write, &observed_pmtu);
            else if (inspect_wire)
                callbacks[port] = rte_eth_add_rx_callback(port, 0, observe_simplex_wire, &observed);
            else
                continue;
            assert(callbacks[port]);
        }
    }

    if (requester) {
        char ready[PATH_MAX];
        path_for(ready, directory, "rx.ready");
        wait_for(ready);
        if (rejected_write)
            post_single_write_imm(qp, &peer, first, 0);
        else if (inline_mode)
            post_inline_writes(qp, &peer);
        else
            post_registered_writes(qp, &peer, first, last);
    } else {
        struct ibv_sge receive_sge = {
            .addr = (uintptr_t)&recv_buffer, .length = sizeof(recv_buffer), .lkey = receive->lkey,
        };
        post_receives(qp, &receive_sge);
        if (!wire_error)
            assert(mrc_req_notify_cq(cq, 1) == 0);
        publish(directory, "rx.ready", NULL, 0);
    }
    if (ev_profile_id && !generated_ev && wire_mode != SIMPLEX_SRV6_GENERATED_EV && !mapped_srv6)
        check_blocked_profile(qp, cq, ev_store, requester, first_data);
    if (wire_error)
        complete_async_error(context, qp, cq, requester, local.qpn, error_mode);
    else
        complete(qp, cq, channel, directory, requester, local.qpn);
    if (inspect_wire) {
        const unsigned int both_rails = (1u << DUPLEX_RAILS) - 1;
        assert(observed.probe_ports == both_rails && observed.data_ports);
        if (!requester)
            assert(observed.data_ports == both_rails);
        if (srv6) {
            /* The final case can only complete its WRITEs after the
             * responder accepts independently decapsulated traffic. */
            unsigned int decapped_ports = requester ? 1u << 1 : (wire_mode == SIMPLEX_SRV6 ? both_rails : 0);
            unsigned int advanced_ports = requester ? 0 : (srv6_ev ? both_rails : transit ? 1u << 1 : 0);
            assert(observed.decapped_ports == decapped_ports);
            assert(observed.advanced_ports == advanced_ports);
            assert(observed.wrong_sid_injected == !requester);
        }
        if (mapped_srv6)
            assert(observed.probe_evs == 0xb && (requester || observed.data_evs == 0xb));
    }
    if (inline_mode && !requester)
        assert(observed_inline.dropped && observed_inline.retransmitted);
    if (pmtu && !requester)
        assert(observed_pmtu.seen == (UINT32_C(1) << observed_pmtu.fragments) - 1);
    for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
        if (!callbacks[port])
            continue;
        assert(rte_eth_remove_rx_callback(port, 0, callbacks[port]) == 0);
        rte_free((void *)callbacks[port]);
    }
    if (requester) {
        publish(directory, "tx.done", NULL, 0);
    } else {
        for (size_t i = 0; i < TOTAL_BYTES; i++)
            assert(first_data[i] == (rejected_write ? 0 : pattern(i)));
        assert(recv_buffer == 0x5a);
        if (wire_error) {
            char done[PATH_MAX];
            path_for(done, directory, "tx.done");
            wait_for(done);
        } else {
            struct pollfd wait = { .fd = completion_fd(channel), .events = POLLIN };
            assert(poll(&wait, 1, 0) == 0);
        }
    }

    assert(mrc_destroy_qp(qp) == 0);
    assert(mrc_destroy_cq(cq) == 0);
    if (channel)
        assert(mrc_destroy_comp_channel(channel) == 0);
    if (last)
        deregister_memory(context, last);
    if (first)
        deregister_memory(context, first);
    if (receive)
        deregister_memory(context, receive);
    if (cq_overrun) {
        printf("{\"role\":\"%s\",\"qp_fatal\":true,\"cq_error\":%s,\"qpn\":%u}\n",
               requester ? "tx" : "rx", requester ? "false" : "true", local.qpn);
    } else if (rejected_write) {
        const bool invalid = error_mode == SIMPLEX_REQUEST_ERROR;
        const char *event = requester ? "QP_FATAL" : invalid ? "QP_REQ_ERR" : "QP_ACCESS_ERR";
        const char *completion = requester ? (invalid ? "REM_INV_REQ_ERR" : "REM_ACCESS_ERR") : "WR_FLUSH_ERR";
        printf("{\"role\":\"%s\",\"qp_event\":\"%s\",\"completion\":\"%s\","
               "\"completions\":%u,\"qpn\":%u}\n",
               requester ? "tx" : "rx", event, completion, requester ? 1u : 2u, local.qpn);
    } else {
        printf("{\"role\":\"%s\",\"bytes\":%u,\"completions\":2,\"qpn\":%u",
               requester ? "tx" : "rx", (unsigned int)TOTAL_BYTES, local.qpn);
        if (generated_ev)
            printf(",\"rail_entropy\":[%u,%u]", observed.entropy[0], observed.entropy[1]);
        if (srv6)
            printf(",\"srv6_probe_rails\":%u,\"srv6_decapped_rails\":%u,"
                   "\"srv6_advanced_rails\":%u,\"srv6_wrong_sid_probes\":%u",
                   (unsigned int)__builtin_popcount(observed.probe_ports),
                   (unsigned int)__builtin_popcount(observed.decapped_ports),
                   (unsigned int)__builtin_popcount(observed.advanced_ports),
                   (unsigned int)observed.wrong_sid_injected);
        if (mapped_srv6)
            printf(",\"write_evs\":%u,\"probe_evs\":%u", observed.data_evs, observed.probe_evs);
        else if (srv6_ev)
            printf(",\"rail_entropy\":[%u,%u]", observed.entropy[0], observed.entropy[1]);
        if (inline_mode && !requester)
            printf(",\"inline_fragment_retransmitted\":%s", observed_inline.retransmitted ? "true" : "false");
        if (pmtu)
            printf(",\"pmtu\":%u", pmtu);
        if (pmtu && !requester)
            printf(",\"writeimm_fragments\":%u", (unsigned int)__builtin_popcount(observed_pmtu.seen));
        puts("}");
    }
}

struct wimm_capacity_gate {
    bool released;
    unsigned int held_acks;
    struct rte_mbuf *latest[2];
};

static uint16_t hold_wimm_acks(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                               uint16_t count, void *argument)
{
    (void)queue;
    struct wimm_capacity_gate *gate = argument;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t prefix[14 + 40 + WIRE_UDP_LEN + WIRE_BTH_LEN];
        struct observed_udp udp = read_udp(packets[i], prefix, sizeof(prefix));
        if (!gate->released && udp.header[WIRE_UDP_LEN] == WIRE_TRANSPORT_ACK) {
            assert(port < 2);
            if (gate->latest[port])
                rte_pktmbuf_free(gate->latest[port]);
            gate->latest[port] = rte_pktmbuf_copy(packets[i], packets[i]->pool, 0, UINT32_MAX);
            assert(gate->latest[port]);
            gate->held_acks++;
            /* Deliver a corrupt copy so TX succeeds while the requester cannot
             * consume a semantic credit. Keep the valid cumulative ACK. */
            assert(rte_pktmbuf_is_contiguous(packets[i]));
            uint8_t *bytes = rte_pktmbuf_mtod(packets[i], uint8_t *);
            bytes[rte_pktmbuf_pkt_len(packets[i]) - 1] ^= 1;
        }
    }
    return count;
}

static void run_boundaries(bool requester, const char *directory, const struct address *address,
                           struct mrc_context *context, struct ibv_pd *pd, struct rte_mempool *pool,
                           unsigned int pmtu, bool capacity)
{
    const uint32_t boundary_lengths[] = {0, 1, 2, 3, 4, pmtu - 1, pmtu, pmtu + 1, 2 * pmtu, 2 * pmtu + 1};
    const unsigned int rounds = capacity ? 1025 : 10;
    uint32_t lengths[rounds];
    for (unsigned int i = 0; i < rounds; i++)
        lengths[i] = capacity ? 1 : boundary_lengths[i];
    size_t bytes = 0;
    for (unsigned int i = 0; i < rounds; i++)
        bytes += lengths[i];
    uint8_t *allocation = calloc(bytes + 2, 1);
    assert(allocation);
    allocation[0] = 0xa5;
    allocation[bytes + 1] = 0x5a;
    uint8_t *data = allocation + 1;
    if (requester)
        for (size_t i = 0; i < bytes; i++)
            data[i] = pattern(i);
    struct ibv_mr *mr = register_memory(context, pd, data, bytes,
        requester ? 0 : IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    struct mrc_cq *cq = mrc_create_cq(context, rounds, NULL, NULL, 0);
    assert(cq);
    struct mrc_qp_init_attr init = {
        .pd = pd, .send_cq = requester ? cq : NULL, .recv_cq = requester ? NULL : cq,
        .send_ops_flags = requester ? IBV_QP_EX_WITH_RDMA_WRITE_WITH_IMM : 0,
        .cap = {.max_send_wr = requester ? rounds : 0, .max_send_sge = requester,
                .max_recv_wr = requester ? 0 : rounds},
    };
    if (!requester)
        skip_local_qp_numbers(context, &init, 1);
    struct mrc_qp *qp = mrc_create_qp(context, &init);
    struct connection local = {.gid = address->gid, .psn = SEND_PSN,
                                .remote_address = (uintptr_t)data, .rkey = mr->rkey};
    assert(qp && mrc_get_qpn(qp, &local.qpn) == 0);
    publish(directory, requester ? "tx.connection" : "rx.connection", &local, sizeof(local));
    struct connection peer;
    read_peer_connections(directory, requester, &peer, 1);
    enum ibv_mtu mtu = pmtu == 256 ? IBV_MTU_256 : pmtu == 512 ? IBV_MTU_512 : IBV_MTU_1024;
    configure_qp(qp, address, &peer, local.psn, requester, !requester, 0, 0, 0, 0, mtu);
    if (capacity) {
        struct mrc_qp_attr attrs = {0};
        if (requester) attrs.wimm.max_wimm = 1024;
        else attrs.wimm.max_wimm_dest = 1024;
        assert(mrc_modify_qp(qp, NULL, 0, &attrs,
               requester ? MRC_QP_MAX_WIMM : MRC_QP_MAX_WIMM_DEST) == 0);
    }
    struct mrc_dpdk_route route = path_route(pool, requester);
    route.requester = requester;
    attach_qp(qp, &route);
    if (capacity) {
        /* First verify legal independent WIMM=1024/MPR=1 attaches. Then use
         * a larger packet window so semantic credits alone limit admission. */
        struct ibv_qp_attr reset = {.qp_state = IBV_QPS_RESET};
        assert(mrc_modify_qp(qp, &reset, IBV_QP_STATE, NULL, 0) == 0);
        configure_qp(qp, address, &peer, local.psn, requester, !requester, 0, 0, 0, 0, mtu);
        struct mrc_qp_attr attrs = {0};
        if (requester) { attrs.mpr.mpr = 9; attrs.wimm.max_wimm = 1024; }
        else { attrs.mpr.mpr_dest = 9; attrs.wimm.max_wimm_dest = 1024; }
        assert(mrc_modify_qp(qp, NULL, 0, &attrs,
               requester ? MRC_QP_MPR | MRC_QP_MAX_WIMM : MRC_QP_MPR_DEST | MRC_QP_MAX_WIMM_DEST) == 0);
        attach_qp(qp, &route);
    }
    struct wimm_capacity_gate gate = {0};
    const struct rte_eth_rxtx_callback *callbacks[2] = {0};
    if (capacity && !requester)
        for (uint16_t port = 0; port < 2; port++) {
            callbacks[port] = rte_eth_add_tx_callback(port, 0, hold_wimm_acks, &gate);
            assert(callbacks[port]);
        }
    uint64_t saturated_at = 0;

    if (requester) {
        char ready[PATH_MAX];
        path_for(ready, directory, "rx.boundaries-ready");
        wait_for(ready);
        struct ibv_sge sges[rounds];
        struct ibv_send_wr writes[rounds];
        size_t offset = 0;
        for (unsigned int i = 0; i < rounds; i++) {
            sges[i] = (struct ibv_sge){.addr = (uintptr_t)(data + offset),
                                      .length = lengths[i], .lkey = mr->lkey};
            writes[i] = (struct ibv_send_wr){
                .wr_id = i + 1, .next = i + 1 < rounds ? &writes[i + 1] : NULL,
                .sg_list = lengths[i] ? &sges[i] : NULL, .num_sge = lengths[i] ? 1 : 0,
                .opcode = IBV_WR_RDMA_WRITE_WITH_IMM, .send_flags = IBV_SEND_SIGNALED,
                .imm_data = htonl(i + 1),
                .wr.rdma = {.remote_addr = peer.remote_address + offset, .rkey = peer.rkey},
            };
            offset += lengths[i];
        }
        struct ibv_send_wr *bad = NULL;
        assert(mrc_post_send(qp, writes, &bad) == 0 && !bad);
    } else {
        struct ibv_recv_wr receives[rounds];
        for (unsigned int i = 0; i < rounds; i++)
            receives[i] = (struct ibv_recv_wr){.wr_id = i + 1,
                .next = i + 1 < rounds ? &receives[i + 1] : NULL};
        struct ibv_recv_wr *bad = NULL;
        assert(mrc_post_recv(qp, receives, &bad) == 0 && !bad);
        publish(directory, "rx.boundaries-ready", NULL, 0);
    }
    unsigned int completed = 0;
    uint64_t deadline = rte_get_timer_cycles() + 3 * rte_get_timer_hz();
    char done[PATH_MAX];
    path_for(done, directory, "tx.boundaries-done");
    do {
        assert(rte_get_timer_cycles() < deadline);
        assert(mrc_dpdk_progress_qp(qp) == 0);
        struct ibv_wc wc;
        int count = mrc_poll_cq(cq, 1, &wc);
        assert(count >= 0);
        if (count) {
            assert(completed < rounds && wc.status == IBV_WC_SUCCESS && wc.wr_id == completed + 1);
            assert(wc.opcode == (requester ? IBV_WC_RDMA_WRITE : IBV_WC_RECV_RDMA_WITH_IMM));
            if (!requester)
                assert((wc.wc_flags & IBV_WC_WITH_IMM) && ntohl(wc.imm_data) == completed + 1 &&
                       wc.byte_len == lengths[completed]);
            completed++;
        }
        if (capacity && !requester && !gate.released && completed >= 1024) {
            assert(completed == 1024); /* The 1025th WR needs a semantic credit. */
            if (!saturated_at) saturated_at = rte_get_timer_cycles();
            if (rte_get_timer_cycles() - saturated_at > rte_get_timer_hz() / 50) {
                assert(gate.held_acks > 0);
                gate.released = true;
                for (uint16_t port = 0; port < 2; port++)
                    if (gate.latest[port]) {
                        assert(rte_eth_tx_burst(port, 0, &gate.latest[port], 1) == 1);
                        gate.latest[port] = NULL;
                    }
            }
        }
        rte_pause();
    } while (completed < rounds || (!requester && access(done, F_OK) != 0));
    if (requester)
        publish(directory, "tx.boundaries-done", NULL, 0);
    else
        for (size_t i = 0; i < bytes; i++)
            assert(data[i] == pattern(i));
    assert(allocation[0] == 0xa5 && allocation[bytes + 1] == 0x5a);
    if (capacity && !requester) {
        assert(gate.released && saturated_at);
        for (uint16_t port = 0; port < 2; port++) {
            assert(rte_eth_remove_tx_callback(port, 0, callbacks[port]) == 0);
            rte_free((void *)callbacks[port]);
        }
    }
    assert(mrc_destroy_qp(qp) == 0);
    deregister_memory(context, mr);
    assert(mrc_destroy_cq(cq) == 0);
    free(allocation);
    printf("{\"role\":\"%s\",\"qpn\":%u,\"pmtu\":%u,\"bytes\":%zu,\"completions\":%u,\"outstanding_before_ack\":%u,\"held_acks\":%u}\n",
           requester ? "tx" : "rx", local.qpn, pmtu, bytes, completed,
           capacity && !requester ? 1024 : 0, gate.held_acks);
}

static void run_pending_teardown(bool requester, const char *directory, const struct address *address,
                                 struct mrc_context *context, struct ibv_pd *pd, struct rte_mempool *pool,
                                 const char *operation)
{
    bool overflow = !strcmp(operation, "error_pending_overflow");
    uint8_t data[FIRST_BYTES] = {0};
    if (requester)
        for (size_t i = 0; i < sizeof(data); i++) data[i] = pattern(i);
    struct ibv_mr *mr = register_memory(context, pd, data, sizeof(data),
        requester ? 0 : IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    struct mrc_cq *cq = mrc_create_cq(context, 1, NULL, NULL, 0);
    assert(cq);
    struct mrc_qp_init_attr init = {
        .pd = pd, .send_cq = requester ? cq : NULL, .recv_cq = requester ? NULL : cq,
        .cap = {.max_send_wr = requester ? (overflow ? 2 : 1) : 0, .max_send_sge = requester, .max_recv_wr = !requester},
    };
    if (!requester) skip_local_qp_numbers(context, &init, 1);
    struct mrc_qp *qp = mrc_create_qp(context, &init);
    struct connection local = {.gid = address->gid, .psn = SEND_PSN,
        .remote_address = (uintptr_t)data, .rkey = mr->rkey};
    assert(qp && mrc_get_qpn(qp, &local.qpn) == 0);
    publish(directory, requester ? "tx.connection" : "rx.connection", &local, sizeof(local));
    struct connection peer;
    read_peer_connections(directory, requester, &peer, 1);
    configure_qp(qp, address, &peer, local.psn, requester, !requester, 0, 0, 0, 0, IBV_MTU_1024);
    struct mrc_dpdk_route route = path_route(pool, requester);
    route.requester = requester;
    attach_qp(qp, &route);
    prepare_memif_links();
    struct wimm_capacity_gate gate = {0};
    const struct rte_eth_rxtx_callback *callbacks[2] = {0};
    char ready[PATH_MAX], delivered[PATH_MAX], done[PATH_MAX];
    path_for(ready, directory, "rx.cancel-ready");
    path_for(delivered, directory, "rx.cancel-delivered");
    path_for(done, directory, "tx.cancel-done");
    if (requester) {
        wait_for(ready);
        post_single_write_imm(qp, &peer, mr, 0);
    } else {
        for (uint16_t port = 0; port < 2; port++) {
            callbacks[port] = rte_eth_add_tx_callback(port, 0, hold_wimm_acks, &gate);
            assert(callbacks[port]);
        }
        struct ibv_recv_wr receive = {.wr_id = 81}, *bad = NULL;
        assert(mrc_post_recv(qp, &receive, &bad) == 0 && !bad);
        publish(directory, "rx.cancel-ready", NULL, 0);
    }
    uint64_t deadline = rte_get_timer_cycles() + 3 * rte_get_timer_hz();
    struct ibv_wc wc;
    bool received = false;
    do {
        assert(rte_get_timer_cycles() < deadline);
        assert(mrc_dpdk_progress_qp(qp) == 0);
        int count = mrc_poll_cq(cq, 1, &wc);
        assert(count >= 0);
        if (requester) {
            assert(count == 0); /* Reliability delivery must not finish the WR. */
        } else if (count) {
            assert(!received && wc.status == IBV_WC_SUCCESS && wc.wr_id == 81 &&
                   wc.byte_len == FIRST_BYTES && ntohl(wc.imm_data) == FIRST_IMMEDIATE);
            assert(gate.held_acks > 0);
            received = true;
            publish(directory, "rx.cancel-delivered", NULL, 0);
        }
    } while (access(requester ? delivered : done, F_OK) != 0);
    if (requester) {
        assert(mrc_dpdk_unbind_mr(context, mr) == EBUSY);
        if (overflow) {
            /* Queue another WR without transmitting it. Flushing two WRs into
             * a one-entry CQ makes ERROR transition report a partial failure. */
            struct ibv_sge sg = {.addr = (uintptr_t)data, .length = FIRST_BYTES, .lkey = mr->lkey};
            struct ibv_send_wr wr = {.wr_id = 71, .sg_list = &sg, .num_sge = 1,
                .opcode = IBV_WR_RDMA_WRITE_WITH_IMM, .send_flags = IBV_SEND_SIGNALED,
                .wr.rdma = {.remote_addr = peer.remote_address, .rkey = peer.rkey}}, *bad = NULL;
            assert(mrc_post_send(qp, &wr, &bad) == 0 && !bad);
        }
        if (!strcmp(operation, "destroy_pending")) {
            assert(mrc_destroy_qp(qp) == 0);
            qp = NULL;
        } else {
            bool error = overflow || !strcmp(operation, "error_pending");
            struct ibv_qp_attr attrs = {.qp_state = error ? IBV_QPS_ERR : IBV_QPS_RESET};
            assert(mrc_modify_qp(qp, &attrs, IBV_QP_STATE, NULL, 0) == (overflow ? EOVERFLOW : 0));
            assert(mrc_query_qp(qp, &attrs, IBV_QP_STATE, NULL, 0, NULL) == 0);
            assert(attrs.qp_state == (error ? IBV_QPS_ERR : IBV_QPS_RESET));
            assert(mrc_dpdk_progress_qp(qp) == ENOTCONN);
            if (error && !overflow) {
                assert(mrc_poll_cq(cq, 1, &wc) == 1 && wc.wr_id == 70 &&
                       wc.status == IBV_WC_WR_FLUSH_ERR);
            }
        }
        if (overflow) assert(mrc_poll_cq(cq, 1, &wc) == -1 && errno == EOVERFLOW);
        else assert(mrc_poll_cq(cq, 1, &wc) == 0);
        publish(directory, "tx.cancel-done", NULL, 0);
    } else {
        assert(received);
        for (size_t i = 0; i < sizeof(data); i++) assert(data[i] == pattern(i));
        for (uint16_t port = 0; port < 2; port++) {
            assert(rte_eth_remove_tx_callback(port, 0, callbacks[port]) == 0);
            rte_free((void *)callbacks[port]);
            if (gate.latest[port]) rte_pktmbuf_free(gate.latest[port]);
        }
    }
    if (qp) assert(mrc_destroy_qp(qp) == 0);
    deregister_memory(context, mr);
    assert(mrc_destroy_cq(cq) == 0);
    printf("{\"role\":\"%s\",\"operation\":\"%s\",\"pending_cancelled\":true}\n",
           requester ? "tx" : "rx", operation);
}

struct cc_capacity_witness { uint32_t qpn[32]; uint32_t sent; unsigned int packets[32]; };
static uint16_t observe_cc_capacity(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                    uint16_t count, void *argument)
{
    (void)port;
    (void)queue;
    struct cc_capacity_witness *witness = argument;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t prefix[14 + 40 + WIRE_UDP_LEN + WIRE_BTH_LEN];
        struct observed_udp udp = read_udp(packets[i], prefix, sizeof(prefix));
        const uint8_t *bth = udp.header + WIRE_UDP_LEN;
        if (bth[0] != WIRE_WRITE_FIRST && bth[0] != WIRE_WRITE_MIDDLE && bth[0] != WIRE_WRITE_LAST_IMM) continue;
        unsigned int slot = 0;
        while (slot < 32 && witness->qpn[slot] != wire_be24(bth + 5)) slot++;
        assert(slot < 32);
        witness->sent |= UINT32_C(1) << slot;
        witness->packets[slot]++;
    }
    return count;
}

static void run_cc_capacity(bool requester, const char *directory, const struct address *address,
                              struct mrc_context *context, struct ibv_pd *pd, struct rte_mempool *pool,
                              const char *store, uint64_t first_profile)
{
    enum { count = 32, bytes = 2049 };
    uint8_t data[count][bytes];
    memset(data, 0, sizeof(data));
    if (requester)
        for (size_t i = 0; i < sizeof(data); i++) ((uint8_t *)data)[i] = pattern(i);
    assert(mrc_dpdk_set_ev_store(context, store) == 0);
    struct ibv_mr *mr = register_memory(context, pd, data[0], sizeof(data),
        requester ? 0 : IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    struct mrc_cq *cq = mrc_create_cq(context, count, NULL, NULL, 0);
    assert(cq);
    struct mrc_qp_init_attr init = {.pd = pd,
        .send_cq = requester ? cq : NULL, .recv_cq = requester ? NULL : cq,
        .cap = {.max_send_wr = requester, .max_send_sge = requester, .max_recv_wr = !requester}};
    if (!requester) skip_local_qp_numbers(context, &init, count);
    struct mrc_qp *qps[count];
    struct connection local[count], peer[count];
    for (unsigned int i = 0; i < count; i++) {
        qps[i] = mrc_create_qp(context, &init);
        local[i] = (struct connection){.gid = address->gid, .psn = SEND_PSN,
            .remote_address = (uintptr_t)data[i], .rkey = mr->rkey};
        assert(qps[i] && mrc_get_qpn(qps[i], &local[i].qpn) == 0);
    }
    publish(directory, requester ? "tx.connection" : "rx.connection", local, sizeof(local));
    read_peer_connections(directory, requester, peer, count);
    for (unsigned int i = 0; i < count; i++) {
        assert(local[i].qpn != peer[i].qpn);
        configure_qp(qps[i], address, &peer[i], local[i].psn, requester, !requester,
                     0, requester ? first_profile + i : 0, 0, 0, IBV_MTU_1024);
        if (requester) {
            struct mrc_qp_attr attrs;
            assert(mrc_query_qp(qps[i], NULL, 0, &attrs, MRC_QP_CC_PROFILE_ID, NULL) == 0 &&
                   attrs.profile.cc_profile_id == first_profile + i);
        }
        struct mrc_dpdk_route route = path_route(pool, requester);
        route.requester = requester;
        attach_qp(qps[i], &route);
    }
    prepare_memif_links();
    struct cc_capacity_witness witness = {0};
    const struct rte_eth_rxtx_callback *callbacks[2] = {0};
    if (requester) {
        for (unsigned int i = 0; i < count; i++) witness.qpn[i] = peer[i].qpn;
        for (uint16_t port = 0; port < 2; port++) {
            callbacks[port] = rte_eth_add_tx_callback(port, 0, observe_cc_capacity, &witness);
            assert(callbacks[port]);
        }
    }
    char ready[PATH_MAX], checked[PATH_MAX], done[PATH_MAX];
    path_for(ready, directory, "rx.cc-capacity-ready");
    path_for(checked, store, "capacity-checked");
    path_for(done, directory, "tx.cc-capacity-done");
    if (requester) {
        wait_for(ready);
        for (unsigned int i = 0; i < count; i++) {
            struct ibv_sge sg = {.addr = (uintptr_t)data[i], .length = bytes, .lkey = mr->lkey};
            struct ibv_send_wr wr = {.wr_id = i + 1, .sg_list = &sg, .num_sge = 1,
                .opcode = IBV_WR_RDMA_WRITE_WITH_IMM, .send_flags = IBV_SEND_SIGNALED,
                .imm_data = htonl(i + 1),
                .wr.rdma = {.remote_addr = peer[i].remote_address, .rkey = peer[i].rkey}}, *bad = NULL;
            assert(mrc_post_send(qps[i], &wr, &bad) == 0 && !bad);
            assert(mrc_dpdk_progress_qp(qps[i]) == 0);
        }
        uint64_t flight_deadline = rte_get_timer_cycles() + rte_get_timer_hz();
        while (witness.sent != UINT32_MAX) {
            assert(rte_get_timer_cycles() < flight_deadline);
            for (unsigned int i = 0; i < count; i++) assert(mrc_dpdk_progress_qp(qps[i]) == 0);
        }
        struct ibv_wc wc;
        assert(mrc_poll_cq(cq, 1, &wc) == 0);
        for (unsigned int i = 0; i < count; i++) assert(witness.packets[i] == 1);
        publish(store, "capacity-live", NULL, 0);
    } else {
        for (unsigned int i = 0; i < count; i++) {
            struct ibv_recv_wr wr = {.wr_id = i + 1}, *bad = NULL;
            assert(mrc_post_recv(qps[i], &wr, &bad) == 0 && !bad);
        }
        publish(directory, "rx.cc-capacity-ready", NULL, 0);
    }
    wait_for(checked); /* Independent controller verifies every live profile lease. */
    uint32_t seen = 0;
    uint64_t deadline = rte_get_timer_cycles() + 3 * rte_get_timer_hz();
    do {
        assert(rte_get_timer_cycles() < deadline);
        for (unsigned int i = 0; i < count; i++) assert(mrc_dpdk_progress_qp(qps[i]) == 0);
        struct ibv_wc wc;
        int available;
        while ((available = mrc_poll_cq(cq, 1, &wc)) > 0) {
            assert(wc.wr_id >= 1 && wc.wr_id <= count && wc.status == IBV_WC_SUCCESS);
            unsigned int i = wc.wr_id - 1;
            assert(!(seen & (UINT32_C(1) << i)) && wc.qp_num == local[i].qpn);
            seen |= UINT32_C(1) << i;
            assert(wc.opcode == (requester ? IBV_WC_RDMA_WRITE : IBV_WC_RECV_RDMA_WITH_IMM));
            if (!requester) assert(wc.byte_len == bytes && ntohl(wc.imm_data) == i + 1);
        }
        assert(available == 0);
    } while (seen != UINT32_MAX || (!requester && access(done, F_OK) != 0));
    if (requester) publish(directory, "tx.cc-capacity-done", NULL, 0);
    else for (size_t i = 0; i < sizeof(data); i++) assert(((uint8_t *)data)[i] == pattern(i));
    if (requester)
        for (uint16_t port = 0; port < 2; port++) {
            assert(rte_eth_remove_tx_callback(port, 0, callbacks[port]) == 0);
            rte_free((void *)callbacks[port]);
        }
    for (unsigned int i = 0; i < count; i++) assert(mrc_destroy_qp(qps[i]) == 0);
    deregister_memory(context, mr);
    assert(mrc_destroy_cq(cq) == 0);
    printf("{\"role\":\"%s\",\"profiles\":32,\"completions\":32,\"bytes\":%zu}\n",
           requester ? "tx" : "rx", sizeof(data));
}

struct blackout_witness {
    unsigned int dropped_writes[DUPLEX_RAILS];
    unsigned int first_rails;
    unsigned int retry_rails;
    unsigned int probe_rails_after_write;
    bool rail_restored;
    bool recovery_probe;
};

static uint16_t blackhole_ingress(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                  uint16_t count, uint16_t capacity, void *argument)
{
    (void)queue;
    (void)capacity;
    struct blackout_witness *observed = argument;
    unsigned int bit = 1u << port;
    uint16_t kept = 0;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t prefix[RTE_ETHER_HDR_LEN + WIRE_IPV6_LEN + WIRE_UDP_LEN + WIRE_BTH_LEN];
        struct observed_udp udp = read_udp(packets[i], prefix, sizeof(prefix));
        const uint8_t *bth = udp.header + WIRE_UDP_LEN;
        if (observed->rail_restored && port == 0) {
            if (bth[0] == WIRE_PROBE_REQUEST)
                observed->recovery_probe = true;
            if (bth[0] == WIRE_WRITE_ONLY_IMM)
                assert(observed->recovery_probe && (bth[8] & 0x20));
            packets[kept++] = packets[i];
            continue;
        }
        if (bth[0] == WIRE_WRITE_ONLY_IMM) {
            assert(wire_be24(bth + 9) == SEND_PSN);
            observed->dropped_writes[port]++;
            if (bth[8] & 0x20)
                observed->retry_rails |= bit;
            else
                observed->first_rails |= bit;
        } else if (bth[0] == WIRE_PROBE_REQUEST && observed->dropped_writes[port]) {
            observed->probe_rails_after_write |= bit;
        }
        rte_pktmbuf_free(packets[i]);
    }
    return kept;
}

static uint64_t check_blackout_requester(struct mrc_context *context, struct mrc_qp *qp, struct mrc_cq *cq,
                                         const char *directory, uint32_t qpn, bool infinite)
{
    int fd;
    assert(mrc_dpdk_get_async_fd(context, &fd) == 0);
    int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    uint64_t hz = rte_get_timer_hz();
    uint64_t start = rte_get_timer_cycles();
    uint64_t restore_at = 0;
    struct ibv_wc wc;
    struct mrc_async_event event;
    struct ibv_qp_attr state;
    int result, completions;
    do {
        uint64_t elapsed = rte_get_timer_cycles() - start;
        assert(elapsed < 4 * hz && "requester did not complete the two-rail blackout case");
        result = mrc_dpdk_progress_qp(qp);
        completions = mrc_poll_cq(cq, 1, &wc);
        assert(completions >= 0 && (completions || !result));
        if (infinite && !restore_at && elapsed >= 3 * hz / 2) {
            assert(!completions && !result);
            assert(mrc_query_qp(qp, &state, IBV_QP_STATE, NULL, 0, NULL) == 0 &&
                   state.qp_state == IBV_QPS_RTS);
            assert(mrc_get_async_event(context, &event) == -1 && errno == EAGAIN);
            restore_at = rte_get_timer_cycles();
            publish(directory, "tx.retrying", NULL, 0);
        }
        rte_pause();
    } while (!completions);
    uint64_t finished = rte_get_timer_cycles();
    assert(wc.wr_id == 70 && wc.qp_num == qpn && wc.opcode == IBV_WC_RDMA_WRITE);
    assert(mrc_query_qp(qp, &state, IBV_QP_STATE, NULL, 0, NULL) == 0);
    if (infinite) {
        assert(restore_at && !result && wc.status == IBV_WC_SUCCESS && state.qp_state == IBV_QPS_RTS);
        assert(finished - restore_at < 2 * hz / 5 && "restored rail did not retry the pending WRITE promptly");
    } else {
        /* Exponent 16 and three exponential retries allow about one
         * second without a response when both rails remain unavailable. */
        assert(finished - start >= 3 * hz / 4);
        assert(result == ETIMEDOUT && wc.status == IBV_WC_RETRY_EXC_ERR && state.qp_state == IBV_QPS_ERR);
        assert(mrc_get_async_event(context, &event) == 0 && event.event_type == IBV_EVENT_QP_FATAL &&
               event.element.qp == qp);
        mrc_ack_async_event(&event);
    }
    assert(mrc_poll_cq(cq, 1, &wc) == 0);
    assert(mrc_get_async_event(context, &event) == -1 && errno == EAGAIN);
    return (finished - (infinite ? restore_at : start)) * 1000 / hz;
}

struct trim_witness {
    bool requester;
    bool checked_before_retry;
    bool saw_sack;
    bool saw_transport_ack;
    uint8_t data_dscp;
    uint8_t retry_dscp;
    unsigned int marked;
    unsigned int retries;
    unsigned int nacks;
    unsigned int probe_rails;
    uint16_t marked_rail;
    uint16_t retry_rail;
    uint16_t nack_rail;
    uint16_t original_udp_len;
};

static uint16_t witness_last_hop_trim(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                      uint16_t count, uint16_t capacity, void *argument)
{
    (void)queue;
    (void)capacity;
    struct trim_witness *observed = argument;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t prefix[RTE_ETHER_HDR_LEN + WIRE_IPV6_LEN + WIRE_UDP_LEN + WIRE_BTH_LEN + WIRE_NETH_LEN];
        struct observed_udp udp = read_udp(packets[i], prefix, sizeof(prefix));
        const uint8_t *bth = udp.header + WIRE_UDP_LEN;
        uint8_t opcode = bth[0];
        if (observed->requester) {
            if (opcode == WIRE_SACK || opcode == WIRE_NACK || opcode == WIRE_TRANSPORT_ACK)
                assert(udp.traffic_class == CONTROL_DSCP << 2);
            observed->saw_sack |= opcode == WIRE_SACK;
            observed->saw_transport_ack |= opcode == WIRE_TRANSPORT_ACK;
            if (opcode != WIRE_NACK)
                continue;
            const uint8_t *neth = bth + WIRE_BTH_LEN;
            assert(udp.bytes >= WIRE_UDP_LEN + WIRE_BTH_LEN + WIRE_NETH_LEN);
            assert(neth[2] == LAST_HOP_TRIM_REASON && wire_be24(neth + 13) == SEND_PSN);
            assert(wire_be24(bth + 9) == SEND_PSN && !(bth[8] & 0x20));
            observed->nack_rail = port;
            observed->original_udp_len = wire_be16(udp.header + 4);
            observed->nacks++;
            continue;
        }
        if (opcode == WIRE_PROBE_REQUEST) {
            assert(udp.traffic_class == observed->data_dscp << 2);
            observed->probe_rails |= 1u << port;
        }
        if (opcode != WIRE_WRITE_ONLY_IMM)
            continue;
        assert(wire_be24(bth + 9) == SEND_PSN);
        assert(wire_be16(udp.header + 4) == SINGLE_WRITE_UDP_LEN);
        assert(rte_pktmbuf_pkt_len(packets[i]) == RTE_ETHER_HDR_LEN + WIRE_IPV6_LEN + SINGLE_WRITE_UDP_LEN);
        if (bth[8] & 0x20) {
            assert(udp.traffic_class == ((observed->retry_dscp << 2) | 2));
            assert(observed->marked == 1 && observed->checked_before_retry);
            observed->retry_rail = port;
            observed->retries++;
            continue;
        }
        assert(udp.traffic_class == ((observed->data_dscp << 2) | 2));
        assert(observed->marked++ == 0 && rte_pktmbuf_is_contiguous(packets[i]));
        observed->marked_rail = port;
        /* Keep the full frame and iCRC. The iCRC masks the IPv6 traffic class. */
        uint8_t *ip = rte_pktmbuf_mtod_offset(packets[i], uint8_t *, RTE_ETHER_HDR_LEN);
        assert((ip[0] >> 4) == 6);
        uint8_t traffic_class = (LAST_HOP_TRIM_DSCP << 2) | ((ip[1] >> 4) & 3);
        ip[0] = (ip[0] & 0xf0) | (traffic_class >> 4);
        ip[1] = (ip[1] & 0x0f) | (uint8_t)(traffic_class << 4);
    }
    return count;
}

static void run_last_hop_trim(bool requester, const char *directory, struct mrc_qp *qp, struct mrc_cq *cq,
                             struct ibv_mr *mr, const struct connection *peer, uint32_t qpn,
                             const uint8_t data[FIRST_BYTES], bool service_level_dscp)
{
    struct trim_witness observed = {
        .requester = requester,
        .data_dscp = service_level_dscp ? SERVICE_DATA_DSCP : 0,
        .retry_dscp = service_level_dscp ? SERVICE_RETRY_DSCP : 0,
    };
    const struct rte_eth_rxtx_callback *callbacks[DUPLEX_RAILS];
    for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
        callbacks[port] = rte_eth_add_rx_callback(port, 0, witness_last_hop_trim, &observed);
        assert(callbacks[port]);
    }

    char ready[PATH_MAX], done[PATH_MAX];
    path_for(ready, directory, "rx.ready");
    path_for(done, directory, "tx.done");
    if (requester) {
        wait_for(ready);
        post_single_write_imm(qp, peer, mr, 0);
    } else {
        struct ibv_recv_wr receive = { .wr_id = 81 };
        struct ibv_recv_wr *bad = NULL;
        assert(mrc_post_recv(qp, &receive, &bad) == 0 && !bad);
        publish(directory, "rx.ready", NULL, 0);
    }

    unsigned int completed = 0;
    uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
    do {
        assert(rte_get_timer_cycles() < deadline);
        assert(mrc_dpdk_progress_qp(qp) == 0);
        struct ibv_wc wc;
        int entries = mrc_poll_cq(cq, 1, &wc);
        assert(entries >= 0 && completed + (unsigned int)entries <= 1);
        if (!requester && observed.marked && !observed.checked_before_retry) {
            assert(!entries && !observed.retries);
            for (size_t i = 0; i < FIRST_BYTES; i++)
                assert(data[i] == 0);
            observed.checked_before_retry = true;
        }
        if (entries) {
            assert(wc.status == IBV_WC_SUCCESS && wc.qp_num == qpn);
            if (requester) {
                assert(wc.wr_id == 70 && wc.opcode == IBV_WC_RDMA_WRITE);
            } else {
                assert(wc.wr_id == 81 && wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM);
                assert(wc.byte_len == FIRST_BYTES && (wc.wc_flags & IBV_WC_WITH_IMM));
                assert(ntohl(wc.imm_data) == FIRST_IMMEDIATE);
            }
            completed++;
        }
        rte_pause();
    } while (!completed || (!requester && access(done, F_OK) != 0));

    for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
        assert(rte_eth_remove_rx_callback(port, 0, callbacks[port]) == 0);
        rte_free((void *)callbacks[port]);
    }
    if (requester) {
        assert(observed.nacks == 1 && observed.original_udp_len == SINGLE_WRITE_UDP_LEN);
        publish(directory, "tx.done", NULL, 0);
        printf("{\"role\":\"tx\",\"qpn\":%u,\"completions\":%u,\"nack_reason\":%u,"
               "\"nack_rail\":%u,\"original_udp_len\":%u",
               qpn, completed, LAST_HOP_TRIM_REASON, observed.nack_rail, observed.original_udp_len);
        if (service_level_dscp) {
            assert(observed.saw_sack && observed.saw_transport_ack);
            printf(",\"control_dscp\":%u", CONTROL_DSCP);
        }
    } else {
        assert(observed.marked == 1 && observed.retries == 1 && observed.checked_before_retry);
        assert(observed.marked_rail != observed.retry_rail);
        for (size_t i = 0; i < FIRST_BYTES; i++)
            assert(data[i] == pattern(i));
        printf("{\"role\":\"rx\",\"qpn\":%u,\"completions\":%u,\"marked_rail\":%u,\"retry_rail\":%u",
               qpn, completed, observed.marked_rail, observed.retry_rail);
        if (service_level_dscp) {
            assert(observed.probe_rails == (1u << DUPLEX_RAILS) - 1);
            printf(",\"data_dscp\":%u,\"retry_dscp\":%u,\"probe_rails\":%u",
                   observed.data_dscp, observed.retry_dscp, observed.probe_rails);
        }
    }
    puts("}");
}

struct mpr_witness {
    uint32_t psn;
    uint32_t requester_qpn;
    uint32_t responder_qpn;
    uint8_t advertised;
    bool write_sent;
    bool sack_received;
};

static uint16_t observe_mpr_write(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                  uint16_t count, void *argument)
{
    (void)port;
    (void)queue;
    struct mpr_witness *observed = argument;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t prefix[RTE_ETHER_HDR_LEN + WIRE_IPV6_LEN + WIRE_UDP_LEN + WIRE_BTH_LEN];
        struct observed_udp udp = read_udp(packets[i], prefix, sizeof(prefix));
        const uint8_t *bth = udp.header + WIRE_UDP_LEN;
        if (bth[0] == WIRE_WRITE_ONLY_IMM && wire_be24(bth + 5) == observed->responder_qpn &&
            wire_be24(bth + 9) == observed->psn)
            observed->write_sent = true;
    }
    return count;
}

static uint16_t observe_mpr_sack(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                 uint16_t count, uint16_t capacity, void *argument)
{
    (void)port;
    (void)queue;
    (void)capacity;
    struct mpr_witness *observed = argument;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t prefix[RTE_ETHER_HDR_LEN + WIRE_IPV6_LEN + WIRE_UDP_LEN + WIRE_BTH_LEN + WIRE_SETH_LEN];
        struct observed_udp udp = read_udp(packets[i], prefix, sizeof(prefix));
        const uint8_t *bth = udp.header + WIRE_UDP_LEN;
        if (bth[0] != WIRE_SACK || wire_be24(bth + 5) != observed->requester_qpn)
            continue;
        assert(udp.bytes >= WIRE_UDP_LEN + WIRE_BTH_LEN + WIRE_SETH_LEN);
        const uint8_t *seth = bth + WIRE_BTH_LEN;
        if (!(seth[1] & 0x02) && wire_be24(seth + 13) == observed->psn) {
            observed->sack_received = true;
            observed->advertised = seth[17];
        }
    }
    return count;
}

static uint8_t query_mpr(struct mrc_qp *qp, bool requester)
{
    struct mrc_qp_attr attrs;
    assert(mrc_query_qp(qp, NULL, 0, &attrs, requester ? MRC_QP_MPR : MRC_QP_MPR_DEST, NULL) == 0);
    return requester ? attrs.mpr.mpr : attrs.mpr.mpr_dest;
}

static void run_mpr_updates(bool requester, const char *directory, struct mrc_qp *qp, struct mrc_cq *cq,
                            struct ibv_mr *mr, const struct connection *peer, uint32_t qpn,
                            bool dynamic, enum ibv_qp_state state, uint8_t maximum)
{
    struct mpr_witness observed = {
        .requester_qpn = qpn, .responder_qpn = peer->qpn,
    };
    const struct rte_eth_rxtx_callback *tx_callbacks[DUPLEX_RAILS], *rx_callbacks[DUPLEX_RAILS];
    if (requester) {
        for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
            tx_callbacks[port] = rte_eth_add_tx_callback(port, 0, observe_mpr_write, &observed);
            rx_callbacks[port] = rte_eth_add_rx_callback(port, 0, observe_mpr_sack, &observed);
            assert(tx_callbacks[port] && rx_callbacks[port]);
        }
    }
    struct ibv_qp_attr verbs;
    assert(mrc_query_qp(qp, &verbs, IBV_QP_STATE, NULL, 0, NULL) == 0 && verbs.qp_state == state);
    struct mrc_qp_attr update = { .mpr.mpr_dest = 1 };
    if (requester || !dynamic) {
        assert(mrc_modify_qp(qp, NULL, 0, &update, MRC_QP_MPR_DEST) == EOPNOTSUPP);
    } else {
        /* The upper bound is the negotiated limit rather than the device limit. */
        if (maximum < UINT8_MAX) {
            update.mpr.mpr_dest = maximum + 1;
            assert(mrc_modify_qp(qp, NULL, 0, &update, MRC_QP_MPR_DEST) == EINVAL);
        }
        update.mpr.mpr_dest = 0;
        assert(mrc_modify_qp(qp, NULL, 0, &update, MRC_QP_MPR_DEST) == EINVAL);
    }
    assert(query_mpr(qp, requester) == maximum);

    uint8_t queried[MPR_TEST_ROUNDS], advertised[MPR_TEST_ROUNDS] = {0};
    const uint8_t *data = mr->addr;
    for (unsigned int round = 0; round < MPR_TEST_ROUNDS; round++) {
        char ready_name[32], sent_name[32], marker[PATH_MAX];
        snprintf(ready_name, sizeof(ready_name), "rx.mpr-ready.%u", round);
        snprintf(sent_name, sizeof(sent_name), "tx.mpr-sent.%u", round);
        uint8_t expected = dynamic && round == 1 ? 1 : maximum;
        observed.psn = (SEND_PSN + round) & 0xffffff;
        observed.write_sent = observed.sack_received = false;
        observed.advertised = 0;
        unsigned int offset = round * FIRST_BYTES;
        uint64_t deadline = rte_get_timer_cycles() + 4 * rte_get_timer_hz();
        if (requester) {
            path_for(marker, directory, ready_name);
            wait_for(marker);
            post_single_write_imm(qp, peer, mr, round);
            do {
                assert(rte_get_timer_cycles() < deadline);
                assert(mrc_dpdk_progress_qp(qp) == 0);
            } while (!observed.write_sent);
            publish(directory, sent_name, NULL, 0);
        } else {
            struct ibv_recv_wr receive = { .wr_id = 81 + round }, *bad = NULL;
            assert(mrc_post_recv(qp, &receive, &bad) == 0 && !bad);
            publish(directory, ready_name, NULL, 0);
            path_for(marker, directory, sent_name);
            wait_for(marker);
            /* The requester sent this WRITEIMM while responder progress was paused. */
            for (size_t i = 0; i < FIRST_BYTES; i++)
                assert(data[offset + i] == 0);
            if (dynamic && round) {
                update.mpr.mpr_dest = expected;
                if (round == 1) {
                    assert(mrc_modify_qp(qp, NULL, 0, &update, MRC_QP_MPR_DEST) == 0);
                } else {
                    verbs.cur_qp_state = IBV_QPS_INIT;
                    assert(mrc_modify_qp(qp, &verbs, IBV_QP_CUR_STATE, &update, MRC_QP_MPR_DEST) == EINVAL);
                    assert(query_mpr(qp, false) == 1);
                    verbs.cur_qp_state = state;
                    assert(mrc_modify_qp(qp, &verbs, IBV_QP_CUR_STATE, &update, MRC_QP_MPR_DEST) == 0);
                }
            }
            assert(query_mpr(qp, false) == expected);
        }

        struct ibv_wc wc;
        int count;
        do {
            assert(rte_get_timer_cycles() < deadline);
            assert(mrc_dpdk_progress_qp(qp) == 0);
            count = mrc_poll_cq(cq, 1, &wc);
            assert(count >= 0);
            rte_pause();
        } while (!count);
        assert(wc.status == IBV_WC_SUCCESS && wc.qp_num == qpn);
        if (requester) {
            assert(wc.wr_id == 70 + round && wc.opcode == IBV_WC_RDMA_WRITE);
            assert(observed.sack_received && observed.advertised == (dynamic ? expected : 0));
            advertised[round] = observed.advertised;
        } else {
            assert(wc.wr_id == 81 + round && wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM);
            assert(wc.byte_len == FIRST_BYTES && (wc.wc_flags & IBV_WC_WITH_IMM));
            assert(ntohl(wc.imm_data) == FIRST_IMMEDIATE + round);
            for (size_t i = 0; i < FIRST_BYTES; i++)
                assert(data[offset + i] == pattern(offset + i));
        }
        queried[round] = query_mpr(qp, requester);
        assert(queried[round] == expected);
        assert(mrc_poll_cq(cq, 1, &wc) == 0);
    }
    if (requester) {
        publish(directory, "tx.mpr-done", NULL, 0);
        for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
            assert(rte_eth_remove_tx_callback(port, 0, tx_callbacks[port]) == 0);
            assert(rte_eth_remove_rx_callback(port, 0, rx_callbacks[port]) == 0);
            rte_free((void *)tx_callbacks[port]);
            rte_free((void *)rx_callbacks[port]);
        }
    } else {
        char done[PATH_MAX];
        path_for(done, directory, "tx.mpr-done");
        wait_for(done);
    }
    printf("{\"role\":\"%s\",\"qpn\":%u,\"completions\":%u,\"queried_mpr\":[%u,%u,%u]",
           requester ? "tx" : "rx", qpn, MPR_TEST_ROUNDS, queried[0], queried[1], queried[2]);
    if (requester)
        printf(",\"advertised_mpr\":[%u,%u,%u]", advertised[0], advertised[1], advertised[2]);
    puts("}");
}

static void run_single_write(bool requester, const char *directory, const struct address *address,
                             struct mrc_context *context, struct ibv_pd *pd, struct rte_mempool *pool,
                             enum single_write_case scenario)
{
    bool infinite = scenario == BLACKOUT_INFINITE;
    bool service_level_dscp = scenario == SERVICE_LEVEL_DSCP;
    bool last_hop_trim = scenario == LAST_HOP_TRIM || service_level_dscp;
    bool responder_rts = scenario == DYNAMIC_MPR_RTS;
    bool dynamic = scenario == DYNAMIC_MPR || responder_rts;
    bool mpr_updates = dynamic || scenario == STATIC_MPR;
    uint8_t maximum = responder_rts ? UINT8_MAX : MPR_TEST_MAX;
    struct mrc_cq *cq = mrc_create_cq(context, 1, NULL, NULL, 0);
    assert(cq);
    uint8_t data[MPR_TEST_ROUNDS * FIRST_BYTES] = {0};
    size_t data_length = mpr_updates ? sizeof(data) : FIRST_BYTES;
    if (requester)
        for (size_t i = 0; i < data_length; i++)
            data[i] = pattern(i);
    struct ibv_mr *mr = register_memory(context, pd, data, data_length,
                                        requester ? 0 : IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

    struct mrc_qp_init_attr init = {
        .pd = pd, .protocol_version = MRC_PROTOCOL_VERSION_1,
        .send_cq = requester ? cq : NULL, .recv_cq = requester ? NULL : cq,
        .cap = {
            .max_send_wr = requester, .max_send_sge = requester, .max_recv_wr = !requester,
        },
    };
    if (!requester)
        skip_local_qp_numbers(context, &init, 1);
    struct mrc_qp *qp = mrc_create_qp(context, &init);
    struct connection local = {
        .gid = address->gid, .psn = requester ? SEND_PSN : 135,
        .remote_address = (uintptr_t)data, .rkey = mr->rkey,
    };
    assert(qp && mrc_get_qpn(qp, &local.qpn) == 0);
    publish(directory, requester ? "tx.connection" : "rx.connection", &local, sizeof(local));
    struct connection peer;
    read_peer_connections(directory, requester, &peer, 1);
    assert(local.qpn != peer.qpn);
    /* The remote requester does not generate trim NACKs. The local responder does. */
    unsigned int caps = last_hop_trim && !requester ? MRC_QP_CAP_TRIM_NACK_DEST_UNSUPPORTED : 0;
    if (dynamic)
        caps |= MRC_QP_CAP_DYNAMIC_MPR;
    uint8_t sl = service_level_dscp ? MAPPED_SERVICE_LEVEL : 0;
    if (service_level_dscp) {
        assert(mrc_dpdk_set_sl_dscp(NULL, sl, 0, 0) == EINVAL);
        assert(mrc_dpdk_set_sl_dscp(context, 16, 0, 0) == EINVAL);
        assert(mrc_dpdk_set_sl_dscp(context, sl, 64, 0) == EINVAL);
        assert(mrc_dpdk_set_sl_dscp(context, sl, 0, 64) == EINVAL);
        assert(mrc_dpdk_set_sl_dscp(context, sl, CONTROL_DSCP, 0) == EINVAL);
        assert(mrc_dpdk_set_sl_dscp(context, sl, 0, CONTROL_DSCP) == EINVAL);
        assert(mrc_dpdk_set_sl_dscp(context, sl, SERVICE_DATA_DSCP, SERVICE_RETRY_DSCP) == 0);
        assert(mrc_dpdk_set_sl_dscp(context, 0, 8, 16) == 0);
    }
    configure_qp(qp, address, &peer, local.psn, requester, !requester, 0, 0, caps, sl, IBV_MTU_1024);
    if (mpr_updates) {
        struct mrc_qp_attr attrs = { .mpr = { .mpr = maximum, .mpr_dest = maximum } };
        assert(mrc_modify_qp(qp, NULL, 0, &attrs, requester ? MRC_QP_MPR : MRC_QP_MPR_DEST) == 0);
    }
    if (responder_rts && !requester) {
        struct ibv_qp_attr verbs = { .qp_state = IBV_QPS_RTS, .sq_psn = local.psn };
        assert(mrc_modify_qp(qp, &verbs, IBV_QP_STATE | IBV_QP_SQ_PSN, NULL, 0) == 0);
    }
    if (service_level_dscp) {
        struct ibv_qp_attr verbs;
        assert(mrc_query_qp(qp, &verbs, IBV_QP_AV, NULL, 0, NULL) == 0 && verbs.ah_attr.sl == sl);
    }
    if (requester) {
        struct mrc_qp_attr attrs = {
            .timeout = last_hop_trim || mpr_updates ? 20 : BLACKOUT_TIMEOUT_EXPONENT,
            .retry.exp_cnt = infinite ? 25 : BLACKOUT_FINITE_RETRIES,
        };
        assert(mrc_modify_qp(qp, NULL, 0, &attrs, MRC_QP_TIMEOUT | MRC_QP_RETRY_CNT) == 0);
    }
    if (last_hop_trim)
        assert(mrc_dpdk_set_trim_last_hop_dscp(qp, LAST_HOP_TRIM_DSCP) == ENOTCONN);
    struct mrc_dpdk_route route = path_route(pool, requester);
    route.requester = requester;
    attach_qp(qp, &route);
    if (service_level_dscp)
        assert(mrc_dpdk_set_sl_dscp(context, sl, 14, 22) == 0);
    prepare_memif_links();
    if (mpr_updates) {
        enum ibv_qp_state state = requester || responder_rts ? IBV_QPS_RTS : IBV_QPS_RTR;
        run_mpr_updates(requester, directory, qp, cq, mr, &peer, local.qpn, dynamic, state, maximum);
    } else if (last_hop_trim) {
        if (requester) {
            assert(mrc_dpdk_set_trim_last_hop_dscp(qp, LAST_HOP_TRIM_DSCP) == ENOTCONN);
        } else {
            assert(mrc_dpdk_set_trim_last_hop_dscp(qp, 64) == EINVAL);
            assert(mrc_dpdk_set_trim_last_hop_dscp(qp, 48) == EINVAL);
            assert(mrc_dpdk_set_trim_last_hop_dscp(qp, 49) == EINVAL);
            assert(mrc_dpdk_set_trim_last_hop_dscp(qp, LAST_HOP_TRIM_DSCP) == 0);
            assert(mrc_dpdk_set_trim_last_hop_dscp(qp, 0) == 0);
            assert(mrc_dpdk_set_trim_last_hop_dscp(qp, LAST_HOP_TRIM_DSCP) == 0);
        }
        run_last_hop_trim(requester, directory, qp, cq, mr, &peer, local.qpn, data, service_level_dscp);
    } else if (requester) {
        char ready[PATH_MAX];
        path_for(ready, directory, "rx.ready");
        wait_for(ready);
        post_single_write_imm(qp, &peer, mr, 0);
        uint64_t elapsed_ms = check_blackout_requester(context, qp, cq, directory, local.qpn, infinite);
        publish(directory, "tx.done", NULL, 0);
        printf("{\"role\":\"tx\",\"qpn\":%u,\"completion\":\"%s\",\"%s\":%llu}\n", local.qpn,
               infinite ? "success" : "retry-exceeded",
               infinite ? "restore_ms" : "timeout_ms", (unsigned long long)elapsed_ms);
    } else {
        struct blackout_witness observed = {0};
        const struct rte_eth_rxtx_callback *callbacks[DUPLEX_RAILS];
        for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
            callbacks[port] = rte_eth_add_rx_callback(port, 0, blackhole_ingress, &observed);
            assert(callbacks[port]);
        }
        struct ibv_recv_wr receive = { .wr_id = 81 };
        struct ibv_recv_wr *bad = NULL;
        assert(mrc_post_recv(qp, &receive, &bad) == 0 && !bad);
        char done[PATH_MAX], recovering[PATH_MAX];
        path_for(done, directory, "tx.done");
        path_for(recovering, directory, "tx.retrying");
        publish(directory, "rx.ready", NULL, 0);
        uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
        while (access(done, F_OK) != 0) {
            assert(rte_get_timer_cycles() < deadline && "requester did not finish during the blackout test");
            if (infinite && !observed.rail_restored && access(recovering, F_OK) == 0)
                observed.rail_restored = true;
            assert(mrc_dpdk_progress_qp(qp) == 0);
        }
        assert(mrc_dpdk_progress_qp(qp) == 0);
        for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
            assert(rte_eth_remove_rx_callback(port, 0, callbacks[port]) == 0);
            rte_free((void *)callbacks[port]);
        }
        assert(observed.dropped_writes[0] == 1 && observed.dropped_writes[1] == 1);
        const unsigned int both_rails = (1u << DUPLEX_RAILS) - 1;
        assert((observed.first_rails | observed.retry_rails) == both_rails &&
               (observed.first_rails & observed.retry_rails) == 0 &&
               observed.probe_rails_after_write == both_rails);
        assert(observed.recovery_probe == infinite);
        struct ibv_wc wc;
        assert(mrc_poll_cq(cq, 1, &wc) == (int)infinite);
        if (infinite)
            assert(wc.wr_id == 81 && wc.qp_num == local.qpn && wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM &&
                   wc.status == IBV_WC_SUCCESS && wc.byte_len == FIRST_BYTES &&
                   (wc.wc_flags & IBV_WC_WITH_IMM) && ntohl(wc.imm_data) == FIRST_IMMEDIATE);
        assert(mrc_poll_cq(cq, 1, &wc) == 0);
        for (size_t i = 0; i < data_length; i++)
            assert(data[i] == (infinite ? pattern(i) : 0));
        printf("{\"role\":\"rx\",\"qpn\":%u,\"blocked_writes\":[%u,%u],"
               "\"failed_probe_rails\":%u,\"recovered\":%s}\n",
               local.qpn, observed.dropped_writes[0], observed.dropped_writes[1],
               observed.probe_rails_after_write, observed.recovery_probe ? "true" : "false");
    }
    assert(mrc_destroy_qp(qp) == 0);
    assert(mrc_destroy_cq(cq) == 0);
    deregister_memory(context, mr);
}

enum {
    BACKPRESSURE_FIRST_RAIL = COUNTED_RAILS - 2,
    BACKPRESSURE_RETRY_RAIL = COUNTED_RAILS - 3,
};

struct counted_rail_witness {
    uint32_t writes;
    uint32_t retries;
    uint32_t write_rejections;
    uint32_t retry_rejections;
    uint16_t ev_base;
    unsigned int write_evs;
    unsigned int probe_evs;
    bool backpressure;
    bool dropped;
};

static uint16_t backpressure_writes(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                     uint16_t count, void *argument)
{
    (void)queue;
    struct counted_rail_witness *observed = argument;
    assert(count == 1);
    uint8_t prefix[14 + 40 + WIRE_UDP_LEN + WIRE_BTH_LEN];
    struct observed_udp udp = read_udp(packets[0], prefix, sizeof(prefix));
    const uint8_t *bth = udp.header + WIRE_UDP_LEN;
    if (bth[0] < WIRE_WRITE_FIRST || bth[0] > WIRE_WRITE_ONLY_IMM)
        return count;
    uint32_t rail = UINT32_C(1) << port;
    if (bth[8] & 0x20) {
        if (port == BACKPRESSURE_RETRY_RAIL)
            observed->retries |= rail;
        else
            observed->retry_rejections |= rail;
        return port == BACKPRESSURE_RETRY_RAIL;
    }
    if (port == BACKPRESSURE_FIRST_RAIL)
        observed->writes |= rail;
    else
        observed->write_rejections |= rail;
    return port == BACKPRESSURE_FIRST_RAIL;
}

static uint16_t observe_counted_rails(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                     uint16_t count, uint16_t capacity, void *argument)
{
    (void)queue;
    (void)capacity;
    struct counted_rail_witness *observed = argument;
    uint16_t kept = 0;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t prefix[14 + 40 + WIRE_UDP_LEN + WIRE_BTH_LEN];
        struct observed_udp udp = read_udp(packets[i], prefix, sizeof(prefix));
        const uint8_t *bth = udp.header + WIRE_UDP_LEN;
        bool write = bth[0] >= WIRE_WRITE_FIRST && bth[0] <= WIRE_WRITE_ONLY_IMM;
        bool probe = bth[0] == WIRE_PROBE_REQUEST;
        if (observed->ev_base && (write || probe)) {
            assert(udp.entropy >= observed->ev_base);
            unsigned int entry = udp.entropy - observed->ev_base;
            assert(entry < 4 && entry != 2 && port == 1 - entry % 2);
            if (write)
                observed->write_evs |= 1u << entry;
            else
                observed->probe_evs |= 1u << entry;
        }
        if (write) {
            if (bth[8] & 0x20) {
                observed->retries |= UINT32_C(1) << port;
            } else {
                if (observed->backpressure && !observed->dropped && wire_be24(bth + 9) == SEND_PSN) {
                    observed->dropped = true;
                    rte_pktmbuf_free(packets[i]);
                    continue;
                }
                observed->writes |= UINT32_C(1) << port;
            }
        }
        packets[kept++] = packets[i];
    }
    return kept;
}

static void run_counted_route(bool requester, const char *directory, const struct address *address,
                              struct mrc_context *context, struct ibv_pd *pd, struct rte_mempool *pool,
                              bool backpressure, uint16_t ev_base, uint64_t profile_id, bool maximum)
{
    const unsigned int rail_count = maximum ? 32 : ev_base ? DUPLEX_RAILS : COUNTED_RAILS;
    const size_t data_bytes = maximum ? 32 * 1024 + 1 : COUNTED_BYTES;
    struct mrc_cq *cq = mrc_create_cq(context, 1, NULL, NULL, 0);
    assert(cq);
    struct mrc_qp_init_attr init = {
        .pd = pd, .send_cq = requester ? cq : NULL, .recv_cq = requester ? NULL : cq,
        .cap = { .max_send_wr = requester, .max_send_sge = requester, .max_recv_wr = !requester },
    };
    if (!requester)
        skip_local_qp_numbers(context, &init, 1);
    struct mrc_qp *qp = mrc_create_qp(context, &init);
    uint8_t data[data_bytes];
    memset(data, 0, sizeof(data));
    if (requester) {
        for (size_t i = 0; i < sizeof(data); i++)
            data[i] = pattern(i);
    }
    struct ibv_mr *mr = register_memory(context, pd, data, sizeof(data),
                                        requester ? 0 : IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    struct connection local = {
        .gid = address->gid, .psn = requester ? SEND_PSN : 135,
        .remote_address = (uintptr_t)data, .rkey = mr->rkey,
    };
    assert(qp && mrc_get_qpn(qp, &local.qpn) == 0);
    publish(directory, requester ? "tx.connection" : "rx.connection", &local, sizeof(local));
    struct connection peer;
    read_peer_connections(directory, requester, &peer, 1);
    configure_qp(qp, address, &peer, local.psn, requester, !requester,
                 requester ? profile_id : 0, 0, 0, 0, IBV_MTU_1024);

    struct mrc_dpdk_rail rails[32];
    for (uint16_t i = 0; i < rail_count; i++)
        rails[i] = (struct mrc_dpdk_rail){ .port = i, .peer_mac = { 2, 0, 0, 0, 0, (requester ? 0x10 : 0x20) + i } };
    struct mrc_dpdk_route_config route = { .rails = rails, .pool = pool, .requester = requester };
    assert(mrc_dpdk_attach_qp_rails(qp, &route) == EINVAL);
    route.rail_count = 33;
    assert(mrc_dpdk_attach_qp_rails(qp, &route) == EINVAL);
    route.rail_count = rail_count;
    rails[rail_count - 1].port = 0;
    assert(mrc_dpdk_attach_qp_rails(qp, &route) == EINVAL);
    rails[rail_count - 1].port = (uint16_t)(rail_count - 1);
    assert(mrc_dpdk_attach_qp_rails(qp, &route) == 0);
    prepare_memif_rails(rail_count);

    struct counted_rail_witness observed = { .backpressure = backpressure, .ev_base = ev_base };
    const struct rte_eth_rxtx_callback *callbacks[32];
    char ready[PATH_MAX];
    if (requester) {
        publish(directory, "tx.links-ready", NULL, 0);
        path_for(ready, directory, "rx.ready");
        wait_for(ready);
        if (backpressure) {
            for (uint16_t port = 0; port < rail_count; port++) {
                callbacks[port] = rte_eth_add_tx_callback(port, 0, backpressure_writes, &observed);
                assert(callbacks[port]);
            }
        }
        struct ibv_sge sg = { .addr = (uintptr_t)data, .length = sizeof(data), .lkey = mr->lkey };
        struct ibv_send_wr wr = {
            .wr_id = 70, .opcode = IBV_WR_RDMA_WRITE_WITH_IMM, .send_flags = IBV_SEND_SIGNALED,
            .num_sge = 1, .sg_list = &sg, .imm_data = htonl(FIRST_IMMEDIATE),
            .wr.rdma = { .remote_addr = peer.remote_address, .rkey = peer.rkey },
        };
        struct ibv_send_wr *bad;
        assert(mrc_post_send(qp, &wr, &bad) == 0 && !bad);
        if (backpressure) {
            assert(mrc_dpdk_progress_qp(qp) == 0);
            assert(observed.writes && __builtin_popcount(observed.write_rejections) >= 2);
        }
    } else {
        path_for(ready, directory, "tx.links-ready");
        wait_for(ready);
        for (uint16_t port = 0; port < rail_count; port++) {
            callbacks[port] = rte_eth_add_rx_callback(port, 0, observe_counted_rails, &observed);
            assert(callbacks[port]);
        }
        struct ibv_recv_wr wr = { .wr_id = 81 };
        struct ibv_recv_wr *bad;
        assert(mrc_post_recv(qp, &wr, &bad) == 0 && !bad);
        publish(directory, "rx.ready", NULL, 0);
    }

    struct ibv_wc wc;
    uint64_t deadline = rte_get_timer_cycles() + 3 * rte_get_timer_hz();
    int completions;
    do {
        assert(rte_get_timer_cycles() < deadline);
        assert(mrc_dpdk_progress_qp(qp) == 0);
        completions = mrc_poll_cq(cq, 1, &wc);
    } while (completions == 0);
    assert(completions == 1 && wc.qp_num == local.qpn && wc.status == IBV_WC_SUCCESS);
    if (requester) {
        assert(wc.wr_id == 70 && wc.opcode == IBV_WC_RDMA_WRITE);
        if (backpressure) {
            assert(__builtin_popcount(observed.retry_rejections) >= 2);
            for (uint16_t port = 0; port < rail_count; port++) {
                assert(rte_eth_remove_tx_callback(port, 0, callbacks[port]) == 0);
                rte_free((void *)callbacks[port]);
            }
        }
        publish(directory, "tx.done", NULL, 0);
    } else {
        assert(wc.wr_id == 81 && wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM && wc.byte_len == sizeof(data) &&
               (wc.wc_flags & IBV_WC_WITH_IMM) && ntohl(wc.imm_data) == FIRST_IMMEDIATE);
        if (backpressure) {
            assert(observed.dropped && observed.writes == (UINT32_C(1) << BACKPRESSURE_FIRST_RAIL) &&
                   observed.retries == (UINT32_C(1) << BACKPRESSURE_RETRY_RAIL));
        } else {
            assert((observed.writes | observed.retries) == (UINT32_MAX >> (32 - rail_count)));
        }
        if (ev_base)
            assert(observed.write_evs == 0xb && observed.probe_evs == 0xb);
        for (size_t i = 0; i < sizeof(data); i++)
            assert(data[i] == pattern(i));
        path_for(ready, directory, "tx.done");
        wait_for(ready);
        for (uint16_t port = 0; port < rail_count; port++) {
            assert(rte_eth_remove_rx_callback(port, 0, callbacks[port]) == 0);
            rte_free((void *)callbacks[port]);
        }
    }
    assert(mrc_poll_cq(cq, 1, &wc) == 0);
    assert(mrc_destroy_qp(qp) == 0 && mrc_destroy_cq(cq) == 0);
    deregister_memory(context, mr);
    printf("{\"role\":\"%s\",\"qpn\":%u,\"bytes\":%zu,\"observed_rails\":%u,\"retry_rails\":%u,"
           "\"write_rejections\":%u,\"retry_rejections\":%u,\"write_evs\":%u,\"probe_evs\":%u}\n",
           requester ? "tx" : "rx", local.qpn, sizeof(data), observed.writes | observed.retries,
           observed.retries, observed.write_rejections, observed.retry_rejections,
           observed.write_evs, observed.probe_evs);
}

struct duplex_qp {
    struct mrc_qp *qp;
    struct ibv_mr *source;
    struct ibv_mr *target;
    uint8_t sent[DUPLEX_BYTES];
    uint8_t received[DUPLEX_BYTES];
};

static uint8_t duplex_pattern(size_t offset, unsigned int index, unsigned int round, bool client)
{
    return pattern(offset) ^ (uint8_t)(19 * index + 71 * round + 113 * client);
}

static uint32_t duplex_immediate(unsigned int index, unsigned int round, bool client)
{
    return UINT32_C(0xbadc0000) | ((uint32_t)client << 12) | (round << 8) | index;
}

static uint64_t duplex_wr_id(unsigned int index, unsigned int round, bool send)
{
    return 100 * (1 + round) + 10 * send + index;
}

static void post_duplex_recv(struct duplex_qp *qp, unsigned int index, unsigned int round)
{
    struct ibv_recv_wr receive = { .wr_id = duplex_wr_id(index, round, false) };
    struct ibv_recv_wr *bad_recv = NULL;
    assert(mrc_post_recv(qp->qp, &receive, &bad_recv) == 0 && !bad_recv);
}

static void post_duplex_send(struct duplex_qp *qp, const struct connection *peer,
                             unsigned int index, unsigned int round, bool client, uint32_t length)
{
    assert(length <= sizeof(qp->sent));
    for (size_t offset = 0; offset < length; offset++)
        qp->sent[offset] = duplex_pattern(offset, index, round, client);
    struct ibv_sge source = {
        .addr = (uintptr_t)qp->sent, .length = length, .lkey = qp->source->lkey,
    };
    struct ibv_send_wr write = {
        .wr_id = duplex_wr_id(index, round, true), .opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
        .num_sge = 1, .sg_list = &source, .send_flags = IBV_SEND_SIGNALED,
        .imm_data = htonl(duplex_immediate(index, round, client)),
        .wr.rdma = { .remote_addr = peer->remote_address, .rkey = peer->rkey },
    };
    struct ibv_send_wr *bad_send = NULL;
    assert(mrc_post_send(qp->qp, &write, &bad_send) == 0 && !bad_send);
}

static unsigned int duplex_qp_index(const struct connection *connections, uint32_t qpn)
{
    unsigned int index = 0;
    while (index < DUPLEX_QPS && connections[index].qpn != qpn)
        index++;
    assert(index < DUPLEX_QPS);
    return index;
}

static unsigned int duplex_completions(struct mrc_cq *cq, const struct connection *connections,
                                       unsigned int round, unsigned int active, unsigned int completed,
                                       bool client, bool send, uint32_t length)
{
    struct ibv_wc entries[DUPLEX_QPS];
    int count = mrc_poll_cq(cq, DUPLEX_QPS, entries);
    assert(count >= 0);
    for (int i = 0; i < count; i++) {
        const struct ibv_wc *wc = &entries[i];
        unsigned int index = duplex_qp_index(connections, wc->qp_num);
        unsigned int bit = 1u << index;
        assert((active & bit) && !(completed & bit));
        assert(wc->status == IBV_WC_SUCCESS && wc->wr_id == duplex_wr_id(index, round, send));
        if (send) {
            assert(wc->opcode == IBV_WC_RDMA_WRITE);
        } else {
            assert(wc->opcode == IBV_WC_RECV_RDMA_WITH_IMM && wc->byte_len == length);
            assert((wc->wc_flags & IBV_WC_WITH_IMM) &&
                   ntohl(wc->imm_data) == duplex_immediate(index, round, !client));
        }
        completed |= bit;
    }
    return completed;
}

static void marker_name(char out[32], bool client, unsigned int round, const char *state)
{
    int count = snprintf(out, 32, "%s.%u.%s", client ? "tx" : "rx", round, state);
    assert(count > 0 && count < 32);
}

struct cc_window_witness {
    unsigned int writes;
    unsigned int retries;
    uint64_t first_sent_at;
    uint8_t first_opcode;
    uint8_t first_flags;
    uint16_t first_udp_length;
};

static uint16_t observe_write_frames(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                      uint16_t count, void *argument)
{
    (void)port;
    (void)queue;
    struct cc_window_witness *observed = argument;
    for (uint16_t i = 0; i < count; i++) {
        /* Ethernet + IPv6 + UDP + BTH, enough for either plain IP family. */
        uint8_t prefix[14 + 40 + 8 + WIRE_BTH_LEN];
        struct observed_udp udp = read_udp(packets[i], prefix, sizeof(prefix));
        const uint8_t *bth = udp.header + 8;
        if (bth[0] < WIRE_WRITE_FIRST || bth[0] > 0xcb)
            continue;
        if (bth[8] & 0x20) {
            observed->retries++;
            continue;
        }
        if (++observed->writes == 1) {
            observed->first_sent_at = rte_get_timer_cycles();
            observed->first_opcode = bth[0];
            observed->first_flags = bth[8];
            observed->first_udp_length = wire_be16(udp.header + 4);
        }
    }
    return count;
}

static void check_single_full_write(const struct cc_window_witness *observed)
{
    assert(observed->writes == 1 && observed->first_opcode == WIRE_WRITE_FIRST &&
           (observed->first_flags & 0x80));
    const uint16_t full_write_udp = 8 + 12 + 20 + 1024 + 4;
    assert(observed->first_udp_length ==
           full_write_udp + ((observed->first_flags & 0x10) ? 4 : 0));
}

static void check_cc_window(struct mrc_qp *qp, const char *directory, bool client)
{
    const char *marker = "tx.cc-window-observed";
    if (!client) {
        char ready[PATH_MAX];
        path_for(ready, directory, marker);
        wait_for(ready);
        return;
    }

    struct cc_window_witness observed = {0};
    const struct rte_eth_rxtx_callback *callbacks[2];
    for (uint16_t port = 0; port < 2; port++) {
        callbacks[port] = rte_eth_add_tx_callback(port, 0, observe_write_frames, &observed);
        assert(callbacks[port]);
    }

    uint64_t hz = rte_get_timer_hz();
    uint64_t deadline = rte_get_timer_cycles() + 3 * hz;
    do {
        assert(rte_get_timer_cycles() < deadline);
        assert(mrc_dpdk_progress_qp(qp) == 0);
        rte_pause();
    } while (observed.writes == 0);

    /* Continue progressing while the peer withholds all feedback. */
    deadline = rte_get_timer_cycles() + hz / 100;
    do {
        assert(mrc_dpdk_progress_qp(qp) == 0);
        rte_pause();
    } while (rte_get_timer_cycles() < deadline);
    check_single_full_write(&observed);

    for (uint16_t port = 0; port < 2; port++) {
        assert(rte_eth_remove_tx_callback(port, 0, callbacks[port]) == 0);
        /* This thread is the only TX poller, so no callback can still be running. */
        rte_free((void *)callbacks[port]);
    }
    publish(directory, marker, NULL, 0);
}

static void duplex_round(struct duplex_qp *qps, const struct connection *local,
                         const struct connection *peer, struct mrc_cq *send_cq,
                         struct mrc_cq *recv_cq, const char *directory, bool client,
                         unsigned int round, unsigned int first, bool observe_cc_window,
                         bool sends_posted)
{
    unsigned int active = 0;
    for (unsigned int index = first; index < DUPLEX_QPS; index++) {
        post_duplex_recv(&qps[index], index, round);
        if (!sends_posted)
            post_duplex_send(&qps[index], &peer[index], index, round, client, DUPLEX_BYTES);
        active |= 1u << index;
    }

    char marker[32], their_ready[PATH_MAX], their_done[PATH_MAX];
    marker_name(marker, client, round, "ready");
    publish(directory, marker, NULL, 0);
    marker_name(marker, !client, round, "ready");
    path_for(their_ready, directory, marker);
    wait_for(their_ready);
    if (observe_cc_window)
        check_cc_window(qps[0].qp, directory, client);

    marker_name(marker, !client, round, "done");
    path_for(their_done, directory, marker);
    marker_name(marker, client, round, "done");

    uint64_t deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
    unsigned int sends = 0, recvs = 0;
    bool done = false;
    do {
        assert(rte_get_timer_cycles() < deadline);
        for (unsigned int index = first; index < DUPLEX_QPS; index++) {
            int progress = mrc_dpdk_progress_qp(qps[index].qp);
            if (progress) {
                fprintf(stderr, "Round %u, QPN %u progress failed: %s (%d)\n",
                        round, local[index].qpn, strerror(progress), progress);
                abort();
            }
        }
        sends = duplex_completions(send_cq, local, round, active, sends, client, true,
                                   DUPLEX_BYTES);
        recvs = duplex_completions(recv_cq, local, round, active, recvs, client, false,
                                   DUPLEX_BYTES);
        if (!done && sends == active && recvs == active) {
            for (unsigned int index = first; index < DUPLEX_QPS; index++) {
                for (size_t offset = 0; offset < sizeof(qps[index].received); offset++)
                    assert(qps[index].received[offset] == duplex_pattern(offset, index, round, !client));
            }
            publish(directory, marker, NULL, 0);
            done = true;
        }
        rte_pause();
    } while (!done || access(their_done, F_OK) != 0);
}

struct probe_exchange {
    uint16_t token;
    uint32_t entropy;
    bool sent;
    bool replied;
};

struct service_time_witness {
    unsigned int data_qps;
    unsigned int probe_qps;
};

struct rx_flow_witness {
    const struct connection *local;
    const struct connection *peer;
    struct probe_exchange probes[DUPLEX_RAILS][DUPLEX_QPS];
    struct service_time_witness service_time;
    uint8_t expected[DUPLEX_QPS];
    unsigned int data_sacks;
    unsigned int first_writes;
    bool check_startup;
};

static uint16_t observe_flow_tx(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                  uint16_t count, void *argument)
{
    (void)queue;
    struct rx_flow_witness *observed = argument;
    for (uint16_t i = 0; i < count; i++) {
        /* Ethernet, IPv6, UDP, BTH, and the longest relevant extension. */
        uint8_t prefix[14 + 40 + 8 + WIRE_BTH_LEN + WIRE_SETH_LEN + WIRE_CC_STATE_LEN];
        struct observed_udp udp = read_udp(packets[i], prefix, sizeof(prefix));
        const uint8_t *bth = udp.header + 8;
        uint8_t opcode = bth[0];
        if (opcode >= WIRE_WRITE_FIRST && opcode <= 0xcb)
            assert(!(bth[8] & 0x10));
        if (opcode != WIRE_SACK &&
            (!observed->check_startup || (opcode != WIRE_PROBE_REQUEST && opcode != WIRE_WRITE_FIRST)))
            continue;

        uint32_t qpn = wire_be24(bth + 5);
        unsigned int index = duplex_qp_index(observed->peer, qpn);
        const uint8_t *extension = bth + WIRE_BTH_LEN;
        unsigned int bit = 1u << index;
        if (opcode == WIRE_PROBE_REQUEST) {
            assert(udp.bytes >= 8 + WIRE_BTH_LEN + WIRE_PETH_LEN);
            assert(!(extension[15] & 0x0f));
            assert(wire_be16(extension + 8) == (uint16_t)observed->local[index].qpn);
            assert(wire_be16(extension + 10) == (uint16_t)qpn);
            struct probe_exchange *probe = &observed->probes[port][index];
            if (!probe->sent)
                *probe = (struct probe_exchange){
                    .token = wire_be16(extension + 4), .entropy = udp.entropy, .sent = true,
                };
        } else if (opcode == WIRE_WRITE_FIRST) {
            if (!(bth[8] & 0x20) && !(observed->first_writes & bit)) {
                assert(wire_be24(bth + 9) == observed->local[index].psn);
                observed->first_writes |= bit;
            }
        } else {
            assert(udp.bytes >= 8 + WIRE_BTH_LEN + WIRE_SETH_LEN + WIRE_CC_STATE_LEN);
            const uint8_t *cc_state = extension + WIRE_SETH_LEN;
            bool probe = (extension[1] & 0x02) != 0;
            if (cc_state[4] != observed->expected[index]) {
                fprintf(stderr, "QPN %u %sSACK CC State byte was 0x%02x, expected 0x%02x\n",
                        qpn, probe ? "probe " : "", cc_state[4], observed->expected[index]);
                abort();
            }
            if (probe) {
                if (wire_be16(cc_state))
                    observed->service_time.probe_qps |= bit;
            } else {
                observed->data_sacks |= bit;
                if (wire_be16(cc_state))
                    observed->service_time.data_qps |= bit;
            }
        }
    }
    return count;
}

static uint16_t observe_probe_replies(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                      uint16_t count, uint16_t capacity, void *argument)
{
    (void)queue;
    (void)capacity;
    struct rx_flow_witness *observed = argument;
    if (!observed->check_startup)
        return count;

    for (uint16_t i = 0; i < count; i++) {
        uint8_t prefix[14 + 40 + 8 + WIRE_BTH_LEN + WIRE_SETH_LEN];
        struct observed_udp udp = read_udp(packets[i], prefix, sizeof(prefix));
        const uint8_t *bth = udp.header + 8;
        if (bth[0] != WIRE_SACK)
            continue;

        assert(udp.bytes >= 8 + WIRE_BTH_LEN + WIRE_SETH_LEN);
        const uint8_t *seth = bth + WIRE_BTH_LEN;
        if (!(seth[1] & 0x02))
            continue;

        uint32_t qpn = wire_be24(bth + 5);
        unsigned int index = duplex_qp_index(observed->local, qpn);
        struct probe_exchange *probe = &observed->probes[port][index];
        assert(probe->sent);
        assert(wire_be16(seth + 8) == (uint16_t)qpn);
        assert(wire_be16(seth + 10) == (uint16_t)observed->peer[index].qpn);
        if (wire_be16(seth + 2) == probe->token) {
            assert(wire_be32(seth + 4) == probe->entropy);
            probe->replied = true;
        }
    }
    return count;
}

static struct service_time_witness check_rx_flow(struct duplex_qp *qps,
                                                  const struct connection *local,
                                                  const struct connection *peer, struct mrc_cq *send_cq,
                                                  struct mrc_cq *recv_cq, const char *directory, bool client)
{
    static const uint8_t settings[RX_FLOW_ROUNDS][DUPLEX_QPS] = {
        {RX_FLOW_RESTORE | RX_FLOW_PENALTY_MASK, 0},
        {RX_FLOW_RESTORE | RX_FLOW_PENALTY_MASK, 31},
        {RX_FLOW_RESTORE, 31},
        {0, 0},
    };
    const unsigned int both_qps = (1u << DUPLEX_QPS) - 1;
    struct rx_flow_witness observed = {.local = local, .peer = peer, .check_startup = true};
    const struct rte_eth_rxtx_callback *tx_callbacks[DUPLEX_RAILS];
    const struct rte_eth_rxtx_callback *rx_callbacks[DUPLEX_RAILS];
    for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
        tx_callbacks[port] = rte_eth_add_tx_callback(port, 0, observe_flow_tx, &observed);
        rx_callbacks[port] = rte_eth_add_rx_callback(port, 0, observe_probe_replies, &observed);
        assert(tx_callbacks[port] && rx_callbacks[port]);
    }

    prepare_memif_links();
    assert(mrc_dpdk_set_rx_flow_control(NULL, 0, false) == EINVAL);
    for (unsigned int round = 0; round < RX_FLOW_ROUNDS; round++) {
        observed.data_sacks = 0;
        for (unsigned int index = 0; index < DUPLEX_QPS; index++) {
            uint8_t setting = settings[round][index];
            if (setting == observed.expected[index])
                continue;
            assert(mrc_dpdk_set_rx_flow_control(qps[index].qp, setting & RX_FLOW_PENALTY_MASK,
                                               (setting & RX_FLOW_RESTORE) != 0) == 0);
            observed.expected[index] = setting;
        }
        if (round == 0)
            assert(mrc_dpdk_set_rx_flow_control(qps[0].qp, 128, false) == EINVAL);
        duplex_round(qps, local, peer, send_cq, recv_cq, directory, client, round, 0, false, false);
        assert(observed.data_sacks == both_qps);
        observed.check_startup = false;
    }

    assert(observed.first_writes == both_qps);
    assert(observed.service_time.data_qps == both_qps && observed.service_time.probe_qps == both_qps);
    for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
        for (unsigned int index = 0; index < DUPLEX_QPS; index++) {
            if (!observed.probes[port][index].replied) {
                fprintf(stderr, "QPN %u saw no matching initial probe SACK on port %u\n",
                        local[index].qpn, port);
                abort();
            }
        }
        assert(rte_eth_remove_tx_callback(port, 0, tx_callbacks[port]) == 0);
        assert(rte_eth_remove_rx_callback(port, 0, rx_callbacks[port]) == 0);
        /* The only poller removed its callbacks after the last burst. */
        rte_free((void *)tx_callbacks[port]);
        rte_free((void *)rx_callbacks[port]);
    }
    return observed.service_time;
}

enum qa_round {
    QA_BASELINE,
    QA_PRIMER,
    QA_TRIGGER,
    QA_CAPPED,
    QA_ROUNDS,
};

struct qa_feedback {
    uint32_t qpn;
    unsigned int probe_ports;
    unsigned int sacks;
    uint64_t last_sack_at;
    uint32_t cack;
    uint32_t received;
};

struct qa_result {
    unsigned int baseline_burst;
    unsigned int capped_burst;
    uint64_t delayed_sack_us;
};

static uint16_t observe_qa_feedback(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                     uint16_t count, uint16_t capacity, void *argument)
{
    (void)queue;
    (void)capacity;
    struct qa_feedback *feedback = argument;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t prefix[14 + 40 + 8 + WIRE_BTH_LEN + WIRE_SETH_LEN + WIRE_CC_STATE_LEN];
        struct observed_udp udp = read_udp(packets[i], prefix, sizeof(prefix));
        const uint8_t *bth = udp.header + 8;
        assert(bth[0] != 0xdd);
        if (bth[0] != WIRE_SACK)
            continue;

        assert(udp.bytes >= 8 + WIRE_BTH_LEN + WIRE_SETH_LEN + WIRE_CC_STATE_LEN);
        assert(wire_be24(bth + 5) == feedback->qpn && !(bth[8] & 0x20));
        const uint8_t *seth = bth + WIRE_BTH_LEN;
        const uint8_t *cc_state = seth + WIRE_SETH_LEN;
        assert(!(seth[1] & 0x60) && cc_state[4] == 0);
        if (seth[1] & 0x02) {
            feedback->probe_ports |= 1u << port;
        } else {
            feedback->sacks++;
            feedback->last_sack_at = rte_get_timer_cycles();
            feedback->cack = wire_be24(seth + 13);
            feedback->received = wire_be24(cc_state + 5);
        }
    }
    return count;
}

static void stage_qa_write(struct mrc_qp *qp, const char *directory, enum qa_round round,
                           const struct cc_window_witness *window, const struct qa_feedback *feedback,
                           struct qa_result *result)
{
    const uint64_t hz = rte_get_timer_hz();
    const uint64_t deadline = rte_get_timer_cycles() + 4 * hz;
    do {
        assert(rte_get_timer_cycles() < deadline);
        assert(mrc_dpdk_progress_qp(qp) == 0);
    } while (!window->writes);

    if (round == QA_BASELINE) {
        result->baseline_burst = window->writes;
        assert(result->baseline_burst == 3);
    }
    if (round != QA_TRIGGER && round != QA_CAPPED)
        return;

    /* The peer has posted the receive but is waiting to poll these packets. */
    const uint64_t hold = round == QA_TRIGGER ? hz / 100 : hz / 1000;
    while (rte_get_timer_cycles() - window->first_sent_at < hold) {
        assert(mrc_dpdk_progress_qp(qp) == 0);
        rte_pause();
    }
    assert(feedback->sacks == 0 && feedback->probe_ports == 0 && window->retries == 0);
    if (round == QA_CAPPED) {
        result->capped_burst = window->writes;
        check_single_full_write(window);
    } else {
        assert(window->writes == 1 && window->first_opcode == 0xcb &&
               (window->first_flags & 0x80));
    }

    char marker[32];
    marker_name(marker, true, round, "release");
    publish(directory, marker, NULL, 0);
}

static struct qa_result check_quick_adapt(struct duplex_qp *qp, const struct connection *local,
                                          const struct connection *peer, struct mrc_cq *cq,
                                          const char *directory, bool client)
{
    static const uint32_t lengths[QA_ROUNDS] = {
        [QA_BASELINE] = DUPLEX_BYTES, [QA_PRIMER] = QA_PRIMER_BYTES,
        [QA_TRIGGER] = QA_TRIGGER_BYTES, [QA_CAPPED] = DUPLEX_BYTES,
    };
    struct qa_result result = {0};
    struct cc_window_witness window;
    struct qa_feedback feedback;
    const struct rte_eth_rxtx_callback *tx[DUPLEX_RAILS], *rx[DUPLEX_RAILS];
    prepare_memif_links();
    if (client) {
        for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
            tx[port] = rte_eth_add_tx_callback(port, 0, observe_write_frames, &window);
            rx[port] = rte_eth_add_rx_callback(port, 0, observe_qa_feedback, &feedback);
            assert(tx[port] && rx[port]);
        }
    }

    const uint64_t hz = rte_get_timer_hz();
    uint32_t last_received = 0;
    for (enum qa_round round = QA_BASELINE; round < QA_ROUNDS; round++) {
        window = (struct cc_window_witness){0};
        feedback = (struct qa_feedback){.qpn = local[0].qpn};
        const uint32_t length = lengths[round];
        char marker[32], receiver_ready[PATH_MAX], sender_release[PATH_MAX], sender_done[PATH_MAX];
        marker_name(marker, false, round, "ready");
        path_for(receiver_ready, directory, marker);
        if (client) {
            /* The short primer closes the baseline's accounting epoch with a
             * normal RTT sample. The delayed WRITE then runs alone. */
            if (round == QA_PRIMER)
                rte_delay_us_sleep(1500);
            wait_for(receiver_ready);
            post_duplex_send(qp, peer, 0, round, true, length);
            stage_qa_write(qp->qp, directory, round, &window, &feedback, &result);
        } else {
            post_duplex_recv(qp, 0, round);
            publish(directory, marker, NULL, 0);
            if (round == QA_TRIGGER || round == QA_CAPPED) {
                marker_name(marker, true, round, "release");
                path_for(sender_release, directory, marker);
                wait_for(sender_release);
            }
        }

        const uint64_t deadline = rte_get_timer_cycles() + 4 * hz;
        do {
            assert(rte_get_timer_cycles() < deadline);
            assert(mrc_dpdk_progress_qp(qp->qp) == 0);
            rte_pause();
        } while (!duplex_completions(cq, local, round, 1, 0, client, client, length));

        marker_name(marker, true, round, "done");
        path_for(sender_done, directory, marker);
        if (client) {
            const unsigned int packets = (length + 1023) / 1024;
            assert(window.writes == packets && !window.retries && feedback.sacks > 0);
            const uint64_t elapsed = feedback.last_sack_at - window.first_sent_at;
            if (round == QA_BASELINE) {
                assert(feedback.probe_ports == (1u << DUPLEX_RAILS) - 1);
                assert(elapsed < 3 * hz / 1000);
            }
            if (round == QA_PRIMER || round == QA_TRIGGER) {
                assert(!feedback.probe_ports);
                assert(feedback.sacks == 1 && feedback.received == last_received + 1);
            }
            if (round == QA_PRIMER)
                assert(elapsed < 3 * hz / 1000);
            if (round == QA_TRIGGER) {
                assert(feedback.cack == ((local[0].psn + 4) & UINT32_C(0xffffff)));
                assert(elapsed >= hz / 100);
                result.delayed_sack_us = elapsed * UINT64_C(1000000) / hz;
            }
            last_received = feedback.received;
            publish(directory, marker, NULL, 0);
        } else {
            for (size_t offset = 0; offset < length; offset++)
                assert(qp->received[offset] == duplex_pattern(offset, 0, round, true));
            wait_for(sender_done);
        }
    }

    if (client) {
        for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
            assert(rte_eth_remove_tx_callback(port, 0, tx[port]) == 0);
            assert(rte_eth_remove_rx_callback(port, 0, rx[port]) == 0);
            rte_free((void *)tx[port]);
            rte_free((void *)rx[port]);
        }
    }
    return result;
}

enum duplex_mode {
    DUPLEX_NORMAL,
    DUPLEX_RX_FLOW,
    DUPLEX_QA,
    DUPLEX_PKEY_PORT_STATUS,
};

enum {
    STATUS_CLOSE_TOKEN = 0xe123,
    STATUS_OPEN_TOKEN = 0xe124,
};

struct port_status_witness {
    unsigned int close_replies;
    unsigned int open_replies;
};

static uint16_t observe_port_status_traffic(uint16_t port, uint16_t queue, struct rte_mbuf **packets,
                                            uint16_t count, void *argument)
{
    (void)queue;
    struct port_status_witness *observed = argument;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t prefix[RTE_ETHER_HDR_LEN + WIRE_IPV6_LEN + WIRE_UDP_LEN + WIRE_BTH_LEN + 2];
        struct observed_udp udp = read_udp(packets[i], prefix, sizeof(prefix));
        const uint8_t *bth = udp.header + WIRE_UDP_LEN;
        if (bth[0] >= WIRE_WRITE_FIRST && bth[0] <= WIRE_WRITE_ONLY_IMM)
            assert(observed->open_replies && "WRITE sent while the node reports all ports closed");
        if (bth[0] != EP_RESPONSE_OPCODE || wire_be24(bth + 5) != 2)
            continue;
        uint32_t token = wire_be24(bth + 9);
        if (token != STATUS_CLOSE_TOKEN && token != STATUS_OPEN_TOKEN)
            continue;
        assert(port == 0 && (udp.traffic_class >> 2) == EP_CONTROL_DSCP);
        assert(udp.bytes >= WIRE_UDP_LEN + WIRE_BTH_LEN + 2 && bth[WIRE_BTH_LEN + 1] == 0);
        unsigned int *replies = token == STATUS_CLOSE_TOKEN ? &observed->close_replies : &observed->open_replies;
        assert(++*replies == 1);
    }
    return count;
}

static void send_port_status(struct mrc_context *context, const struct mrc_dpdk_endpoint_route *route,
                             uint16_t token, uint32_t mask)
{
    struct mrc_ctl_ep_req request = {
        .req_id = token, .sgid = route->local_gid, .dgid = route->peer_gid,
        .ev_fmt_mode = MRC_CTL_EV_FMT_MODE_UDP, .req_ev = { .port = 1, .val = { 0, 0, 0xd1, 0x10 } },
        .op.port_status_update.port_status = mask,
    };
    struct mrc_ctl_ep_rsp reply;
    int returned;
    assert(mrc_ctl_ep_batch_send_wait(context, EP_CONTROL_DSCP, MRC_CTL_EP_OP_PORT_STATUS_UPDATE,
                                     &request, 1, 1000000000, &reply, &returned) == 0);
    assert(returned == 1 && reply.req_id == token && reply.port == request.req_ev.port);
}

static void progress_status_qps(struct duplex_qp *qps)
{
    for (unsigned int index = 0; index < DUPLEX_QPS; index++)
        assert(mrc_dpdk_progress_qp(qps[index].qp) == 0);
    rte_pause();
}

static void wait_for_status_reply(struct duplex_qp *qps, const unsigned int *replies)
{
    uint64_t deadline = rte_get_timer_cycles() + 3 * rte_get_timer_hz();
    while (!*replies) {
        assert(rte_get_timer_cycles() < deadline);
        progress_status_qps(qps);
    }
}

static unsigned int check_shared_port_status(struct mrc_context *context, const struct mrc_dpdk_route *route,
                                             struct duplex_qp *qps, const struct connection *local,
                                             const struct connection *peer, const char *directory,
                                             bool client, uint16_t key)
{
    prepare_memif_links();
    if (!client) {
        struct mrc_dpdk_endpoint_route status_route = {
            .ports = {route->ports[0], route->ports[1]}, .pool = route->pool,
            .local_gid = local[0].gid, .peer_gid = peer[0].gid, .udp_port = route->udp_port,
            .pkey = key, .peer_pkey = key,
        };
        memcpy(status_route.peer_macs, route->peer_macs, sizeof(status_route.peer_macs));
        assert(mrc_dpdk_attach_endpoint(context, &status_route, NULL) == 0);
        publish(directory, "rx.status-ready", NULL, 0);
        char marker[PATH_MAX];
        path_for(marker, directory, "tx.status-ready");
        wait_for(marker);
        send_port_status(context, &status_route, STATUS_CLOSE_TOKEN, 0);
        path_for(marker, directory, "tx.status-checked");
        wait_for(marker);
        send_port_status(context, &status_route, STATUS_OPEN_TOKEN, 0x6);
        assert(mrc_dpdk_detach_endpoint(context) == 0);
        return 2;
    }

    struct port_status_witness observed = {0};
    const struct rte_eth_rxtx_callback *callbacks[DUPLEX_RAILS];
    for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
        callbacks[port] = rte_eth_add_tx_callback(port, 0, observe_port_status_traffic, &observed);
        assert(callbacks[port]);
    }
    publish(directory, "tx.status-ready", NULL, 0);
    char receiver_ready[PATH_MAX];
    path_for(receiver_ready, directory, "rx.status-ready");
    wait_for(receiver_ready);
    wait_for_status_reply(qps, &observed.close_replies);
    for (unsigned int index = 0; index < DUPLEX_QPS; index++)
        post_duplex_send(&qps[index], &peer[index], index, 0, client, DUPLEX_BYTES);
    uint64_t deadline = rte_get_timer_cycles() + rte_get_timer_hz() / 20;
    while (rte_get_timer_cycles() < deadline)
        progress_status_qps(qps);
    publish(directory, "tx.status-checked", NULL, 0);
    wait_for_status_reply(qps, &observed.open_replies);
    for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
        assert(rte_eth_remove_tx_callback(port, 0, callbacks[port]) == 0);
        rte_free((void *)callbacks[port]);
    }
    return observed.close_replies + observed.open_replies;
}

static void run_duplex(bool client, const char *directory, const struct address *address,
                       struct mrc_context *context, struct ibv_pd *pd, struct rte_mempool *pool,
                       const char *profile_store, uint64_t cc_profile_id, enum duplex_mode mode)
{
    const bool rx_flow = mode == DUPLEX_RX_FLOW;
    const bool quick_adapt = mode == DUPLEX_QA;
    const bool pkey_status = mode == DUPLEX_PKEY_PORT_STATUS;
    const unsigned int unsupported_index = client ? 0 : 1;
    if (rx_flow) {
        struct mrc_device_attr device;
        int supported;
        assert(mrc_query_device_ex(NULL, context, &device, &supported) == 0);
        assert(supported && (device.cap & MRC_DEVICE_CAP_SVC_TIME));
    }
    if (client && cc_profile_id)
        assert(mrc_dpdk_set_ev_store(context, profile_store) == 0);
    struct mrc_cq *send_cq = mrc_create_cq(context, 8, NULL, NULL, 0);
    struct mrc_cq *recv_cq = mrc_create_cq(context, 8, NULL, NULL, 0);
    assert(send_cq && recv_cq);
    struct mrc_qp_init_attr init = {
        .pd = pd, .protocol_version = MRC_PROTOCOL_VERSION_1,
        .send_cq = send_cq, .recv_cq = recv_cq,
        .cap = { .max_send_wr = 2, .max_recv_wr = 2, .max_send_sge = 1 },
    };
    if (!client)
        skip_local_qp_numbers(context, &init, DUPLEX_QPS);

    struct duplex_qp qps[DUPLEX_QPS];
    struct connection local[DUPLEX_QPS];
    const uint32_t psns[2][DUPLEX_QPS] = {{0x1240, 0x6900}, {0xffffff, 0x4100}};
    for (unsigned int index = 0; index < DUPLEX_QPS; index++) {
        struct duplex_qp *qp = &qps[index];
        qp->qp = mrc_create_qp(context, &init);
        assert(qp->qp);
        memset(qp->received, 0, sizeof(qp->received));
        qp->source = register_memory(context, pd, qp->sent, sizeof(qp->sent), 0);
        qp->target = register_memory(context, pd, qp->received, sizeof(qp->received),
                                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        local[index] = (struct connection){
            .gid = address->gid, .psn = psns[client][index],
            .remote_address = (uintptr_t)qp->received, .rkey = qp->target->rkey,
        };
        assert(mrc_get_qpn(qp->qp, &local[index].qpn) == 0);
    }
    publish(directory, client ? "tx.connection" : "rx.connection", local, sizeof(local));
    struct connection peer[DUPLEX_QPS];
    read_peer_connections(directory, client, peer, DUPLEX_QPS);

    struct mrc_dpdk_route route = path_route(pool, client);
    route.bidirectional = true;
    uint16_t key = 0;
    if (pkey_status) {
        assert(ibv_query_pkey(pd->context, address->port, 0, &key) == 0);
        key = ntohs(key);
        assert((key & 0x8000) && (key & 0x7fff));
    }
    for (unsigned int index = 0; index < DUPLEX_QPS; index++) {
        const unsigned int caps = rx_flow && index == unsupported_index
                                  ? MRC_QP_CAP_SVC_TIME_DEST_UNSUPPORTED : 0;
        configure_qp(qps[index].qp, address, &peer[index], local[index].psn, true, true, 0,
                     client ? cc_profile_id : 0, caps, 0, IBV_MTU_1024);
        if (rx_flow) {
            struct mrc_qp_attr attr;
            assert(mrc_query_qp(qps[index].qp, NULL, 0, &attr, MRC_QP_CAP, NULL) == 0);
            assert(attr.cap == caps);
            assert(mrc_dpdk_set_rx_flow_control(qps[index].qp, 1, false) == ENOTCONN);
        }
        if (pkey_status) {
            if (index == 0) {
                route.peer_pkey = (key & 0x7fff) == 1 ? 0x8002 : 0x8001;
                assert(mrc_dpdk_attach_qp(qps[index].qp, &route) == EACCES);
            }
            route.peer_pkey = index == 0 ? 0 : key;
        }
        attach_qp(qps[index].qp, &route);
    }

    struct qa_result qa = {0};
    struct service_time_witness service_time = {0};
    unsigned int status_replies = 0;
    if (pkey_status)
        status_replies = check_shared_port_status(context, &route, qps, local, peer, directory, client, key);
    if (quick_adapt)
        qa = check_quick_adapt(&qps[0], local, peer, client ? send_cq : recv_cq, directory, client);
    else if (rx_flow)
        service_time = check_rx_flow(qps, local, peer, send_cq, recv_cq, directory, client);
    else
        duplex_round(qps, local, peer, send_cq, recv_cq, directory, client, 0, 0,
                     cc_profile_id != 0, pkey_status && client);

    struct ibv_qp_attr reset = { .qp_state = IBV_QPS_RESET };
    assert(mrc_modify_qp(qps[0].qp, &reset, IBV_QP_STATE, NULL, 0) == 0);
    assert(mrc_dpdk_progress_qp(qps[0].qp) == ENOTCONN);
    if (rx_flow) {
        assert(mrc_dpdk_set_rx_flow_control(qps[0].qp, 1, false) == ENOTCONN);
    } else if (!quick_adapt) {
        duplex_round(qps, local, peer, send_cq, recv_cq, directory, client, 1, 1, false, false);
        for (size_t offset = 0; offset < sizeof(qps[0].received); offset++)
            assert(qps[0].received[offset] == duplex_pattern(offset, 0, 0, !client));
    }

    for (unsigned int index = 0; index < DUPLEX_QPS; index++) {
        assert(mrc_destroy_qp(qps[index].qp) == 0);
        deregister_memory(context, qps[index].target);
        deregister_memory(context, qps[index].source);
    }
    assert(mrc_destroy_cq(send_cq) == 0);
    assert(mrc_destroy_cq(recv_cq) == 0);
    if (quick_adapt) {
        printf("{\"role\":\"%s\",\"qpn\":%u,\"bytes\":%u,\"completions\":%u,"
               "\"baseline_burst\":%u,\"capped_burst\":%u,\"delayed_sack_us\":%llu}\n",
               client ? "tx" : "rx", local[0].qpn, (unsigned int)QA_BYTES, (unsigned int)QA_ROUNDS,
               qa.baseline_burst, qa.capped_burst, (unsigned long long)qa.delayed_sack_us);
        return;
    }
    unsigned int completions = rx_flow ? RX_FLOW_ROUNDS * DUPLEX_QPS : 3;
    printf("{\"role\":\"%s\",\"bytes\":%u,\"send_completions\":%u,\"recv_completions\":%u,"
           "\"qpns\":[%u,%u],\"reset_qpn\":%u",
           client ? "tx" : "rx", completions * DUPLEX_BYTES, completions, completions,
           local[0].qpn, local[1].qpn, local[0].qpn);
    if (rx_flow) {
        printf(",\"service_time_dest_unsupported_qpn\":%u,\"service_time_data_qps\":%u,"
               "\"service_time_probe_qps\":%u", local[unsupported_index].qpn,
               (unsigned int)__builtin_popcount(service_time.data_qps),
               (unsigned int)__builtin_popcount(service_time.probe_qps));
    }
    if (pkey_status)
        printf(",\"port_status_replies\":%u", status_replies);
    puts("}");
}

int main(int argc, char **argv)
{
    int consumed = rte_eal_init(argc, argv);
    assert(consumed >= 0);
    argc -= consumed;
    argv += consumed;
    assert(argc >= 4 && (strcmp(argv[1], "tx") == 0 || strcmp(argv[1], "rx") == 0));
    bool client = strcmp(argv[1], "tx") == 0;
    const char *directory = argv[2];
    bool pending_teardown = !strcmp(argv[3], "reset_pending") || !strcmp(argv[3], "destroy_pending") || !strcmp(argv[3], "error_pending") || !strcmp(argv[3], "error_pending_overflow");
    bool maximum_rails = strcmp(argv[3], "maximum_rails") == 0;
    bool endpoint_ops = strcmp(argv[3], "endpoint_ops") == 0;
    bool wimm_capacity = !strcmp(argv[3], "wimm_capacity");
    unsigned int boundary_pmtu = wimm_capacity ? 1024 : !strcmp(argv[3], "boundaries_256") ? 256 :
        !strcmp(argv[3], "boundaries_512") ? 512 : !strcmp(argv[3], "boundaries_1024") ? 1024 : 0;
    bool infinite_retry = strcmp(argv[3], "blackout_infinite") == 0;
    bool blackout = infinite_retry || strcmp(argv[3], "blackout_finite") == 0;
    bool service_level_dscp = strcmp(argv[3], "service_level_dscp") == 0;
    bool last_hop_trim = service_level_dscp || strcmp(argv[3], "last_hop_trim") == 0;
    bool dynamic_mpr = strcmp(argv[3], "dynamic_mpr") == 0;
    bool dynamic_mpr_rts = strcmp(argv[3], "dynamic_mpr_rts") == 0;
    bool mpr_updates = dynamic_mpr || dynamic_mpr_rts || strcmp(argv[3], "static_mpr") == 0;
    bool cc_capacity = !strcmp(argv[3], "cc_capacity");
    bool cc_profile = strcmp(argv[3], "cc_profile") == 0;
    bool qa_profile = strcmp(argv[3], "qa_profile") == 0;
    bool rx_flow = strcmp(argv[3], "rx_flow_control") == 0;
    bool pkey_status = strcmp(argv[3], "pkey_port_status") == 0;
    bool bidirectional = cc_profile || qa_profile || rx_flow || pkey_status || strcmp(argv[3], "duplex") == 0;
    enum duplex_mode mode = qa_profile ? DUPLEX_QA : rx_flow ? DUPLEX_RX_FLOW :
        pkey_status ? DUPLEX_PKEY_PORT_STATUS : DUPLEX_NORMAL;
    enum simplex_error error_mode = SIMPLEX_NO_ERROR;
    if (!strcmp(argv[3], "wire_error"))
        error_mode = SIMPLEX_CQ_OVERRUN;
    else if (!strcmp(argv[3], "wire_request_error"))
        error_mode = SIMPLEX_REQUEST_ERROR;
    else if (!strcmp(argv[3], "wire_access_error"))
        error_mode = SIMPLEX_ACCESS_ERROR;
    bool inline_mode = strcmp(argv[3], "inline") == 0;
    bool generated_stev = strcmp(argv[3], "generated_ev_stev") == 0 ||
        strcmp(argv[3], "generated_ev_custom_stev") == 0;
    bool generated_ev = generated_stev || strcmp(argv[3], "generated_ev_udp") == 0;
    bool srv6_transit = strcmp(argv[3], "srv6_transit") == 0;
    bool explicit_srv6 = strcmp(argv[3], "explicit_ev_srv6") == 0;
    bool generated_srv6 = strcmp(argv[3], "generated_ev_srv6_srh") == 0;
    bool mapped_srv6 = strcmp(argv[3], "mapped_ev_srv6") == 0;
    bool srv6 = explicit_srv6 || generated_srv6 || mapped_srv6 || srv6_transit || strcmp(argv[3], "srv6") == 0;
    bool backpressure = strcmp(argv[3], "rail_backpressure") == 0;
    bool counted_route = maximum_rails || backpressure || strcmp(argv[3], "counted_route") == 0;
    uint16_t mapped_ev_base = 0;
    if (!strcmp(argv[3], "mapped_ev_udp"))
        mapped_ev_base = 0xc610;
    else if (!strcmp(argv[3], "mapped_ev_auto"))
        mapped_ev_base = 0xc000;
    bool ev_profile = generated_ev || explicit_srv6 || generated_srv6 || mapped_srv6 || mapped_ev_base ||
                      strcmp(argv[3], "ev_profile") == 0;
    enum simplex_wire_mode wire_mode = SIMPLEX_WIRE_NONE;
    if (inline_mode)
        wire_mode = SIMPLEX_INLINE;
    else if (strcmp(argv[3], "pmtu_256") == 0)
        wire_mode = SIMPLEX_PMTU_256;
    else if (strcmp(argv[3], "pmtu_512") == 0)
        wire_mode = SIMPLEX_PMTU_512;
    else if (generated_ev)
        wire_mode = SIMPLEX_GENERATED_EV;
    else if (explicit_srv6)
        wire_mode = SIMPLEX_SRV6_EXPLICIT_EV;
    else if (generated_srv6)
        wire_mode = SIMPLEX_SRV6_GENERATED_EV;
    else if (mapped_srv6)
        wire_mode = SIMPLEX_SRV6_MAPPED_EV;
    else if (srv6)
        wire_mode = srv6_transit ? SIMPLEX_SRV6_TRANSIT : SIMPLEX_SRV6;
    const char *profile_store = NULL;
    uint64_t profile_id = 0;
    if (endpoint_ops) {
        assert(argc == 5);
    } else if (ev_profile || cc_profile || qa_profile || cc_capacity) {
        assert(argc == 6);
        profile_store = argv[4];
        char *end;
        errno = 0;
        profile_id = strtoull(argv[5], &end, 10);
        assert(!errno && profile_id && !*end);
    } else {
        assert(argc == 4 && (pending_teardown || boundary_pmtu || counted_route || bidirectional || blackout || last_hop_trim || mpr_updates ||
                             error_mode != SIMPLEX_NO_ERROR ||
                             wire_mode != SIMPLEX_WIRE_NONE || strcmp(argv[3], "simplex") == 0));
    }

    bool endpoint_shared = endpoint_ops && !strcmp(argv[4], "shared");
    bool counted_endpoint = endpoint_ops && !strcmp(argv[4], "counted");
    struct address address = {0};
    struct ibv_context *verbs = open_verbs(&address, generated_stev || srv6 || endpoint_shared || last_hop_trim);
    if (!verbs) {
        fputs("A verbs device with a usable Ethernet GID and pkey is required.\n", stderr);
        return EXIT_FAILURE;
    }
    bool need_pd = !endpoint_ops || (endpoint_shared && !client);
    struct ibv_pd *pd = need_pd ? ibv_alloc_pd(verbs) : NULL;
    struct mrc_context *context = mrc_create_context(verbs, NULL);
    assert((!need_pd || pd) && context);
    if (mapped_ev_base && client)
        assert(mrc_dpdk_set_ev_store(context, profile_store) == 0);

    unsigned int rail_count = maximum_rails ? 32 : counted_route || counted_endpoint ? COUNTED_RAILS : DUPLEX_RAILS;
    struct rte_mempool *pool = mrc_dpdk_packet_pool_for_ports(0, rail_count);
    assert(pool);
    for (unsigned int port = 0; port < rail_count; port++) {
        struct rte_ether_addr local;
        assert(mrc_dpdk_open_port(port, pool, &local) == 0);
    }

    if (cc_capacity)
        run_cc_capacity(client, directory, &address, context, pd, pool, profile_store, profile_id);
    else if (pending_teardown)
        run_pending_teardown(client, directory, &address, context, pd, pool, argv[3]);
    else if (boundary_pmtu)
        run_boundaries(client, directory, &address, context, pd, pool, boundary_pmtu, wimm_capacity);
    else if (counted_endpoint)
        run_counted_endpoint(client, directory, context, pool);
    else if (endpoint_ops)
        run_endpoint_ops(client, directory, argv[4], &address, verbs, context, pd, pool);
    else if (counted_route || mapped_ev_base)
        run_counted_route(client, directory, &address, context, pd, pool, backpressure, mapped_ev_base, profile_id, maximum_rails);
    else if (mpr_updates) {
        enum single_write_case scenario = dynamic_mpr_rts ? DYNAMIC_MPR_RTS : dynamic_mpr ? DYNAMIC_MPR : STATIC_MPR;
        run_single_write(client, directory, &address, context, pd, pool, scenario);
    } else if (blackout || last_hop_trim) {
        enum single_write_case scenario = service_level_dscp ? SERVICE_LEVEL_DSCP : last_hop_trim ? LAST_HOP_TRIM :
            infinite_retry ? BLACKOUT_INFINITE : BLACKOUT_FINITE;
        run_single_write(client, directory, &address, context, pd, pool, scenario);
    } else if (bidirectional)
        run_duplex(client, directory, &address, context, pd, pool, profile_store, profile_id, mode);
    else
        run_simplex(client, directory, &address, context, pd, pool, error_mode, profile_store,
                    profile_id, wire_mode);

    assert(mrc_destroy_context(context) == 0);
    if (pd)
        assert(ibv_dealloc_pd(pd) == 0);
    assert(ibv_close_device(verbs) == 0);
    /* The memif client disconnects before its server closes shared rings. */
    if (maximum_rails && !client) {
        char stopped[PATH_MAX];
        path_for(stopped, directory, "tx.ports-closed");
        wait_for(stopped);
    }
    for (unsigned int port = 0; port < rail_count; port++)
        mrc_dpdk_close_port(port);
    if (maximum_rails && client)
        publish(directory, "tx.ports-closed", NULL, 0);
    rte_mempool_free(pool);
    assert(rte_eal_cleanup() == 0);
    return 0;
}
