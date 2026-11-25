#include "mrc_abi.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct mrc_qp_hint {
    struct mrc_context *context;
    struct mrc_qp_hint *next;
    struct mrc_qp_hint_attr attrs;
    size_t users;
};

static const unsigned int hint_fields = MRC_QP_HINT_NUM_QPS_PER_PEER |
    MRC_QP_HINT_NUM_SEND_PEERS | MRC_QP_HINT_NUM_REMOTE_RECV_PEERS | MRC_QP_HINT_VENDOR_CFG;

static int hint_error(int error)
{
    errno = error;
    return -1;
}

static bool valid_values(const struct mrc_qp_hint_attr *attrs, unsigned int mask)
{
    return (!(mask & MRC_QP_HINT_NUM_QPS_PER_PEER) || attrs->num_qps_per_peer >= 0) &&
           (!(mask & MRC_QP_HINT_NUM_SEND_PEERS) || attrs->num_send_peers >= 0) &&
           (!(mask & MRC_QP_HINT_NUM_REMOTE_RECV_PEERS) || attrs->num_remote_recv_peers >= 0);
}

static void copy_fields(struct mrc_qp_hint_attr *dst, const struct mrc_qp_hint_attr *src,
                        unsigned int mask)
{
    if (mask & MRC_QP_HINT_NUM_QPS_PER_PEER)
        dst->num_qps_per_peer = src->num_qps_per_peer;
    if (mask & MRC_QP_HINT_NUM_SEND_PEERS)
        dst->num_send_peers = src->num_send_peers;
    if (mask & MRC_QP_HINT_NUM_REMOTE_RECV_PEERS)
        dst->num_remote_recv_peers = src->num_remote_recv_peers;
    if (mask & MRC_QP_HINT_VENDOR_CFG)
        memcpy(dst->vendor_cfg, src->vendor_cfg, sizeof(dst->vendor_cfg));
}

static struct mrc_qp_hint **find_hint(struct mrc_context *context, const struct mrc_qp_hint *hint)
{
    struct mrc_qp_hint **slot = &context->hints;
    while (*slot && *slot != hint)
        slot = &(*slot)->next;
    return slot;
}

MRC_PUBLIC struct mrc_qp_hint *mrc_create_qp_hint(struct mrc_context *context,
                                                 struct mrc_qp_hint_init_attr *init)
{
    if (!context || !init || !valid_values(&init->attr, hint_fields)) {
        errno = EINVAL;
        return NULL;
    }

    struct mrc_qp_hint *hint = calloc(1, sizeof(*hint));
    if (!hint)
        return NULL;
    hint->context = context;
    hint->attrs = init->attr;

    pthread_mutex_lock(&context->mutex);
    int error = 0;
    if (context->hint_count == MRC_ABI_MAX_QP_HINTS) {
        error = ENOSPC;
    } else if (context->children == SIZE_MAX) {
        error = EOVERFLOW;
    } else {
        hint->next = context->hints;
        context->hints = hint;
        context->hint_count++;
        context->children++;
    }
    pthread_mutex_unlock(&context->mutex);
    if (!error)
        return hint;
    free(hint);
    errno = error;
    return NULL;
}

MRC_PUBLIC int mrc_destroy_qp_hint(struct mrc_qp_hint *hint)
{
    if (!hint)
        return hint_error(EINVAL);
    struct mrc_context *context = hint->context;
    pthread_mutex_lock(&context->mutex);
    struct mrc_qp_hint **slot = find_hint(context, hint);
    int error = 0;
    if (!*slot) {
        error = EINVAL;
    } else if (hint->users) {
        error = EBUSY;
    } else {
        *slot = hint->next;
        context->hint_count--;
        context->children--;
    }
    pthread_mutex_unlock(&context->mutex);
    if (error)
        return hint_error(error);
    free(hint);
    return 0;
}

MRC_PUBLIC int mrc_query_qp_hint(struct mrc_qp_hint *hint, struct mrc_qp_hint_attr *attrs, int mask)
{
    if (!hint || !attrs || ((unsigned int)mask & ~hint_fields))
        return hint_error(EINVAL);
    pthread_mutex_lock(&hint->context->mutex);
    copy_fields(attrs, &hint->attrs, (unsigned int)mask);
    pthread_mutex_unlock(&hint->context->mutex);
    return 0;
}

MRC_PUBLIC int mrc_modify_qp_hint(struct mrc_qp_hint *hint, struct mrc_qp_hint_attr *attrs, int mask)
{
    if (!hint || !attrs || ((unsigned int)mask & ~hint_fields) || !valid_values(attrs, (unsigned int)mask))
        return hint_error(EINVAL);
    pthread_mutex_lock(&hint->context->mutex);
    copy_fields(&hint->attrs, attrs, (unsigned int)mask);
    pthread_mutex_unlock(&hint->context->mutex);
    return 0;
}

int mrc_abi_hint_retain(struct mrc_context *context, struct mrc_qp_hint *hint)
{
    if (!hint)
        return 0;
    pthread_mutex_lock(&context->mutex);
    struct mrc_qp_hint *member = *find_hint(context, hint);
    int error = 0;
    if (!member)
        error = EINVAL;
    else if (member->users == SIZE_MAX)
        error = EOVERFLOW;
    else
        member->users++;
    pthread_mutex_unlock(&context->mutex);
    return error;
}

void mrc_abi_hint_release(struct mrc_qp_hint *hint)
{
    if (!hint)
        return;
    struct mrc_context *context = hint->context;
    pthread_mutex_lock(&context->mutex);
    hint->users--;
    pthread_mutex_unlock(&context->mutex);
}
