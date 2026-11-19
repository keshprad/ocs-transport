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
    version.mrc_api_version_used = MRC_API_VER(1, 0, 1);
    struct mrc_context *context = mrc_create_context(verbs, &version);
    struct mrc_context *other = mrc_create_context(verbs, NULL);
    assert(context && other);
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
    assert(ibv_close_device(verbs) == 0);
    puts("OCP MRC context and completion ABI tests passed");
    return EXIT_SUCCESS;
}
