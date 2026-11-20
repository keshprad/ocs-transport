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
    WIRE_PROBE_REQUEST = 0xde,
    WIRE_SACK = 0xdc,
    WIRE_WRITE_FIRST = 0xc6,
    WIRE_BTH_LEN = 12,
    WIRE_PETH_LEN = 16,
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
                          unsigned int qp_caps)
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
        .qp_state = IBV_QPS_RTR, .path_mtu = IBV_MTU_1024,
        .dest_qp_num = peer->qpn, .rq_psn = peer->psn,
        .ah_attr = {
            .is_global = 1, .port_num = address->port,
            .grh = { .dgid = peer->gid, .sgid_index = address->gid_index, .hop_limit = 64 },
        },
    };
    attrs = (struct mrc_qp_attr){
        .mpr.mpr_dest = 1, .wimm.max_wimm_dest = receive ? 1 : 0, .cap = qp_caps,
    };
    const int rtr_verbs = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN;
    const int rtr_attrs = MRC_QP_MPR_DEST | MRC_QP_MAX_WIMM_DEST | (qp_caps ? MRC_QP_CAP : 0);
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

static void post_receives(struct mrc_qp *qp)
{
    struct ibv_recv_wr credits[] = {{ .wr_id = 81 }, { .wr_id = 82 }};
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

static void post_rejected_write(struct mrc_qp *qp, const struct connection *peer,
                                 const struct ibv_mr *source)
{
    struct ibv_sge sg = {
        .addr = (uintptr_t)source->addr, .length = FIRST_BYTES, .lkey = source->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id = 70, .opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
        .send_flags = IBV_SEND_SIGNALED, .num_sge = 1, .sg_list = &sg,
        .wr.rdma = { .remote_addr = peer->remote_address, .rkey = peer->rkey },
        .imm_data = htonl(FIRST_IMMEDIATE),
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
    return (struct observed_udp){.header = frame + udp, .bytes = length - udp, .entropy = entropy};
}

static void prepare_memif_links(void)
{
    uint64_t deadline = rte_get_timer_cycles() + 4 * rte_get_timer_hz();
    for (uint16_t port = 0; port < DUPLEX_RAILS; port++) {
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

enum simplex_wire_mode {
    SIMPLEX_WIRE_NONE,
    SIMPLEX_INLINE,
    SIMPLEX_GENERATED_EV,
    SIMPLEX_SRV6,
    SIMPLEX_SRV6_TRANSIT,
    SIMPLEX_SRV6_EXPLICIT_EV,
    SIMPLEX_SRV6_GENERATED_EV,
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
        configure_qp(idle_qp, address, &peer, 0, false, true, 0, 0, 0);
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
               observed->mode == SIMPLEX_SRV6_GENERATED_EV) {
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
    if ((observed->probe_ports | observed->data_ports) & bit)
        assert(observed->entropy[port] == entropy);
    else
        observed->entropy[port] = entropy;
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
    const bool srv6_ev = wire_mode == SIMPLEX_SRV6_EXPLICIT_EV ||
                         wire_mode == SIMPLEX_SRV6_GENERATED_EV;
    const bool srv6 = srv6_ev || wire_mode == SIMPLEX_SRV6 || wire_mode == SIMPLEX_SRV6_TRANSIT;
    const bool transit = wire_mode == SIMPLEX_SRV6_TRANSIT;
    const bool inspect_wire = wire_mode != SIMPLEX_WIRE_NONE && !inline_mode;
    if (requester && ev_profile_id)
        assert(mrc_dpdk_set_ev_store(context, ev_store) == 0);
    struct mrc_comp_channel *channel = NULL;
    if (!requester && !wire_error)
        assert(mrc_create_comp_channel(context, &channel) == 0);
    struct mrc_cq *cq = mrc_create_cq(context, !requester && cq_overrun ? 1 : 4, NULL, channel, 0);
    assert(cq);

    uint8_t first_data[TOTAL_BYTES], last_data[TOTAL_BYTES];
    int access_flags = 0;
    if (!requester) {
        access_flags = IBV_ACCESS_LOCAL_WRITE;
        if (error_mode != SIMPLEX_ACCESS_ERROR)
            access_flags |= IBV_ACCESS_REMOTE_WRITE;
    }
    size_t first_length = requester ? FIRST_BYTES + IMMEDIATE_FIRST_SGE_BYTES : TOTAL_BYTES;
    struct ibv_mr *first = NULL;
    struct ibv_mr *last = NULL;
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

    struct mrc_qp_init_attr init = {
        .pd = pd, .protocol_version = MRC_PROTOCOL_VERSION_1,
        .send_cq = requester ? cq : NULL, .recv_cq = requester ? NULL : cq,
        .cap = {
            .max_send_wr = requester ? 3 : 0,
            .max_send_sge = requester ? 2 : 0,
            .max_recv_wr = requester ? 0 : 2,
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
                 requester ? ev_profile_id : 0, 0, 0);
    if (!requester && error_mode == SIMPLEX_REQUEST_ERROR) {
        /* The peer offers one WRITEIMM at a time. This responder has
         * no slots for immediate data, exercising an OOB mismatch. */
        struct mrc_qp_attr attrs = { .wimm.max_wimm_dest = 0 };
        assert(mrc_modify_qp(qp, NULL, 0, &attrs, MRC_QP_MAX_WIMM_DEST) == 0);
    }

    struct mrc_dpdk_route route = path_route(pool, requester);
    route.requester = requester;
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
        for (uint16_t port = 0; port < DUPLEX_RAILS && (!inline_mode || !requester); port++) {
            callbacks[port] = inline_mode ?
                rte_eth_add_rx_callback(port, 0, drop_inline_fragment, &observed_inline) :
                rte_eth_add_rx_callback(port, 0, observe_simplex_wire, &observed);
            assert(callbacks[port]);
        }
    }

    if (requester) {
        char ready[PATH_MAX];
        path_for(ready, directory, "rx.ready");
        wait_for(ready);
        if (rejected_write)
            post_rejected_write(qp, &peer, first);
        else if (inline_mode)
            post_inline_writes(qp, &peer);
        else
            post_registered_writes(qp, &peer, first, last);
    } else {
        post_receives(qp);
        if (!wire_error)
            assert(mrc_req_notify_cq(cq, 1) == 0);
        publish(directory, "rx.ready", NULL, 0);
    }
    if (ev_profile_id && !generated_ev && wire_mode != SIMPLEX_SRV6_GENERATED_EV)
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
    }
    if (inline_mode && !requester)
        assert(observed_inline.dropped && observed_inline.retransmitted);
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
        if (srv6_ev)
            printf(",\"rail_entropy\":[%u,%u]", observed.entropy[0], observed.entropy[1]);
        if (inline_mode && !requester)
            printf(",\"inline_fragment_retransmitted\":%s", observed_inline.retransmitted ? "true" : "false");
        puts("}");
    }
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
                         unsigned int round, unsigned int first, bool observe_cc_window)
{
    unsigned int active = 0;
    for (unsigned int index = first; index < DUPLEX_QPS; index++) {
        post_duplex_recv(&qps[index], index, round);
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
        duplex_round(qps, local, peer, send_cq, recv_cq, directory, client, round, 0, false);
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
};

static void run_duplex(bool client, const char *directory, const struct address *address,
                       struct mrc_context *context, struct ibv_pd *pd, struct rte_mempool *pool,
                       const char *profile_store, uint64_t cc_profile_id, enum duplex_mode mode)
{
    const bool rx_flow = mode == DUPLEX_RX_FLOW;
    const bool quick_adapt = mode == DUPLEX_QA;
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
    for (unsigned int index = 0; index < DUPLEX_QPS; index++) {
        const unsigned int caps = rx_flow && index == unsupported_index
                                  ? MRC_QP_CAP_SVC_TIME_DEST_UNSUPPORTED : 0;
        configure_qp(qps[index].qp, address, &peer[index], local[index].psn, true, true, 0,
                     client ? cc_profile_id : 0, caps);
        if (rx_flow) {
            struct mrc_qp_attr attr;
            assert(mrc_query_qp(qps[index].qp, NULL, 0, &attr, MRC_QP_CAP, NULL) == 0);
            assert(attr.cap == caps);
            assert(mrc_dpdk_set_rx_flow_control(qps[index].qp, 1, false) == ENOTCONN);
        }
        attach_qp(qps[index].qp, &route);
    }

    struct qa_result qa = {0};
    struct service_time_witness service_time = {0};
    if (quick_adapt)
        qa = check_quick_adapt(&qps[0], local, peer, client ? send_cq : recv_cq, directory, client);
    else if (rx_flow)
        service_time = check_rx_flow(qps, local, peer, send_cq, recv_cq, directory, client);
    else
        duplex_round(qps, local, peer, send_cq, recv_cq, directory, client, 0, 0, cc_profile_id != 0);

    struct ibv_qp_attr reset = { .qp_state = IBV_QPS_RESET };
    assert(mrc_modify_qp(qps[0].qp, &reset, IBV_QP_STATE, NULL, 0) == 0);
    assert(mrc_dpdk_progress_qp(qps[0].qp) == ENOTCONN);
    if (rx_flow) {
        assert(mrc_dpdk_set_rx_flow_control(qps[0].qp, 1, false) == ENOTCONN);
    } else if (!quick_adapt) {
        duplex_round(qps, local, peer, send_cq, recv_cq, directory, client, 1, 1, false);
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
    bool endpoint_ops = strcmp(argv[3], "endpoint_ops") == 0;
    bool cc_profile = strcmp(argv[3], "cc_profile") == 0;
    bool qa_profile = strcmp(argv[3], "qa_profile") == 0;
    bool rx_flow = strcmp(argv[3], "rx_flow_control") == 0;
    bool bidirectional = cc_profile || qa_profile || rx_flow || strcmp(argv[3], "duplex") == 0;
    enum duplex_mode mode = qa_profile ? DUPLEX_QA : rx_flow ? DUPLEX_RX_FLOW : DUPLEX_NORMAL;
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
    bool srv6 = explicit_srv6 || generated_srv6 || srv6_transit || strcmp(argv[3], "srv6") == 0;
    bool ev_profile = generated_ev || explicit_srv6 || generated_srv6 || strcmp(argv[3], "ev_profile") == 0;
    enum simplex_wire_mode wire_mode = SIMPLEX_WIRE_NONE;
    if (inline_mode)
        wire_mode = SIMPLEX_INLINE;
    else if (generated_ev)
        wire_mode = SIMPLEX_GENERATED_EV;
    else if (explicit_srv6)
        wire_mode = SIMPLEX_SRV6_EXPLICIT_EV;
    else if (generated_srv6)
        wire_mode = SIMPLEX_SRV6_GENERATED_EV;
    else if (srv6)
        wire_mode = srv6_transit ? SIMPLEX_SRV6_TRANSIT : SIMPLEX_SRV6;
    const char *profile_store = NULL;
    uint64_t profile_id = 0;
    if (endpoint_ops) {
        assert(argc == 5);
    } else if (ev_profile || cc_profile || qa_profile) {
        assert(argc == 6);
        profile_store = argv[4];
        char *end;
        errno = 0;
        profile_id = strtoull(argv[5], &end, 10);
        assert(!errno && profile_id && !*end);
    } else {
        assert(argc == 4 && (bidirectional || error_mode != SIMPLEX_NO_ERROR || srv6 || inline_mode ||
                             strcmp(argv[3], "simplex") == 0));
    }

    bool endpoint_shared = endpoint_ops && !strcmp(argv[4], "shared");
    struct address address = {0};
    struct ibv_context *verbs = open_verbs(&address, generated_stev || srv6 || endpoint_shared);
    if (!verbs) {
        fputs("A verbs device with a usable Ethernet GID and pkey is required.\n", stderr);
        return EXIT_FAILURE;
    }
    bool need_pd = !endpoint_ops || (endpoint_shared && !client);
    struct ibv_pd *pd = need_pd ? ibv_alloc_pd(verbs) : NULL;
    struct mrc_context *context = mrc_create_context(verbs, NULL);
    assert((!need_pd || pd) && context);

    struct rte_mempool *pool = mrc_dpdk_packet_pool();
    assert(pool);
    for (unsigned int port = 0; port < 2; port++) {
        struct rte_ether_addr local;
        assert(mrc_dpdk_open_port(port, pool, &local) == 0);
    }

    if (endpoint_ops)
        run_endpoint_ops(client, directory, argv[4], &address, verbs, context, pd, pool);
    else if (bidirectional)
        run_duplex(client, directory, &address, context, pd, pool, profile_store, profile_id, mode);
    else
        run_simplex(client, directory, &address, context, pd, pool, error_mode, profile_store,
                    profile_id, wire_mode);

    assert(mrc_destroy_context(context) == 0);
    if (pd)
        assert(ibv_dealloc_pd(pd) == 0);
    assert(ibv_close_device(verbs) == 0);
    for (unsigned int port = 0; port < 2; port++)
        mrc_dpdk_close_port(port);
    rte_mempool_free(pool);
    assert(rte_eal_cleanup() == 0);
    return 0;
}
