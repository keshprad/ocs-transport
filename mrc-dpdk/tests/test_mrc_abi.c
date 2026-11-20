#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "mrc_abi.h"
#include "mrc_ev_event_io.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <mrc_ctl.h>
#include <poll.h>
#include <rdma/ib_user_verbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { NO_ASYNC_OPEN_OVERRIDE = -1 };

static int next_native_async_fd = NO_ASYNC_OPEN_OVERRIDE;

int __real_mrc_verbs_async_open(const struct ibv_context *verbs);

int __wrap_mrc_verbs_async_open(const struct ibv_context *verbs)
{
    if (next_native_async_fd == NO_ASYNC_OPEN_OVERRIDE)
        return __real_mrc_verbs_async_open(verbs);
    int fd = next_native_async_fd;
    next_native_async_fd = NO_ASYNC_OPEN_OVERRIDE;
    return fd;
}

static void transition_to_rts(struct mrc_provider_qp *qp)
{
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_RESET, MRC_PROVIDER_QP_INIT) == 0);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_INIT, MRC_PROVIDER_QP_RTR) == 0);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_RTR, MRC_PROVIDER_QP_RTS) == 0);
}

struct waiter {
    struct mrc_comp_channel *channel;
    struct mrc_cq *cq;
    void *context;
    int result;
};

static void *get_event(void *argument)
{
    struct waiter *waiter = argument;
    waiter->result = mrc_get_cq_event(waiter->channel, &waiter->cq, &waiter->context);
    return NULL;
}

static void test_resources_and_completions(struct ibv_context *verbs)
{
    struct mrc_context_attr version = { .mrc_api_version_used = MRC_API_VER(2, 0, 0) };
    assert(mrc_create_context(verbs, &version) == NULL && errno == EPROTONOSUPPORT);
    uint32_t current, oldest, vendor, vendor_oldest;
    mrc_query_version(&current, &oldest, &vendor, &vendor_oldest);
    assert(current == MRC_API_VER(1, 0, 1) && oldest == MRC_API_VER(1, 0, 0));
    assert(vendor == MRC_API_VER(1, 0, 0) && vendor_oldest == vendor);
    version.mrc_api_version_used = current;
    version.mrc_vendor_api_version_used = vendor;
    struct mrc_context *context = mrc_create_context(verbs, &version);
    struct mrc_context *other = mrc_create_context(verbs, NULL);
    assert(context && other);
    struct mrc_device_attr device;
    int supported;
    assert(mrc_query_device(verbs, &device, &supported) == 0);
    if (supported) {
        assert(device.mrc_protocol_version == MRC_PROTOCOL_VERSION_1);
        assert(device.mpr_attr.mpr_resolution == 1 && device.mpr_attr.max_mpr == MRC_ABI_MAX_MPR);
        assert(device.qp_attr.max_qp_hint == MRC_ABI_MAX_QP_HINTS);
    }
    struct mrc_device_attr same;
    int same_support;
    assert(mrc_query_device_ex(NULL, context, &same, &same_support) == 0);
    assert(same_support == supported && same.mrc_protocol_version == device.mrc_protocol_version);
    assert(same.qp_attr.max_qp_hint == device.qp_attr.max_qp_hint);
    struct mrc_comp_channel *channel = NULL;
    assert(mrc_create_comp_channel(context, &channel) == 0 && channel);
    assert(mrc_destroy_context(context) == -1 && errno == EBUSY);
    assert(mrc_create_cq(other, 1, NULL, channel, 0) == NULL && errno == EINVAL);
    assert(mrc_create_cq(context, 1, NULL, channel, verbs->num_comp_vectors) == NULL && errno == EINVAL);
    assert(mrc_create_cq(context, 0, NULL, channel, 0) == NULL && errno == EINVAL);
    assert(mrc_destroy_context(other) == 0);

    int descriptor;
    assert(mrc_get_comp_channel_fd(channel, &descriptor) == 0 && descriptor >= 0);
    int flags = fcntl(descriptor, F_GETFL);
    assert(flags >= 0 && !(flags & O_NONBLOCK));
    struct pollfd wait = { .fd = descriptor, .events = POLLIN };
    assert(poll(&wait, 1, 0) == 0);

    int application_context = 42;
    struct mrc_cq *send = mrc_create_cq(context, 64, NULL, NULL, 0);
    struct mrc_cq *recv = mrc_create_cq(context, 2, &application_context, channel, 0);
    assert(send && recv);
    assert(mrc_destroy_comp_channel(channel) == EBUSY);
    assert(mrc_poll_cq(recv, 0, NULL) == 0);
    assert(mrc_poll_cq(recv, 1, NULL) == -1 && errno == EINVAL);
    const struct mrc_provider_qp_config config = {
        .send_cq = send->provider, .recv_cq = recv->provider, .qpn = 0x201,
        .send_capacity = 64, .recv_capacity = 1, .sq_sig_all = true,
    };
    struct mrc_provider_qp *qp = NULL;
    assert(mrc_provider_qp_create(&config, &qp) == 0);
    assert(mrc_destroy_cq(recv) == EBUSY);
    transition_to_rts(qp);

    assert(mrc_req_notify_cq(recv, 1) == 0);
    struct waiter operation = { .channel = channel, .result = -2 };
    pthread_t consumer;
    assert(pthread_create(&consumer, NULL, get_event, &operation) == 0);
    uint64_t receive_id;
    assert(mrc_provider_qp_post_recv(qp, 0xa1) == 0);
    assert(mrc_provider_qp_complete_recv(qp, 42, 0x01020304, true, &receive_id) == 0);
    assert(receive_id == 0xa1);
    assert(pthread_join(consumer, NULL) == 0);
    assert(operation.result == 0 && operation.cq == recv && operation.context == &application_context);
    mrc_ack_cq_events(recv, 1);
    assert(poll(&wait, 1, 0) == 0);
    assert(fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0);
    struct mrc_cq *event_cq = NULL;
    void *event_context = NULL;
    assert(mrc_get_cq_event(channel, &event_cq, &event_context) == -1 && errno == EAGAIN);
    assert(!event_cq && !event_context);

    struct ibv_wc entries[64];
    assert(mrc_poll_cq(recv, 64, entries) == 1);
    assert(entries[0].status == IBV_WC_SUCCESS && entries[0].opcode == IBV_WC_RECV_RDMA_WITH_IMM);
    assert(entries[0].wr_id == 0xa1 && entries[0].qp_num == 0x201 && entries[0].byte_len == 42);
    assert(entries[0].wc_flags == IBV_WC_WITH_IMM && entries[0].imm_data == htonl(0x01020304));

    uint64_t sequence;
    for (unsigned int i = 0; i < 40; i++)
        assert(mrc_provider_qp_post_send(qp, 0x100 + i, i, false, &sequence) == 0);
    assert(mrc_provider_qp_complete_send(qp, sequence) == 0);
    assert(mrc_poll_cq(send, 64, entries) == 40);
    for (unsigned int i = 0; i < 40; i++) {
        assert(entries[i].status == IBV_WC_SUCCESS && entries[i].opcode == IBV_WC_RDMA_WRITE);
        assert(entries[i].wr_id == 0x100 + i && entries[i].qp_num == 0x201 && entries[i].byte_len == i);
        assert(entries[i].wc_flags == 0);
    }

    uint64_t failure;
    assert(mrc_provider_qp_post_send(qp, 0xf1, 10, false, &failure) == 0);
    assert(mrc_provider_qp_post_send(qp, 0xf2, 20, false, &sequence) == 0);
    assert(mrc_provider_qp_fail(qp, failure, MRC_PROVIDER_WC_RETRY_EXCEEDED) == 0);
    assert(mrc_poll_cq(send, 64, entries) == 2);
    assert(entries[0].status == IBV_WC_RETRY_EXC_ERR && entries[0].wr_id == 0xf1);
    assert(entries[1].status == IBV_WC_WR_FLUSH_ERR && entries[1].wr_id == 0xf2);
    assert(mrc_provider_qp_destroy(qp) == 0);
    assert(mrc_destroy_cq(send) == 0);
    assert(mrc_destroy_cq(recv) == 0);
    assert(mrc_destroy_comp_channel(channel) == 0);
    assert(mrc_destroy_context(context) == 0);
}

static void test_verbs_memory_binding(struct ibv_context *verbs)
{
    struct mrc_context *context = mrc_create_context(verbs, NULL);
    struct ibv_pd *verbs_pd = ibv_alloc_pd(verbs);
    struct ibv_pd *other_pd = ibv_alloc_pd(verbs);
    assert(context && verbs_pd && other_pd);
    unsigned char memory[64] = "external verbs registration";
    struct ibv_mr *mr = ibv_reg_mr(verbs_pd, memory, sizeof(memory),
                                  IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    assert(mr);
    const uint64_t iova = (uintptr_t)memory;
    struct mrc_provider_pd *provider;
    assert(mrc_provider_pd_create(2, &provider) == 0);
    assert(mrc_abi_bind_mr(context, provider, other_pd, mr, iova, 0) == -EINVAL);
    assert(mrc_abi_bind_mr(context, provider, verbs_pd, mr, iova, IBV_ACCESS_REMOTE_WRITE) == -EINVAL);
    assert(mrc_abi_bind_mr(context, provider, verbs_pd, mr, iova,
                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE) == 0);

    struct mrc_cq *send = mrc_create_cq(context, 2, NULL, NULL, 0);
    assert(send);
    const struct mrc_provider_qp_config config = {
        .pd = provider, .send_cq = send->provider, .qpn = 0x202, .send_capacity = 2,
    };
    struct mrc_provider_qp *qp;
    assert(mrc_provider_qp_create(&config, &qp) == 0);
    transition_to_rts(qp);
    const struct mrc_provider_write write = {
        .wr_id = 831, .local_iova = iova + 4, .lkey = mr->lkey, .length = 8,
        .remote_address = 0x1000, .rkey = 0x24601, .signaled = true,
    };
    uint64_t sequence;
    assert(mrc_provider_qp_post_write(qp, &write, &sequence) == 0);
    struct mrc_provider_write_view view;
    assert(mrc_provider_qp_get_write(qp, sequence, &view) == 0);
    assert(view.data == memory + 4 && view.length == 8 && view.rkey == 0x24601);

    const struct mrc_provider_mr_access access = {
        .iova = iova + 32, .key = mr->rkey, .length = 4, .operation = MRC_PROVIDER_MR_WRITE_REMOTE,
    };
    struct mrc_provider_mr_lease destination = {0};
    assert(mrc_provider_mr_acquire(provider, &access, &destination) == 0);
    assert(destination.address == memory + 32);
    assert(mrc_provider_mr_deregister(provider, mr->lkey) == -EBUSY);
    assert(mrc_provider_qp_complete_send(qp, sequence) == 0);
    struct ibv_wc wc;
    assert(mrc_poll_cq(send, 1, &wc) == 1);
    assert(wc.status == IBV_WC_SUCCESS && wc.wr_id == 831 && wc.byte_len == 8);
    assert(mrc_provider_mr_deregister(provider, mr->lkey) == -EBUSY);
    assert(mrc_provider_mr_release(&destination) == 0);
    assert(mrc_provider_mr_deregister(provider, mr->lkey) == 0);
    assert(ibv_dereg_mr(mr) == 0);
    assert(mrc_provider_qp_destroy(qp) == 0);
    assert(mrc_provider_pd_destroy(provider) == 0);
    assert(mrc_destroy_cq(send) == 0);
    assert(ibv_dealloc_pd(other_pd) == 0);
    assert(ibv_dealloc_pd(verbs_pd) == 0);
    assert(mrc_destroy_context(context) == 0);
}

static void test_qp_hints(struct ibv_context *verbs)
{
    const int fields = (int)(MRC_QP_HINT_NUM_QPS_PER_PEER | MRC_QP_HINT_NUM_SEND_PEERS |
                         MRC_QP_HINT_NUM_REMOTE_RECV_PEERS | MRC_QP_HINT_VENDOR_CFG);
    struct mrc_context *context = mrc_create_context(verbs, NULL);
    struct mrc_context *other = mrc_create_context(verbs, NULL);
    assert(context && other);

    struct mrc_qp_hint_init_attr hint_init = {
        .attr = { .num_qps_per_peer = 2, .num_send_peers = 4, .num_remote_recv_peers = -1 },
    };
    assert(mrc_create_qp_hint(context, &hint_init) == NULL && errno == EINVAL);
    hint_init.attr.num_remote_recv_peers = 8;
    hint_init.attr.vendor_cfg[0] = 0x12;
    struct mrc_qp_hint *hint = mrc_create_qp_hint(context, &hint_init);
    struct mrc_qp_hint *replacement = mrc_create_qp_hint(context, &hint_init);
    struct mrc_qp_hint *foreign = mrc_create_qp_hint(other, &hint_init);
    assert(hint && replacement && foreign);
    assert(mrc_destroy_context(context) == -1 && errno == EBUSY);
    assert(mrc_destroy_context(other) == -1 && errno == EBUSY);

    struct mrc_qp_hint_attr attrs = { .num_send_peers = -1 };
    assert(mrc_query_qp_hint(hint, &attrs, 1 << 10) == -1 && errno == EINVAL);
    assert(mrc_query_qp_hint(hint, &attrs, MRC_QP_HINT_NUM_QPS_PER_PEER) == 0);
    assert(attrs.num_qps_per_peer == 2 && attrs.num_send_peers == -1);
    assert(mrc_modify_qp_hint(hint, &attrs, MRC_QP_HINT_NUM_SEND_PEERS) == -1 && errno == EINVAL);
    attrs.num_remote_recv_peers = 0;
    attrs.vendor_cfg[0] = 0x34;
    assert(mrc_modify_qp_hint(hint, &attrs, (int)(MRC_QP_HINT_NUM_REMOTE_RECV_PEERS |
                                               MRC_QP_HINT_VENDOR_CFG)) == 0);
    assert(mrc_query_qp_hint(hint, &attrs, fields) == 0);
    assert(attrs.num_qps_per_peer == 2 && attrs.num_send_peers == 4 &&
           attrs.num_remote_recv_peers == 0 && attrs.vendor_cfg[0] == 0x34);

    struct ibv_pd *pd = ibv_alloc_pd(verbs);
    struct mrc_cq *cq = mrc_create_cq(context, 2, NULL, NULL, 0);
    assert(pd && cq);
    struct mrc_qp_init_attr qp_init = { .pd = pd, .send_cq = cq, .cap = { .max_send_wr = 1 } };
    struct mrc_qp *first = mrc_create_qp(context, &qp_init);
    struct mrc_qp *second = mrc_create_qp(context, &qp_init);
    assert(first && second);
    struct ibv_qp_attr verbs_attr = { .qp_state = IBV_QPS_INIT };
    struct mrc_qp_attr qp_attrs = { .qp_hint = foreign };
    assert(mrc_modify_qp(first, &verbs_attr, IBV_QP_STATE, &qp_attrs, MRC_QP_HINT) == EINVAL);
    qp_attrs.qp_hint = hint;
    assert(mrc_modify_qp(first, &verbs_attr, IBV_QP_STATE, &qp_attrs, MRC_QP_HINT) == 0);
    assert(mrc_modify_qp(second, &verbs_attr, IBV_QP_STATE, &qp_attrs, MRC_QP_HINT) == 0);
    assert(mrc_destroy_qp_hint(hint) == -1 && errno == EBUSY);

    attrs.num_send_peers = 6;
    assert(mrc_modify_qp_hint(hint, &attrs, MRC_QP_HINT_NUM_SEND_PEERS) == 0);
    assert(mrc_query_qp_hint(hint, &attrs, fields) == 0 && attrs.num_send_peers == 6);
    qp_attrs.qp_hint = foreign;
    assert(mrc_modify_qp(first, NULL, 0, &qp_attrs, MRC_QP_HINT) == EINVAL);
    assert(mrc_query_qp(first, NULL, 0, &qp_attrs, MRC_QP_HINT, NULL) == 0 && qp_attrs.qp_hint == hint);
    assert(mrc_destroy_qp_hint(foreign) == 0);
    assert(mrc_destroy_context(other) == 0);

    qp_attrs.qp_hint = replacement;
    assert(mrc_modify_qp(first, NULL, 0, &qp_attrs, MRC_QP_HINT) == 0);
    assert(mrc_destroy_qp_hint(hint) == -1 && errno == EBUSY);
    assert(mrc_destroy_qp(second) == 0);
    assert(mrc_destroy_qp_hint(hint) == 0);
    assert(mrc_destroy_qp_hint(replacement) == -1 && errno == EBUSY);
    qp_attrs.qp_hint = NULL;
    assert(mrc_modify_qp(first, NULL, 0, &qp_attrs, MRC_QP_HINT) == 0);
    assert(mrc_query_qp(first, NULL, 0, &qp_attrs, MRC_QP_HINT, NULL) == 0 && !qp_attrs.qp_hint);
    qp_attrs.qp_hint = replacement;
    assert(mrc_modify_qp(first, NULL, 0, &qp_attrs, MRC_QP_HINT) == 0);
    verbs_attr.qp_state = IBV_QPS_RESET;
    assert(mrc_modify_qp(first, &verbs_attr, IBV_QP_STATE, NULL, 0) == 0);
    assert(mrc_query_qp(first, NULL, 0, &qp_attrs, MRC_QP_HINT, NULL) == 0 && !qp_attrs.qp_hint);
    assert(mrc_destroy_qp_hint(replacement) == 0);
    assert(mrc_destroy_qp(first) == 0 && mrc_destroy_cq(cq) == 0 && ibv_dealloc_pd(pd) == 0);

    struct mrc_qp_hint *hints[MRC_ABI_MAX_QP_HINTS];
    for (size_t i = 0; i < MRC_ABI_MAX_QP_HINTS; i++) {
        hints[i] = mrc_create_qp_hint(context, &hint_init);
        assert(hints[i]);
    }
    assert(mrc_create_qp_hint(context, &hint_init) == NULL && errno == ENOSPC);
    for (size_t i = MRC_ABI_MAX_QP_HINTS; i > 0; i--)
        assert(mrc_destroy_qp_hint(hints[i - 1]) == 0);
    assert(mrc_destroy_context(context) == 0);
    puts("OCP MRC QP hint attributes, sharing, and lifetime tests passed");
}

static void test_public_qp(struct ibv_context *verbs)
{
    struct mrc_context *context = mrc_create_context(verbs, NULL);
    assert(context);
    struct ibv_pd *pd = ibv_alloc_pd(verbs);
    struct ibv_pd *foreign_pd = ibv_alloc_pd(verbs);
    assert(pd && foreign_pd);
    unsigned char first[] = {1, 2, 3};
    unsigned char last[] = {4, 5};
    struct ibv_mr *first_mr = ibv_reg_mr(pd, first, sizeof(first), 0);
    struct ibv_mr *last_mr = ibv_reg_mr(pd, last, sizeof(last), 0);
    struct ibv_mr *foreign_mr = ibv_reg_mr(foreign_pd, first, sizeof(first), 0);
    assert(first_mr && last_mr && foreign_mr);
    assert(mrc_dpdk_bind_mr(context, first_mr, (uintptr_t)first, 0) == 0);
    assert(mrc_dpdk_bind_mr(context, last_mr, (uintptr_t)last, 0) == 0);
    assert(mrc_dpdk_bind_mr(context, foreign_mr, (uintptr_t)first, 0) == 0);
    assert(mrc_dpdk_bind_mr(context, first_mr, (uintptr_t)first, 0) == EEXIST);

    struct mrc_cq *cq = mrc_create_cq(context, 8, NULL, NULL, 0);
    assert(cq);
    struct mrc_qp_init_attr init = {
        .qp_context = &init, .send_cq = cq, .recv_cq = cq, .pd = pd,
        .cap = { .max_send_wr = 2, .max_recv_wr = 1, .max_send_sge = 2, .max_recv_sge = 1 },
        .send_ops_flags = IBV_QP_EX_WITH_RDMA_WRITE | IBV_QP_EX_WITH_RDMA_WRITE_WITH_IMM,
    };
    struct mrc_qp_init_attr unsupported = init;
    unsupported.send_ops_flags |= IBV_QP_EX_WITH_RDMA_READ;
    assert(mrc_create_qp(context, &unsupported) == NULL && errno == EOPNOTSUPP);
    struct mrc_qp *qp = mrc_create_qp(context, &init);
    assert(qp && init.protocol_version == MRC_PROTOCOL_VERSION_1);
    struct mrc_qp *second_qp = mrc_create_qp(context, &init);
    assert(second_qp);
    uint32_t qpn, other_qpn;
    assert(mrc_get_qpn(qp, &qpn) == 0 && qpn > 2);
    assert(mrc_get_qpn(second_qp, &other_qpn) == 0 && other_qpn != qpn);
    assert(mrc_destroy_qp(second_qp) == 0);
    assert(mrc_destroy_cq(cq) == EBUSY);

    struct ibv_qp_attr vattr = { .qp_state = IBV_QPS_RTS };
    assert(mrc_modify_qp(qp, &vattr, IBV_QP_STATE, NULL, 0) == EINVAL);
    struct mrc_qp_init_attr actual;
    assert(mrc_query_qp(qp, &vattr, IBV_QP_STATE | IBV_QP_CAP, NULL, 0, &actual) == 0);
    assert(vattr.qp_state == IBV_QPS_RESET && vattr.cap.max_send_sge == 2);
    assert(actual.qp_context == &init && actual.pd == pd);
    assert(actual.send_ops_flags == init.send_ops_flags && vattr.cap.max_recv_sge == 1);
    vattr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_INIT, .qp_access_flags = IBV_ACCESS_REMOTE_WRITE };
    assert(mrc_modify_qp(qp, &vattr, IBV_QP_STATE | IBV_QP_ACCESS_FLAGS, NULL, 0) == 0);

    struct ibv_sge recv_sge = { .addr = (uintptr_t)first, .length = sizeof(first), .lkey = first_mr->lkey };
    struct ibv_recv_wr recv[] = {
        { .wr_id = 71, .sg_list = &recv_sge, .num_sge = 1 },
        { .wr_id = 72, .num_sge = 1 },
    };
    struct ibv_recv_wr *bad_recv;
    recv[0].next = &recv[1];
    assert(mrc_post_recv(qp, recv, &bad_recv) == -1 && errno == EINVAL && bad_recv == &recv[1]);
    recv[1].sg_list = &recv_sge;
    recv[1].num_sge = 2;
    assert(mrc_post_recv(qp, &recv[1], &bad_recv) == -1 && errno == EINVAL && bad_recv == &recv[1]);
    recv[1].num_sge = 0;
    assert(mrc_post_recv(qp, &recv[1], &bad_recv) == -1 && errno == ENOSPC && bad_recv == &recv[1]);

    struct ibv_device_attr device;
    union ibv_gid gid;
    uint8_t port = 1;
    assert(ibv_query_device(verbs, &device) == 0 && device.phys_port_cnt >= port);
    assert(ibv_query_gid(verbs, port, 0, &gid) == 0);
    vattr = (struct ibv_qp_attr){
        .qp_state = IBV_QPS_RTR, .cur_qp_state = IBV_QPS_RESET,
        .path_mtu = IBV_MTU_1024, .dest_qp_num = other_qpn, .rq_psn = 777,
        .ah_attr = { .is_global = 1, .port_num = port, .grh = { .dgid = gid } },
    };
    const int rtr_mask = IBV_QP_STATE | IBV_QP_CUR_STATE | IBV_QP_PATH_MTU |
        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_AV;
    struct mrc_qp_attr attr = {
        .mpr.mpr_dest = 3, .wimm.max_wimm_dest = 1, .cap = MRC_QP_CAP_DYNAMIC_MPR,
    };
    const int rtr_attrs = MRC_QP_MPR_DEST | MRC_QP_MAX_WIMM_DEST | MRC_QP_CAP;
    assert(mrc_modify_qp(qp, &vattr, rtr_mask, &attr, rtr_attrs) == EINVAL);
    vattr.cur_qp_state = IBV_QPS_INIT;
    assert(mrc_modify_qp(qp, &vattr, rtr_mask, &attr, rtr_attrs) == 0);

    vattr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTS, .sq_psn = 0xffffff };
    attr = (struct mrc_qp_attr){
        .wimm.max_wimm = 2, .mpr.mpr = 1, .timeout = 13, .retry = { .linear_cnt = 2, .exp_cnt = 6 },
    };
    const int rts_attrs = MRC_QP_MAX_WIMM | MRC_QP_MPR | MRC_QP_TIMEOUT | MRC_QP_RETRY_CNT;
    attr.timeout = 25;
    assert(mrc_modify_qp(qp, &vattr, IBV_QP_STATE | IBV_QP_SQ_PSN, &attr, rts_attrs) == EINVAL);
    attr.timeout = 13;
    assert(mrc_modify_qp(qp, &vattr, IBV_QP_STATE | IBV_QP_SQ_PSN, &attr, rts_attrs) == 0);
    assert(mrc_query_qp(qp, &vattr, rtr_mask | IBV_QP_SQ_PSN, &attr, rts_attrs | rtr_attrs, NULL) == 0);
    assert(vattr.qp_state == IBV_QPS_RTS && vattr.cur_qp_state == IBV_QPS_RTS);
    assert(vattr.sq_psn == 0xffffff && vattr.rq_psn == 777 && vattr.dest_qp_num == other_qpn);
    assert(attr.wimm.max_wimm == 2 && attr.wimm.max_wimm_dest == 1 &&
           attr.mpr.mpr == 1 && attr.mpr.mpr_dest == 3 && attr.timeout == 13 &&
           attr.retry.linear_cnt == 2 && attr.retry.exp_cnt == 6);

    struct ibv_sge sg[] = {
        { .addr = (uintptr_t)first, .length = sizeof(first), .lkey = first_mr->lkey },
        { .addr = (uintptr_t)last, .length = sizeof(last), .lkey = last_mr->lkey },
    };
    struct ibv_sge wrong_pd = { .addr = (uintptr_t)first, .length = 1, .lkey = foreign_mr->lkey };
    struct ibv_send_wr send[] = {
        { .wr_id = 81, .opcode = IBV_WR_RDMA_WRITE_WITH_IMM, .sg_list = sg, .num_sge = 2,
          .imm_data = htonl(0x1234abcd), .send_flags = IBV_SEND_SIGNALED,
          .wr.rdma = {.remote_addr = 0x10000, .rkey = 0x24680} },
        { .wr_id = 82, .opcode = IBV_WR_RDMA_WRITE, .sg_list = &wrong_pd, .num_sge = 1 },
        { .wr_id = 83, .opcode = IBV_WR_RDMA_WRITE, .send_flags = IBV_SEND_SIGNALED },
    };
    send[0].next = &send[1];
    send[1].next = &send[2];
    struct ibv_send_wr *bad_send;
    assert(mrc_post_send(qp, send, &bad_send) == EACCES && bad_send == &send[1]);
    assert(mrc_dpdk_unbind_mr(context, first_mr) == EBUSY);
    struct mrc_provider_qp_attr provider;
    assert(mrc_provider_qp_query(qp->provider, &provider) == 0 && provider.send_pending == 1);
    struct mrc_provider_write_view write;
    assert(mrc_provider_qp_get_write(qp->provider, provider.send_first_sequence, &write) == 0);
    assert(write.with_immediate && write.immediate == 0x1234abcd && write.length == 5);
    assert(mrc_provider_qp_complete_send(qp->provider, provider.send_first_sequence) == 0);
    uint64_t wr_id;
    assert(mrc_provider_qp_complete_recv(qp->provider, 5, 0x1234abcd, false, &wr_id) == 0);
    struct ibv_wc wc[2];
    assert(mrc_poll_cq(cq, 2, wc) == 2);
    assert(wc[0].wr_id == 81 && wc[0].status == IBV_WC_SUCCESS && wc[0].qp_num == qpn);
    assert(wc[1].wr_id == 71 && wc[1].wc_flags == IBV_WC_WITH_IMM && wc[1].imm_data == htonl(0x1234abcd));

    assert(mrc_post_send(qp, &send[2], &bad_send) == 0 && !bad_send);
    vattr.qp_state = IBV_QPS_RESET;
    assert(mrc_modify_qp(qp, &vattr, IBV_QP_STATE, NULL, 0) == 0);
    assert(mrc_poll_cq(cq, 2, wc) == 0);
    assert(mrc_provider_qp_query(qp->provider, &provider) == 0 && provider.send_pending == 0);
    assert(mrc_destroy_qp(qp) == 0);
    assert(mrc_destroy_cq(cq) == 0);
    assert(mrc_destroy_context(context) == -1 && errno == EBUSY);
    assert(mrc_dpdk_unbind_mr(context, first_mr) == 0);
    assert(mrc_dpdk_unbind_mr(context, last_mr) == 0);
    assert(mrc_dpdk_unbind_mr(context, foreign_mr) == 0);
    assert(mrc_destroy_context(context) == 0);
    assert(ibv_dereg_mr(foreign_mr) == 0 && ibv_dealloc_pd(foreign_pd) == 0);
    assert(ibv_dereg_mr(last_mr) == 0 && ibv_dereg_mr(first_mr) == 0 && ibv_dealloc_pd(pd) == 0);
}

struct async_call {
    struct mrc_context *context;
    struct mrc_async_event event;
    struct mrc_cq *destroy_cq;
    struct mrc_qp *destroy_qp;
    int result;
};

static void *run_async_call(void *argument)
{
    struct async_call *call = argument;
    if (call->destroy_cq)
        call->result = mrc_destroy_cq(call->destroy_cq);
    else if (call->destroy_qp)
        call->result = mrc_destroy_qp(call->destroy_qp);
    else
        call->result = mrc_get_async_event(call->context, &call->event);
    return NULL;
}

static int join_with_timeout(pthread_t thread, long milliseconds)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (milliseconds % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    return pthread_timedjoin_np(thread, NULL, &deadline);
}

static void expect_async_readiness(int fd, bool ready)
{
    struct pollfd poll_fd = { .fd = fd, .events = POLLIN };
    assert(poll(&poll_fd, 1, 0) == (ready ? 1 : 0));
    if (ready)
        assert(poll_fd.revents & POLLIN);
}

static void overflow_cq(struct mrc_cq *cq)
{
    const struct mrc_provider_wc completion = {
        .wr_id = 500, .status = MRC_PROVIDER_WC_SUCCESS, .opcode = MRC_PROVIDER_WC_WRITE,
    };
    assert(mrc_provider_cq_push(cq->provider, &completion, false) == 0);
    assert(mrc_provider_cq_push(cq->provider, &completion, false) == -EOVERFLOW);
    assert(mrc_provider_cq_push(cq->provider, &completion, false) == -EOVERFLOW);
}

static struct mrc_qp *create_async_qp(struct mrc_context *context, struct ibv_pd *pd,
                                      struct mrc_cq *send_cq)
{
    struct mrc_qp_init_attr init = {
        .pd = pd, .send_cq = send_cq, .cap = { .max_send_wr = 1 },
    };
    struct mrc_qp *qp = mrc_create_qp(context, &init);
    assert(qp);
    struct ibv_qp_attr verbs = { .qp_state = IBV_QPS_INIT };
    assert(mrc_modify_qp(qp, &verbs, IBV_QP_STATE, NULL, 0) == 0);
    return qp;
}

static void test_async_events(struct ibv_context *verbs)
{
    struct mrc_context *context = mrc_create_context(verbs, NULL);
    struct ibv_pd *pd = ibv_alloc_pd(verbs);
    assert(context && pd);

    int fd;
    struct mrc_async_event event;
    assert(mrc_dpdk_get_async_fd(NULL, &fd) == EINVAL);
    assert(mrc_dpdk_get_async_fd(context, NULL) == EINVAL);
    assert(mrc_dpdk_get_async_fd(context, &fd) == 0 && fd >= 0);
    assert(mrc_get_async_event(NULL, &event) == -1 && errno == EINVAL);
    assert(mrc_get_async_event(context, NULL) == -1 && errno == EINVAL);
    int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && !(flags & O_NONBLOCK));
    expect_async_readiness(fd, false);

    struct mrc_cq *send_cq = mrc_create_cq(context, 4, NULL, NULL, 0);
    struct mrc_cq *overflow = mrc_create_cq(context, 1, NULL, NULL, 0);
    assert(send_cq && overflow);
    struct mrc_qp *fatal = create_async_qp(context, pd, send_cq);
    struct mrc_qp *manual = create_async_qp(context, pd, send_cq);

    struct ibv_qp_attr set_error = { .qp_state = IBV_QPS_ERR };
    assert(mrc_modify_qp(manual, &set_error, IBV_QP_STATE, NULL, 0) == 0);
    expect_async_readiness(fd, false);

    pthread_t getter;
    struct async_call waiting = { .context = context, .result = -2 };
    assert(pthread_create(&getter, NULL, run_async_call, &waiting) == 0);
    assert(join_with_timeout(getter, 50) == ETIMEDOUT);
    overflow_cq(overflow);
    assert(mrc_provider_qp_fail(fatal->provider, 0, MRC_PROVIDER_WC_FLUSHED) == 0);
    assert(join_with_timeout(getter, 2000) == 0 && waiting.result == 0);
    assert(waiting.event.event_type == IBV_EVENT_CQ_ERR && waiting.event.element.cq == overflow);

    assert(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    expect_async_readiness(fd, true);
    assert(mrc_get_async_event(context, &event) == 0);
    assert(event.event_type == IBV_EVENT_QP_FATAL && event.element.qp == fatal);
    expect_async_readiness(fd, false);
    struct mrc_async_event none;
    assert(mrc_get_async_event(context, &none) == -1 && errno == EAGAIN);

    /* Dequeuing clears readiness. The fetched events still hold their objects
     * alive until their acknowledgements, even when destruction is in progress. */
    pthread_t cq_destroyer, qp_destroyer;
    struct async_call cq_destroy = { .destroy_cq = overflow, .result = -2 };
    struct async_call qp_destroy = { .destroy_qp = fatal, .result = -2 };
    assert(pthread_create(&cq_destroyer, NULL, run_async_call, &cq_destroy) == 0);
    assert(pthread_create(&qp_destroyer, NULL, run_async_call, &qp_destroy) == 0);
    assert(join_with_timeout(cq_destroyer, 50) == ETIMEDOUT);
    assert(join_with_timeout(qp_destroyer, 50) == ETIMEDOUT);
    mrc_ack_async_event(&waiting.event);
    assert(join_with_timeout(cq_destroyer, 2000) == 0 && cq_destroy.result == 0);
    assert(join_with_timeout(qp_destroyer, 50) == ETIMEDOUT);
    mrc_ack_async_event(&event);
    assert(join_with_timeout(qp_destroyer, 2000) == 0 && qp_destroy.result == 0);
    assert(mrc_destroy_qp(manual) == 0);

    /* Destruction can drop unread events from the head and middle of the
     * FIFO without consuming a notification belonging to another CQ. */
    struct mrc_cq *drop_cq = mrc_create_cq(context, 1, NULL, NULL, 0);
    struct mrc_cq *keep_cq = mrc_create_cq(context, 1, NULL, NULL, 0);
    assert(drop_cq && keep_cq);
    struct mrc_qp *drop_qp = create_async_qp(context, pd, send_cq);
    overflow_cq(drop_cq);
    assert(mrc_provider_qp_fail(drop_qp->provider, 0, MRC_PROVIDER_WC_FLUSHED) == 0);
    overflow_cq(keep_cq);

    cq_destroy = (struct async_call){ .destroy_cq = drop_cq, .result = -2 };
    qp_destroy = (struct async_call){ .destroy_qp = drop_qp, .result = -2 };
    assert(pthread_create(&cq_destroyer, NULL, run_async_call, &cq_destroy) == 0);
    assert(join_with_timeout(cq_destroyer, 2000) == 0 && cq_destroy.result == 0);
    assert(pthread_create(&qp_destroyer, NULL, run_async_call, &qp_destroy) == 0);
    assert(join_with_timeout(qp_destroyer, 2000) == 0 && qp_destroy.result == 0);
    expect_async_readiness(fd, true);
    assert(mrc_get_async_event(context, &event) == 0);
    assert(event.event_type == IBV_EVENT_CQ_ERR && event.element.cq == keep_cq);
    mrc_ack_async_event(&event);
    expect_async_readiness(fd, false);
    assert(mrc_get_async_event(context, &none) == -1 && errno == EAGAIN);

    assert(mrc_destroy_cq(keep_cq) == 0);
    assert(mrc_destroy_cq(send_cq) == 0);

    assert(fcntl(fd, F_SETFL, flags) == 0);
    waiting = (struct async_call){ .context = context, .result = -2 };
    assert(pthread_create(&getter, NULL, run_async_call, &waiting) == 0);
    assert(join_with_timeout(getter, 50) == ETIMEDOUT);
    assert(mrc_destroy_context(context) == -1 && errno == EBUSY);
    assert(pthread_cancel(getter) == 0);
    void *cancelled;
    assert(pthread_join(getter, &cancelled) == 0 && cancelled == PTHREAD_CANCELED);
    assert(mrc_destroy_context(context) == 0);
    assert(ibv_dealloc_pd(pd) == 0);
}

static void send_native_async_event(int fd, enum ibv_event_type type, uint64_t element)
{
    const struct ib_uverbs_async_event_desc notice = { .event_type = type, .element = element };
    assert(write(fd, &notice, sizeof(notice)) == sizeof(notice));
}

static void test_native_async_events(struct ibv_context *verbs)
{
    int pipe_fds[2];
    assert(pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) == 0);
    next_native_async_fd = pipe_fds[0];
    struct mrc_context *context = mrc_create_context(verbs, NULL);
    assert(context && next_native_async_fd == NO_ASYNC_OPEN_OVERRIDE);
    int fd;
    assert(mrc_dpdk_get_async_fd(context, &fd) == 0 && fd != pipe_fds[0]);
    int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && !(flags & O_NONBLOCK));
    expect_async_readiness(fd, false);

    pthread_t getter;
    struct async_call waiting = { .context = context, .result = -2 };
    assert(pthread_create(&getter, NULL, run_async_call, &waiting) == 0);
    assert(join_with_timeout(getter, 50) == ETIMEDOUT);
    send_native_async_event(pipe_fds[1], IBV_EVENT_PORT_ACTIVE, 1);
    assert(join_with_timeout(getter, 2000) == 0 && waiting.result == 0);
    assert(waiting.event.event_type == IBV_EVENT_PORT_ACTIVE && waiting.event.element.port_num == 1);
    mrc_ack_async_event(&waiting.event);
    expect_async_readiness(fd, false);

    assert(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    send_native_async_event(pipe_fds[1], IBV_EVENT_QP_FATAL, UINT64_MAX);
    send_native_async_event(pipe_fds[1], IBV_EVENT_PORT_ERR, 0);
    send_native_async_event(pipe_fds[1], IBV_EVENT_GID_CHANGE, UINT64_MAX);
    expect_async_readiness(fd, true);
    struct mrc_async_event event;
    assert(mrc_get_async_event(context, &event) == -1 && errno == EAGAIN);
    expect_async_readiness(fd, false);

    struct mrc_cq *cq = mrc_create_cq(context, 1, NULL, NULL, 0);
    assert(cq);
    send_native_async_event(pipe_fds[1], IBV_EVENT_PORT_ERR, 1);
    overflow_cq(cq);
    send_native_async_event(pipe_fds[1], IBV_EVENT_GID_CHANGE, 1);
    const enum ibv_event_type port_events[] = { IBV_EVENT_PORT_ERR, IBV_EVENT_GID_CHANGE };
    unsigned int port_event_count = 0;
    bool saw_cq_error = false;
    for (unsigned int i = 0; i < 3; i++) {
        expect_async_readiness(fd, true);
        assert(mrc_get_async_event(context, &event) == 0);
        if (event.event_type == IBV_EVENT_CQ_ERR) {
            assert(!saw_cq_error && event.element.cq == cq);
            saw_cq_error = true;
        } else {
            assert(port_event_count < sizeof(port_events) / sizeof(port_events[0]));
            assert(event.event_type == port_events[port_event_count] && event.element.port_num == 1);
            port_event_count++;
        }
        mrc_ack_async_event(&event);
    }
    assert(saw_cq_error && port_event_count == 2);
    assert(mrc_destroy_cq(cq) == 0);
    expect_async_readiness(fd, false);

    send_native_async_event(pipe_fds[1], IBV_EVENT_DEVICE_FATAL, 0);
    expect_async_readiness(fd, true);
    assert(mrc_get_async_event(context, &event) == 0 && event.event_type == IBV_EVENT_DEVICE_FATAL);
    mrc_ack_async_event(&event);
    assert(close(pipe_fds[1]) == 0);
    assert(mrc_get_async_event(context, &event) == -1 && errno == EAGAIN);
    expect_async_readiness(fd, false);

    cq = mrc_create_cq(context, 1, NULL, NULL, 0);
    assert(cq);
    overflow_cq(cq);
    expect_async_readiness(fd, true);
    assert(mrc_get_async_event(context, &event) == 0);
    assert(event.event_type == IBV_EVENT_CQ_ERR && event.element.cq == cq);
    mrc_ack_async_event(&event);
    expect_async_readiness(fd, false);
    assert(mrc_destroy_cq(cq) == 0);
    assert(mrc_destroy_context(context) == 0);
    assert(fcntl(pipe_fds[0], F_GETFD) == -1 && errno == EBADF);

    assert(pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) == 0);
    next_native_async_fd = pipe_fds[0];
    context = mrc_create_context(verbs, NULL);
    assert(context && next_native_async_fd == NO_ASYNC_OPEN_OVERRIDE);
    assert(mrc_dpdk_get_async_fd(context, &fd) == 0);
    flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    assert(close(pipe_fds[1]) == 0);
    expect_async_readiness(fd, true);
    assert(mrc_get_async_event(context, &event) == 0 && event.event_type == IBV_EVENT_DEVICE_FATAL);
    mrc_ack_async_event(&event);
    expect_async_readiness(fd, false);
    assert(mrc_get_async_event(context, &event) == -1 && errno == EAGAIN);
    assert(mrc_destroy_context(context) == 0);
}

static void test_native_async_descriptor_ownership(struct ibv_context *verbs)
{
    int verbs_status_flags = fcntl(verbs->async_fd, F_GETFL);
    int verbs_fd_flags = fcntl(verbs->async_fd, F_GETFD);
    assert(verbs_status_flags >= 0 && verbs_fd_flags >= 0);
    int probe = __real_mrc_verbs_async_open(verbs);
    struct mrc_context *first = mrc_create_context(verbs, NULL);
    struct mrc_context *second = mrc_create_context(verbs, NULL);
    assert(first && second);
    int fd;
    assert(mrc_dpdk_get_async_fd(first, &fd) == 0);
    int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    assert(fcntl(verbs->async_fd, F_GETFL) == verbs_status_flags);
    assert(fcntl(verbs->async_fd, F_GETFD) == verbs_fd_flags);

    int first_native = first->async.native_fd;
    int second_native = second->async.native_fd;
    if (probe >= 0) {
        assert(first_native >= 0 && second_native >= 0 && first_native != second_native);
        assert(first_native != verbs->async_fd && second_native != verbs->async_fd);
        assert(first_native != probe && second_native != probe);
        int status = fcntl(first_native, F_GETFL);
        int descriptor = fcntl(first_native, F_GETFD);
        assert(status >= 0 && (status & O_NONBLOCK));
        assert(descriptor >= 0 && (descriptor & FD_CLOEXEC));
        assert(fcntl(first_native, F_SETFL, status & ~O_NONBLOCK) == 0);
        int second_status = fcntl(second_native, F_GETFL);
        assert(second_status >= 0 && (second_status & O_NONBLOCK));
        assert(fcntl(first_native, F_SETFL, status) == 0);
        assert(close(probe) == 0);
    }
    assert(mrc_destroy_context(first) == 0);
    if (probe >= 0) {
        assert(fcntl(first_native, F_GETFD) == -1 && errno == EBADF);
        assert(fcntl(second_native, F_GETFD) >= 0);
    }
    assert(mrc_destroy_context(second) == 0);
    assert(fcntl(verbs->async_fd, F_GETFL) == verbs_status_flags);
    assert(fcntl(verbs->async_fd, F_GETFD) == verbs_fd_flags);
}

static void test_shared_cq_error(struct ibv_context *verbs)
{
    struct mrc_context *context = mrc_create_context(verbs, NULL);
    struct ibv_pd *pd = ibv_alloc_pd(verbs);
    assert(context && pd);
    struct mrc_cq *send_cq = mrc_create_cq(context, 1, NULL, NULL, 0);
    struct mrc_cq *recv_cq = mrc_create_cq(context, 1, NULL, NULL, 0);
    assert(send_cq && recv_cq);
    struct mrc_qp_init_attr init = {
        .pd = pd, .send_cq = send_cq, .recv_cq = recv_cq, .sq_sig_all = 1,
        .cap = { .max_send_wr = 2, .max_recv_wr = 1 },
    };
    struct mrc_qp *trigger = mrc_create_qp(context, &init);
    struct mrc_qp *sibling = mrc_create_qp(context, &init);
    assert(trigger && sibling);
    transition_to_rts(trigger->provider);
    transition_to_rts(sibling->provider);
    struct ibv_recv_wr receive = { .wr_id = 601 };
    struct ibv_recv_wr *bad_receive = NULL;
    assert(mrc_post_recv(sibling, &receive, &bad_receive) == 0 && !bad_receive);

    int fd;
    assert(mrc_dpdk_get_async_fd(context, &fd) == 0);
    int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    uint64_t sequence;
    assert(mrc_provider_qp_post_send(trigger->provider, 501, 0, false, &sequence) == 0);
    assert(mrc_provider_qp_post_send(trigger->provider, 502, 0, false, &sequence) == 0);
    assert(mrc_provider_qp_complete_send(trigger->provider, sequence) == -EOVERFLOW);

    struct mrc_async_event event;
    assert(mrc_get_async_event(context, &event) == 0);
    assert(event.event_type == IBV_EVENT_CQ_ERR && event.element.cq == send_cq);
    mrc_ack_async_event(&event);
    assert(mrc_get_async_event(context, &event) == 0);
    assert(event.event_type == IBV_EVENT_QP_FATAL && event.element.qp == trigger);
    mrc_ack_async_event(&event);

    struct ibv_qp_attr observed;
    assert(mrc_query_qp(sibling, &observed, IBV_QP_STATE, NULL, 0, NULL) == 0);
    assert(observed.qp_state == IBV_QPS_ERR);
    assert(mrc_get_async_event(context, &event) == 0);
    assert(event.event_type == IBV_EVENT_QP_FATAL && event.element.qp == sibling);
    mrc_ack_async_event(&event);
    struct ibv_wc completion;
    uint32_t sibling_qpn;
    assert(mrc_get_qpn(sibling, &sibling_qpn) == 0);
    assert(mrc_poll_cq(recv_cq, 1, &completion) == 1);
    assert(completion.wr_id == 601 && completion.qp_num == sibling_qpn &&
           completion.status == IBV_WC_WR_FLUSH_ERR);
    assert(mrc_get_async_event(context, &event) == -1 && errno == EAGAIN);

    assert(mrc_destroy_qp(trigger) == 0);
    assert(mrc_destroy_qp(sibling) == 0);
    assert(mrc_destroy_cq(recv_cq) == 0);
    assert(mrc_destroy_cq(send_cq) == 0);
    assert(mrc_destroy_context(context) == 0);
    assert(ibv_dealloc_pd(pd) == 0);
}

static void send_ev_event(int sender, int directory, struct mrc_comp_channel *channel,
                           struct mrc_cq *cq, void *context, unsigned int token,
                           enum mrc_ev_event_state state, bool dropped)
{
    struct mrc_ev_event_record record = {
        .profile_id = 71, .value = {0x20, 0x01, 0x0d, 0xb8}, .port = 2,
        .state = state, .dropped = dropped,
    };
    record.value[31] = (uint8_t)token;
    int fd;
    assert(mrc_get_comp_channel_fd(channel, &fd) == 0);
    struct pollfd readiness = { .fd = fd, .events = POLLIN };
    assert(poll(&readiness, 1, 0) == 0);
    assert(mrc_req_notify_cq(cq, 1) == 0);
    assert(mrc_ev_event_send(sender, directory, &record) == 0);
    assert(poll(&readiness, 1, 2000) == 1 && (readiness.revents & POLLIN));
    struct mrc_cq *ready = NULL;
    void *received_context = NULL;
    assert(mrc_get_cq_event(channel, &ready, &received_context) == 0);
    assert(ready == cq && received_context == context);
    mrc_ack_cq_events(ready, 1);
}

static void test_ev_event_cq(struct ibv_context *verbs)
{
    struct mrc_context *context = mrc_create_context(verbs, NULL);
    assert(context);
    if (!mrc_abi_controller_capable()) {
        assert(mrc_ctl_create_ev_event_cq(context, 2, NULL, NULL, 0) == NULL && errno == EPERM);
        assert(mrc_destroy_context(context) == 0);
        return;
    }

    char directory[] = "/tmp/ocs-mrc-ev-cq.XXXXXX";
    assert(mkdtemp(directory));
    assert(mrc_dpdk_set_profile_store(context, directory) == 0);
    struct mrc_ctl_device_attr device;
    int supported;
    assert(mrc_ctl_query_device_ex(NULL, context, &device, &supported) == 0 && supported);
    assert(device.opt_attr & MRC_CTL_OPT_CAP_EV_EVENT);
    assert(!(device.opt_attr & MRC_CTL_OPT_CAP_EV_EVENT_PRECISE_DROP_CNT));
    struct mrc_comp_channel *channel;
    assert(mrc_create_comp_channel(context, &channel) == 0);
    int consumer = 51;
    struct mrc_cq *cq = mrc_ctl_create_ev_event_cq(context, 2, &consumer, channel, 0);
    assert(cq);
    struct mrc_context *other = mrc_create_context(verbs, NULL);
    assert(other && mrc_dpdk_set_profile_store(other, directory) == 0);
    assert(mrc_ctl_create_ev_event_cq(other, 2, NULL, NULL, 0) == NULL && errno == EBUSY);
    assert(mrc_poll_cq(cq, 0, NULL) == -1 && errno == EINVAL);
    assert(mrc_ctl_poll_ev_event(cq, 1, NULL) == -EINVAL);
    assert(mrc_ctl_poll_ev_event(cq, 0, NULL) == 0);
    struct mrc_cq *normal = mrc_create_cq(context, 1, NULL, NULL, 0);
    assert(normal && mrc_ctl_poll_ev_event(normal, 0, NULL) == -EINVAL);
    assert(mrc_destroy_cq(normal) == 0);
    struct ibv_pd *pd = ibv_alloc_pd(verbs);
    assert(pd);
    struct mrc_qp_init_attr init = {
        .pd = pd, .send_cq = cq, .cap = { .max_send_wr = 1 },
    };
    assert(mrc_create_qp(context, &init) == NULL && errno == EINVAL);
    assert(ibv_dealloc_pd(pd) == 0);

    int sender = mrc_ev_event_open_sender();
    int store_fd = mrc_abi_profile_store(context).fd;
    assert(sender >= 0 && store_fd >= 0);
    send_ev_event(sender, store_fd, channel, cq, &consumer, 1, MRC_EV_EVENT_ASSUMED_BAD, false);
    send_ev_event(sender, store_fd, channel, cq, &consumer, 2, MRC_EV_EVENT_GOOD, false);
    send_ev_event(sender, store_fd, channel, cq, &consumer, 3, MRC_EV_EVENT_ASSUMED_BAD, false);

    struct mrc_ctl_ev_event events[2];
    assert(mrc_ctl_poll_ev_event(cq, 2, events) == 2);
    assert(events[0].profile_id == 71 && events[0].ev.port == 2 &&
           events[0].ev.val[31] == 2 && events[0].state == MRC_CTL_EV_GOOD &&
           events[0].drop_count == 1);
    assert(events[1].profile_id == 71 && events[1].ev.port == 2 &&
           events[1].ev.val[31] == 3 && events[1].state == MRC_CTL_EV_ASSUMED_BAD &&
           events[1].drop_count == 0);
    assert(mrc_ctl_poll_ev_event(cq, 2, events) == 0);
    int async_fd;
    assert(mrc_dpdk_get_async_fd(context, &async_fd) == 0);
    expect_async_readiness(async_fd, false);

    send_ev_event(sender, store_fd, channel, cq, &consumer, 4, MRC_EV_EVENT_GOOD, true);
    assert(mrc_ctl_poll_ev_event(cq, 1, events) == 1 && events[0].ev.val[31] == 4 &&
           events[0].drop_count == 1);
    send_ev_event(sender, store_fd, channel, cq, &consumer, 5, MRC_EV_EVENT_ASSUMED_BAD, false);
    assert(mrc_ctl_poll_ev_event(cq, 1, events) == 1 && events[0].ev.val[31] == 5 &&
           events[0].drop_count == 0);
    assert(mrc_destroy_cq(cq) == 0);
    assert(mrc_destroy_comp_channel(channel) == 0);

    /* Legacy callers use a shorter array stride. Ensure a second event data
     * record is contiguous and neither returned event writes past that array. */
    assert(mrc_ctl_query_device(verbs, &device) == 0);
    assert(mrc_create_comp_channel(other, &channel) == 0);
    cq = mrc_ctl_create_ev_event_cq(other, 2, &consumer, channel, 0);
    assert(cq);
    send_ev_event(sender, store_fd, channel, cq, &consumer, 6, MRC_EV_EVENT_GOOD, false);
    send_ev_event(sender, store_fd, channel, cq, &consumer, 7, MRC_EV_EVENT_ASSUMED_BAD, true);
    struct legacy_event {
        uint64_t profile_id;
        struct mrc_ctl_ev ev;
        uint32_t drop_count;
    } legacy[2];
    _Alignas(struct mrc_ctl_ev_event) uint8_t output[sizeof(events)];
    memset(output, 0x5a, sizeof(output));
    assert(mrc_ctl_poll_ev_event(cq, 2, (struct mrc_ctl_ev_event *)output) == 2);
    memcpy(legacy, output, sizeof(legacy));
    for (size_t i = 0; i < 2; i++) {
        assert(legacy[i].profile_id == 71 && legacy[i].ev.port == 2);
        assert(legacy[i].ev.val[31] == 6 + i && legacy[i].drop_count == i);
    }
    for (size_t i = sizeof(legacy); i < sizeof(output); i++)
        assert(output[i] == 0x5a);
    assert(mrc_destroy_cq(cq) == 0);
    assert(mrc_destroy_comp_channel(channel) == 0);
    assert(mrc_destroy_context(other) == 0);

    /* A context with an extended query keeps its layout after a legacy
     * query elsewhere in the process. Polling also works without a channel. */
    cq = mrc_ctl_create_ev_event_cq(context, 1, NULL, NULL, 0);
    assert(cq);
    const struct mrc_ev_event_record record = {
        .profile_id = 72, .port = 1, .state = MRC_EV_EVENT_ASSUMED_BAD,
    };
    assert(mrc_ev_event_send(sender, store_fd, &record) == 0);
    int count = mrc_ctl_poll_ev_event(cq, 2, events);
    const struct timespec delay = { .tv_nsec = 1000000 };
    for (unsigned int retry = 0; !count && retry < 2000; retry++) {
        nanosleep(&delay, NULL);
        count = mrc_ctl_poll_ev_event(cq, 2, events);
    }
    assert(count == 1 && events[0].profile_id == 72 && events[0].ev.port == 1 &&
           events[0].state == MRC_CTL_EV_ASSUMED_BAD && events[0].drop_count == 0);
    assert(mrc_destroy_cq(cq) == 0);
    assert(close(sender) == 0);
    assert(unlinkat(store_fd, "ev-events.lock", 0) == 0);
    assert(mrc_destroy_context(context) == 0);
    assert(rmdir(directory) == 0);
    puts("OCP MRC controller EV CQ notification, overflow, and legacy polling tests passed");
}

int main(void)
{
    struct ibv_device **devices = ibv_get_device_list(NULL);
    if (!devices) {
        perror("ibv_get_device_list");
        return EXIT_FAILURE;
    }
    struct ibv_context *verbs = NULL;
    for (struct ibv_device **device = devices; *device && !verbs; device++)
        verbs = ibv_open_device(*device);
    ibv_free_device_list(devices);
    if (!verbs) {
        fputs("The MRC ABI test needs an accessible libibverbs device.\n", stderr);
        return EXIT_FAILURE;
    }
    test_native_async_descriptor_ownership(verbs);
    test_resources_and_completions(verbs);
    test_verbs_memory_binding(verbs);
    test_qp_hints(verbs);
    test_public_qp(verbs);
    test_async_events(verbs);
    /* The provider queue also works on kernels without a native channel. */
    next_native_async_fd = -EOPNOTSUPP;
    test_async_events(verbs);
    assert(next_native_async_fd == NO_ASYNC_OPEN_OVERRIDE);
    test_native_async_events(verbs);
    test_shared_cq_error(verbs);
    test_ev_event_cq(verbs);
    assert(ibv_close_device(verbs) == 0);
    puts("OCP MRC context, QP, completion, and async ABI tests passed");
    return EXIT_SUCCESS;
}
