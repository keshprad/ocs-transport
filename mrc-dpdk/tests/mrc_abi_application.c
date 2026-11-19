#include "dpdk_io.h"
#include "mrc_dpdk.h"

#include <arpa/inet.h>
#include <assert.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ether.h>
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

static struct ibv_context *open_verbs(struct address *address)
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

static struct connection peer_connection(const char *directory, bool requester)
{
    char path[PATH_MAX];
    path_for(path, directory, requester ? "rx.connection" : "tx.connection");
    wait_for(path);
    FILE *file = fopen(path, "rb");
    assert(file);
    struct connection peer;
    assert(fread(&peer, sizeof(peer), 1, file) == 1);
    assert(fclose(file) == 0);
    return peer;
}

static void configure_qp(struct mrc_qp *qp, const struct address *address,
                          const struct connection *peer, bool requester)
{
    struct ibv_qp_attr verbs = {
        .qp_state = IBV_QPS_INIT,
        .qp_access_flags = requester ? 0 : IBV_ACCESS_REMOTE_WRITE,
        .pkey_index = 0,
    };
    assert(mrc_modify_qp(qp, &verbs, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_ACCESS_FLAGS,
                         NULL, 0) == 0);

    verbs = (struct ibv_qp_attr){
        .qp_state = IBV_QPS_RTR, .path_mtu = IBV_MTU_1024,
        .dest_qp_num = peer->qpn, .rq_psn = peer->psn,
        .ah_attr = {
            .is_global = 1, .port_num = address->port,
            .grh = { .dgid = peer->gid, .sgid_index = address->gid_index, .hop_limit = 64 },
        },
    };
    struct mrc_qp_attr attrs = { .mpr.mpr_dest = 1, .wimm.max_wimm_dest = requester ? 0 : 1 };
    const int rtr_verbs = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN;
    assert(mrc_modify_qp(qp, &verbs, rtr_verbs, &attrs, MRC_QP_MPR_DEST | MRC_QP_MAX_WIMM_DEST) == 0);
    if (!requester)
        return;

    verbs.qp_state = IBV_QPS_RTS;
    verbs.sq_psn = SEND_PSN;
    attrs = (struct mrc_qp_attr){
        .mpr.mpr = 1, .wimm.max_wimm = 1, .timeout = 16,
        .retry = { .linear_cnt = 3, .exp_cnt = 4 },
    };
    const int rts_attrs = MRC_QP_MPR | MRC_QP_MAX_WIMM | MRC_QP_TIMEOUT | MRC_QP_RETRY_CNT;
    assert(mrc_modify_qp(qp, &verbs, IBV_QP_STATE | IBV_QP_SQ_PSN, &attrs, rts_attrs) == 0);
}

static void post_writes(struct mrc_qp *qp, const struct connection *peer,
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
    struct ibv_send_wr wr[] = {
        {
            .wr_id = 70, .opcode = IBV_WR_RDMA_WRITE, .num_sge = 1, .sg_list = &sg[0],
            .wr.rdma = { .remote_addr = peer->remote_address, .rkey = peer->rkey },
        },
        {
            .wr_id = 71, .opcode = IBV_WR_RDMA_WRITE_WITH_IMM, .send_flags = IBV_SEND_SIGNALED,
            .num_sge = 2, .sg_list = &sg[1], .imm_data = htonl(FIRST_IMMEDIATE),
            .wr.rdma = { .remote_addr = peer->remote_address + FIRST_BYTES, .rkey = peer->rkey },
        },
        {
            .wr_id = 72, .opcode = IBV_WR_RDMA_WRITE_WITH_IMM, .send_flags = IBV_SEND_SIGNALED,
            .imm_data = htonl(SECOND_IMMEDIATE),
            .wr.rdma = { .remote_addr = peer->remote_address + TOTAL_BYTES, .rkey = peer->rkey },
        },
    };
    wr[0].next = &wr[1];
    wr[1].next = &wr[2];
    struct ibv_send_wr *bad = NULL;
    assert(mrc_post_send(qp, wr, &bad) == 0 && !bad);
}

static void post_receives(struct mrc_qp *qp)
{
    struct ibv_recv_wr credits[] = {{ .wr_id = 81 }, { .wr_id = 82 }};
    credits[0].next = &credits[1];
    struct ibv_recv_wr *bad = NULL;
    assert(mrc_post_recv(qp, credits, &bad) == 0 && !bad);
}

static void complete(struct mrc_qp *qp, struct mrc_cq *cq, const char *directory,
                     bool requester, uint32_t qpn)
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

int main(int argc, char **argv)
{
    int consumed = rte_eal_init(argc, argv);
    assert(consumed >= 0);
    argc -= consumed;
    argv += consumed;
    assert(argc == 3 && (strcmp(argv[1], "tx") == 0 || strcmp(argv[1], "rx") == 0));
    bool requester = strcmp(argv[1], "tx") == 0;
    const char *directory = argv[2];

    struct address address = {0};
    struct ibv_context *verbs = open_verbs(&address);
    if (!verbs) {
        fputs("A verbs device with a usable Ethernet GID and pkey is required.\n", stderr);
        return EXIT_FAILURE;
    }
    struct ibv_pd *pd = ibv_alloc_pd(verbs);
    struct mrc_context *context = mrc_create_context(verbs, NULL);
    struct mrc_comp_channel *channel = NULL;
    if (!requester)
        assert(mrc_create_comp_channel(context, &channel) == 0);
    struct mrc_cq *cq = mrc_create_cq(context, 4, NULL, channel, 0);
    assert(pd && context && cq);

    uint8_t first_data[TOTAL_BYTES], last_data[TOTAL_BYTES];
    int access_flags = requester ? 0 : IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    size_t first_length = requester ? FIRST_BYTES + IMMEDIATE_FIRST_SGE_BYTES : TOTAL_BYTES;
    for (size_t i = 0; i < first_length; i++)
        first_data[i] = requester ? pattern(i) : 0;
    struct ibv_mr *first = ibv_reg_mr(pd, first_data, first_length, access_flags);
    assert(first && mrc_dpdk_bind_mr(context, first, (uintptr_t)first_data, access_flags) == 0);
    struct ibv_mr *last = NULL;
    if (requester) {
        size_t length = TOTAL_BYTES - first_length;
        for (size_t i = 0; i < length; i++)
            last_data[i] = pattern(first_length + i);
        last = ibv_reg_mr(pd, last_data, length, 0);
        assert(last && mrc_dpdk_bind_mr(context, last, (uintptr_t)last_data, 0) == 0);
    }

    struct rte_mempool *pool = mrc_dpdk_packet_pool();
    assert(pool);
    for (unsigned int port = 0; port < 2; port++) {
        struct rte_ether_addr local;
        assert(mrc_dpdk_open_port(port, pool, &local) == 0);
    }

    struct mrc_qp_init_attr init = {
        .pd = pd, .protocol_version = MRC_PROTOCOL_VERSION_1,
        .send_cq = requester ? cq : NULL, .recv_cq = requester ? NULL : cq,
        .cap = {
            .max_send_wr = requester ? 3 : 0,
            .max_send_sge = requester ? 2 : 0,
            .max_recv_wr = requester ? 0 : 2,
        },
    };
    /* Allocate different QPNs even though both this test's processes start fresh. */
    if (!requester) {
        struct mrc_qp *unused = mrc_create_qp(context, &init);
        assert(unused && mrc_destroy_qp(unused) == 0);
    }
    struct mrc_qp *qp = mrc_create_qp(context, &init);
    struct connection local = {
        .gid = address.gid, .psn = requester ? SEND_PSN : 135,
        .remote_address = requester ? 0 : (uintptr_t)first_data,
        .rkey = requester ? 0 : first->rkey,
    };
    assert(qp && mrc_get_qpn(qp, &local.qpn) == 0);
    publish(directory, requester ? "tx.connection" : "rx.connection", &local, sizeof(local));
    struct connection peer = peer_connection(directory, requester);
    assert(local.qpn != peer.qpn);
    configure_qp(qp, &address, &peer, requester);

    struct mrc_dpdk_route route = { .ports = {0, 1}, .pool = pool, .requester = requester };
    uint8_t peer_mac_end = requester ? 0x10 : 0x20;
    for (unsigned int port = 0; port < 2; port++) {
        route.peer_macs[port][0] = 2;
        route.peer_macs[port][5] = peer_mac_end + port;
    }
    int attached = mrc_dpdk_attach_qp(qp, &route);
    if (attached) {
        fprintf(stderr, "Failed to attach QP: %s (%d)\n", strerror(attached), attached);
        return EXIT_FAILURE;
    }

    if (requester) {
        char ready[PATH_MAX];
        path_for(ready, directory, "rx.ready");
        wait_for(ready);
        post_writes(qp, &peer, first, last);
    } else {
        post_receives(qp);
        assert(mrc_req_notify_cq(cq, 0) == 0);
        publish(directory, "rx.ready", NULL, 0);
    }
    complete(qp, cq, directory, requester, local.qpn);
    if (requester) {
        publish(directory, "tx.done", NULL, 0);
    } else {
        for (size_t i = 0; i < TOTAL_BYTES; i++)
            assert(first_data[i] == pattern(i));
        int fd;
        assert(mrc_get_comp_channel_fd(channel, &fd) == 0);
        struct pollfd wait = { .fd = fd, .events = POLLIN };
        assert(poll(&wait, 1, 0) == 1 && (wait.revents & POLLIN));
        struct mrc_cq *event = NULL;
        void *event_context = NULL;
        assert(mrc_get_cq_event(channel, &event, &event_context) == 0 && event == cq);
        mrc_ack_cq_events(cq, 1);
    }

    assert(mrc_destroy_qp(qp) == 0);
    assert(mrc_destroy_cq(cq) == 0);
    if (channel)
        assert(mrc_destroy_comp_channel(channel) == 0);
    if (last) {
        assert(mrc_dpdk_unbind_mr(context, last) == 0);
        assert(ibv_dereg_mr(last) == 0);
    }
    assert(mrc_dpdk_unbind_mr(context, first) == 0);
    assert(ibv_dereg_mr(first) == 0);
    assert(mrc_destroy_context(context) == 0);
    assert(ibv_dealloc_pd(pd) == 0);
    assert(ibv_close_device(verbs) == 0);
    for (unsigned int port = 0; port < 2; port++)
        mrc_dpdk_close_port(port);
    rte_mempool_free(pool);
    assert(rte_eal_cleanup() == 0);
    printf("{\"role\":\"%s\",\"bytes\":%u,\"completions\":2,\"qpn\":%u}\n",
           requester ? "tx" : "rx", (unsigned int)TOTAL_BYTES, local.qpn);
    return 0;
}
