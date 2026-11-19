#include "mrc_abi.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>

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
    }
    struct mrc_device_attr same;
    int same_support;
    assert(mrc_query_device_ex(NULL, context, &same, &same_support) == 0);
    assert(same_support == supported && same.mrc_protocol_version == device.mrc_protocol_version);
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
        .cap = { .max_send_wr = 2, .max_recv_wr = 1, .max_send_sge = 2 },
    };
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
    vattr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_INIT, .qp_access_flags = IBV_ACCESS_REMOTE_WRITE };
    assert(mrc_modify_qp(qp, &vattr, IBV_QP_STATE | IBV_QP_ACCESS_FLAGS, NULL, 0) == 0);

    struct ibv_recv_wr recv[] = {{.wr_id = 71}, {.wr_id = 72}};
    struct ibv_recv_wr *bad_recv;
    recv[0].next = &recv[1];
    assert(mrc_post_recv(qp, recv, &bad_recv) == -1 && errno == ENOSPC && bad_recv == &recv[1]);

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
    test_resources_and_completions(verbs);
    test_verbs_memory_binding(verbs);
    test_public_qp(verbs);
    assert(ibv_close_device(verbs) == 0);
    puts("OCP MRC context, QP, and completion ABI tests passed");
    return EXIT_SUCCESS;
}
