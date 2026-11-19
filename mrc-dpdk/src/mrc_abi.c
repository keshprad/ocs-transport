#include "mrc_abi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>

#define MRC_PUBLIC __attribute__((visibility("default")))

static int set_errno(int error)
{
    errno = error;
    return -1;
}

static int retain_context(struct mrc_context *context)
{
    pthread_mutex_lock(&context->mutex);
    int error = 0;
    if (context->children == SIZE_MAX)
        error = EOVERFLOW;
    else
        context->children++;
    pthread_mutex_unlock(&context->mutex);
    return error;
}

static void release_context(struct mrc_context *context)
{
    pthread_mutex_lock(&context->mutex);
    context->children--;
    pthread_mutex_unlock(&context->mutex);
}

MRC_PUBLIC struct mrc_context *mrc_create_context(struct ibv_context *vcontext,
                                                  struct mrc_context_attr *attr)
{
    if (!vcontext) {
        errno = EINVAL;
        return NULL;
    }
    if (attr) {
        uint32_t api = attr->mrc_api_version_used;
        uint32_t vendor = attr->mrc_vendor_api_version_used;
        if ((api && api != UINT32_MAX &&
             (api < MRC_API_VER(1, 0, 0) || api > MRC_API_VER(1, 0, 1))) || vendor) {
            errno = EPROTONOSUPPORT;
            return NULL;
        }
    }
    struct ibv_device_attr device;
    int error = ibv_query_device(vcontext, &device);
    if (error) {
        if (error > 0)
            errno = error;
        return NULL;
    }
    struct mrc_context *context = calloc(1, sizeof(*context));
    if (!context)
        return NULL;
    error = pthread_mutex_init(&context->mutex, NULL);
    if (error) {
        free(context);
        errno = error;
        return NULL;
    }
    context->verbs = vcontext;
    return context;
}

MRC_PUBLIC int mrc_destroy_context(struct mrc_context *context)
{
    if (!context)
        return set_errno(EINVAL);
    pthread_mutex_lock(&context->mutex);
    bool busy = context->children != 0;
    pthread_mutex_unlock(&context->mutex);
    if (busy)
        return set_errno(EBUSY);
    pthread_mutex_destroy(&context->mutex);
    free(context);
    return 0;
}

MRC_PUBLIC int mrc_create_comp_channel(struct mrc_context *context,
                                       struct mrc_comp_channel **channel)
{
    if (!context || !channel)
        return set_errno(EINVAL);
    struct mrc_comp_channel *object = calloc(1, sizeof(*object));
    if (!object)
        return -1;
    int error = retain_context(context);
    if (error)
        goto free_object;
    error = -mrc_provider_comp_channel_create(&object->provider);
    if (error)
        goto release_parent;
    int fd = mrc_provider_comp_channel_fd(object->provider);
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        error = errno;
        mrc_provider_comp_channel_destroy(object->provider);
        goto release_parent;
    }
    object->context = context;
    *channel = object;
    return 0;

release_parent:
    release_context(context);
free_object:
    free(object);
    return set_errno(error);
}

MRC_PUBLIC int mrc_get_comp_channel_fd(struct mrc_comp_channel *channel, int *fd)
{
    if (!channel || !fd)
        return set_errno(EINVAL);
    *fd = mrc_provider_comp_channel_fd(channel->provider);
    return 0;
}

MRC_PUBLIC int mrc_destroy_comp_channel(struct mrc_comp_channel *channel)
{
    if (!channel)
        return EINVAL;
    int error = -mrc_provider_comp_channel_destroy(channel->provider);
    if (error)
        return error;
    release_context(channel->context);
    free(channel);
    return 0;
}

MRC_PUBLIC struct mrc_cq *mrc_create_cq(struct mrc_context *context, int cqe,
                                       void *cq_context, struct mrc_comp_channel *channel,
                                       int comp_vector)
{
    if (!context || cqe <= 0 || comp_vector < 0 || comp_vector >= context->verbs->num_comp_vectors ||
        (channel && channel->context != context)) {
        errno = EINVAL;
        return NULL;
    }
    struct mrc_cq *cq = calloc(1, sizeof(*cq));
    if (!cq)
        return NULL;
    int error = retain_context(context);
    if (!error) {
        error = -mrc_provider_cq_create_on_channel((size_t)cqe,
                                                   channel ? channel->provider : NULL,
                                                   cq, &cq->provider);
        if (error)
            release_context(context);
    }
    if (error) {
        free(cq);
        errno = error;
        return NULL;
    }
    cq->context = context;
    cq->application_context = cq_context;
    cq->capacity = cqe;
    return cq;
}

static struct ibv_wc verbs_completion(const struct mrc_provider_wc *entry)
{
    static const enum ibv_wc_status statuses[] = {
        [MRC_PROVIDER_WC_SUCCESS] = IBV_WC_SUCCESS,
        [MRC_PROVIDER_WC_LOCAL_LENGTH] = IBV_WC_LOC_LEN_ERR,
        [MRC_PROVIDER_WC_LOCAL_PROTECTION] = IBV_WC_LOC_PROT_ERR,
        [MRC_PROVIDER_WC_LOCAL_OPERATION] = IBV_WC_LOC_QP_OP_ERR,
        [MRC_PROVIDER_WC_BAD_RESPONSE] = IBV_WC_BAD_RESP_ERR,
        [MRC_PROVIDER_WC_REMOTE_INVALID_REQUEST] = IBV_WC_REM_INV_REQ_ERR,
        [MRC_PROVIDER_WC_REMOTE_ACCESS] = IBV_WC_REM_ACCESS_ERR,
        [MRC_PROVIDER_WC_REMOTE_OPERATION] = IBV_WC_REM_OP_ERR,
        [MRC_PROVIDER_WC_RETRY_EXCEEDED] = IBV_WC_RETRY_EXC_ERR,
        [MRC_PROVIDER_WC_FLUSHED] = IBV_WC_WR_FLUSH_ERR,
    };
    unsigned int status = entry->status;
    struct ibv_wc wc = {
        .wr_id = entry->wr_id, .qp_num = entry->qpn, .byte_len = entry->byte_length,
        .status = status < sizeof(statuses) / sizeof(statuses[0]) ? statuses[status]
                                                                 : IBV_WC_GENERAL_ERR,
        .opcode = entry->opcode == MRC_PROVIDER_WC_WRITE ? IBV_WC_RDMA_WRITE
                                                       : IBV_WC_RECV_RDMA_WITH_IMM,
    };
    if (entry->with_immediate) {
        wc.wc_flags = IBV_WC_WITH_IMM;
        wc.imm_data = htonl(entry->immediate);
    }
    return wc;
}

MRC_PUBLIC int mrc_poll_cq(struct mrc_cq *cq, int num_entries, struct ibv_wc *wc)
{
    if (!cq || num_entries < 0 || (num_entries && !wc))
        return set_errno(EINVAL);
    size_t count = num_entries < cq->capacity ? (size_t)num_entries : (size_t)cq->capacity;
    struct mrc_provider_wc stack[32];
    struct mrc_provider_wc *entries = stack;
    if (count > sizeof(stack) / sizeof(stack[0])) {
        if (count > SIZE_MAX / sizeof(*entries))
            return set_errno(EOVERFLOW);
        entries = malloc(count * sizeof(*entries));
        if (!entries)
            return -1;
    }
    int result = mrc_provider_cq_poll(cq->provider, entries, count);
    for (int i = 0; i < result; i++)
        wc[i] = verbs_completion(&entries[i]);
    if (entries != stack)
        free(entries);
    return result < 0 ? set_errno(-result) : result;
}

MRC_PUBLIC int mrc_destroy_cq(struct mrc_cq *cq)
{
    if (!cq)
        return EINVAL;
    int error = -mrc_provider_cq_destroy_wait(cq->provider);
    if (error)
        return error;
    release_context(cq->context);
    free(cq);
    return 0;
}

MRC_PUBLIC int mrc_req_notify_cq(struct mrc_cq *cq, int solicited_only)
{
    if (!cq)
        return EINVAL;
    return -mrc_provider_cq_arm(cq->provider, solicited_only != 0);
}

MRC_PUBLIC int mrc_get_cq_event(struct mrc_comp_channel *channel, struct mrc_cq **cq,
                                 void **cq_context)
{
    if (!channel || !cq || !cq_context)
        return set_errno(EINVAL);
    int fd = mrc_provider_comp_channel_fd(channel->provider);
    for (;;) {
        struct mrc_provider_cq *backend;
        void *context;
        int result = mrc_provider_comp_channel_next(channel->provider, &backend, &context);
        if (!result) {
            struct mrc_cq *object = context;
            *cq = object;
            *cq_context = object->application_context;
            return 0;
        }
        if (result != -EAGAIN)
            return set_errno(-result);
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0)
            return -1;
        if (flags & O_NONBLOCK)
            return set_errno(EAGAIN);
        struct pollfd wait = { .fd = fd, .events = POLLIN };
        if (poll(&wait, 1, -1) < 0)
            return -1;
        if (!(wait.revents & POLLIN))
            return set_errno(EIO);
    }
}

MRC_PUBLIC void mrc_ack_cq_events(struct mrc_cq *cq, unsigned int events)
{
    if (cq)
        mrc_provider_cq_ack_events(cq->provider, events);
}
