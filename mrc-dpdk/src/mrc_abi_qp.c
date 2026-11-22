#include "mrc_abi.h"
#include "mrc_qp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>

enum {
    MAX_SGE = 16,
    MAX_QUEUE_DEPTH = 4096,
    MAX_REGISTERED_MRS = 4096,
    MAX_WRITE_BYTES = 1U << 30,
    MAX_INLINE_BYTES = 4096,
};

static atomic_uint next_qpn = 3;

static int allocate_qpn(uint32_t *qpn)
{
    unsigned int number = atomic_load_explicit(&next_qpn, memory_order_relaxed);
    do {
        if (number > MRC_PSN_MASK)
            return ENOSPC;
    } while (!atomic_compare_exchange_weak_explicit(&next_qpn, &number, number + 1,
                                                    memory_order_relaxed, memory_order_relaxed));
    *qpn = number;
    return 0;
}

/* The context mutex guards the registry and its QP and MR references. */
static struct mrc_abi_pd **find_pd(struct mrc_context *context, const struct ibv_pd *verbs)
{
    struct mrc_abi_pd **slot = &context->pds;
    while (*slot && (*slot)->verbs != verbs)
        slot = &(*slot)->next;
    return slot;
}

static int get_pd(struct mrc_abi_pd **slot, const struct ibv_pd *verbs)
{
    if (*slot)
        return (*slot)->users == SIZE_MAX ? EOVERFLOW : 0;
    struct mrc_abi_pd *pd = calloc(1, sizeof(*pd));
    if (!pd)
        return ENOMEM;
    int error = -mrc_provider_pd_create(MAX_REGISTERED_MRS, &pd->provider);
    if (error) {
        free(pd);
        return error;
    }
    pd->verbs = verbs;
    *slot = pd;
    return 0;
}

static void remove_unused_pd(struct mrc_abi_pd **slot)
{
    struct mrc_abi_pd *pd = *slot;
    if (pd->users == 0 && mrc_provider_pd_destroy(pd->provider) == 0) {
        *slot = pd->next;
        free(pd);
    }
}

static bool valid_mr(const struct mrc_context *context, const struct ibv_mr *mr)
{
    return context && mr && mr->context == context->verbs && mr->pd &&
           mr->pd->context == context->verbs;
}

MRC_PUBLIC int mrc_dpdk_bind_mr(struct mrc_context *context, const struct ibv_mr *mr,
                                uint64_t iova, unsigned int access)
{
    if (!valid_mr(context, mr))
        return EINVAL;
    pthread_mutex_lock(&context->mutex);
    struct mrc_abi_pd **slot = find_pd(context, mr->pd);
    int error = get_pd(slot, mr->pd);
    if (!error) {
        error = -mrc_abi_bind_mr(context, (*slot)->provider, mr->pd, mr, iova, access);
        if (!error)
            (*slot)->users++;
        else
            remove_unused_pd(slot);
    }
    pthread_mutex_unlock(&context->mutex);
    return error;
}

MRC_PUBLIC int mrc_dpdk_unbind_mr(struct mrc_context *context, const struct ibv_mr *mr)
{
    if (!valid_mr(context, mr))
        return EINVAL;
    pthread_mutex_lock(&context->mutex);
    struct mrc_abi_pd **slot = find_pd(context, mr->pd);
    int error = *slot ? -mrc_provider_mr_deregister((*slot)->provider, mr->lkey) : ENOENT;
    if (!error) {
        (*slot)->users--;
        remove_unused_pd(slot);
    }
    pthread_mutex_unlock(&context->mutex);
    return error;
}

static int validate_init(struct mrc_context *context, const struct mrc_qp_init_attr *attr)
{
    if (!context || !attr || !attr->pd || attr->pd->context != context->verbs ||
        (!attr->send_cq && !attr->recv_cq) ||
        (attr->cap.max_send_wr && !attr->send_cq) || (attr->cap.max_recv_wr && !attr->recv_cq) ||
        (attr->send_cq && (attr->send_cq->context != context || attr->send_cq->ev_events)) ||
        (attr->recv_cq && (attr->recv_cq->context != context || attr->recv_cq->ev_events)))
        return EINVAL;
    if (attr->protocol_version != MRC_PROTOCOL_VERSION_0 &&
        attr->protocol_version != MRC_PROTOCOL_VERSION_1)
        return EPROTONOSUPPORT;
    const uint64_t supported_send_ops = IBV_QP_EX_WITH_RDMA_WRITE | IBV_QP_EX_WITH_RDMA_WRITE_WITH_IMM;
    if (attr->cap.max_send_wr > MAX_QUEUE_DEPTH || attr->cap.max_recv_wr > MAX_QUEUE_DEPTH ||
        attr->cap.max_send_sge > MAX_SGE || attr->cap.max_recv_sge > MAX_SGE ||
        attr->cap.max_inline_data > MAX_INLINE_BYTES || (attr->send_ops_flags & ~supported_send_ops))
        return EOPNOTSUPP;
    return 0;
}

static struct mrc_qp_attr default_qp_attrs(void)
{
    return (struct mrc_qp_attr){
        .mpr = { .mpr = MRC_MPR_DEFAULT_UNITS, .mpr_dest = MRC_MPR_DEFAULT_UNITS },
        .timeout = 16, .retry = { .linear_cnt = 3, .exp_cnt = 4 },
    };
}

static void qp_error_event(void *argument, enum mrc_provider_qp_error error)
{
    struct mrc_qp *qp = argument;
    static const enum ibv_event_type types[] = {
        [MRC_PROVIDER_QP_FATAL_ERROR] = IBV_EVENT_QP_FATAL,
        [MRC_PROVIDER_QP_REQUEST_ERROR] = IBV_EVENT_QP_REQ_ERR,
        [MRC_PROVIDER_QP_ACCESS_ERROR] = IBV_EVENT_QP_ACCESS_ERR,
    };
    const struct mrc_async_event event = { .event_type = types[error], .element.qp = qp };
    mrc_abi_async_publish(&qp->async, &event);
}

MRC_PUBLIC struct mrc_qp *mrc_create_qp(struct mrc_context *context, struct mrc_qp_init_attr *init)
{
    int error = validate_init(context, init);
    if (error) {
        errno = error;
        return NULL;
    }
    struct mrc_qp *qp = calloc(1, sizeof(*qp));
    if (!qp)
        return NULL;
    error = pthread_mutex_init(&qp->mutex, NULL);
    if (error)
        goto free_qp;
    qp->async.context = context;
    error = mrc_abi_async_prepare(&qp->async);
    if (!error)
        error = allocate_qpn(&qp->qpn);
    if (error)
        goto destroy_mutex;

    qp->init = *init;
    if (init->send_cq && !qp->init.cap.max_send_wr)
        qp->init.cap.max_send_wr = 1;
    if (init->recv_cq && !qp->init.cap.max_recv_wr)
        qp->init.cap.max_recv_wr = 1;

    pthread_mutex_lock(&context->mutex);
    struct mrc_abi_pd **slot = find_pd(context, init->pd);
    error = get_pd(slot, init->pd);
    if (!error) {
        const struct mrc_provider_qp_config config = {
            .pd = (*slot)->provider, .qpn = qp->qpn,
            .send_capacity = qp->init.cap.max_send_wr, .recv_capacity = qp->init.cap.max_recv_wr,
            .send_cq = init->send_cq ? init->send_cq->provider : NULL,
            .recv_cq = init->recv_cq ? init->recv_cq->provider : NULL,
            .sq_sig_all = init->sq_sig_all != 0,
            .on_error = qp_error_event, .error_context = qp,
        };
        error = -mrc_provider_qp_create(&config, &qp->provider);
        if (!error) {
            qp->pd = *slot;
            qp->pd->users++;
        } else {
            remove_unused_pd(slot);
        }
    }
    pthread_mutex_unlock(&context->mutex);
    if (error)
        goto destroy_mutex;

    qp->context = context;
    qp->ev_lease.fd = -1;
    qp->cc_lease.fd = -1;
    qp->init.protocol_version = MRC_PROTOCOL_VERSION_1;
    qp->verbs = (struct ibv_qp_attr){ .cap = qp->init.cap, .path_mtu = IBV_MTU_1024 };
    qp->attrs = default_qp_attrs();
    *init = qp->init;
    return qp;

destroy_mutex:
    mrc_abi_async_finish(&qp->async);
    pthread_mutex_destroy(&qp->mutex);
free_qp:
    free(qp);
    errno = error;
    return NULL;
}

MRC_PUBLIC int mrc_destroy_qp(struct mrc_qp *qp)
{
    if (!qp)
        return EINVAL;
    struct mrc_context *context = qp->context;
    pthread_mutex_lock(&qp->mutex);
    /* Cancellation enters ERROR before releasing reliability and CC state. */
    int error = -mrc_provider_qp_cancel(qp->provider);
    if (!error) {
        mrc_abi_release_route(qp);
        error = -mrc_provider_qp_reset(qp->provider);
    }
    if (!error)
        error = -mrc_provider_qp_destroy(qp->provider);
    if (!error) {
        mrc_ctl_store_release(&qp->ev_lease);
        mrc_ctl_store_release(&qp->cc_lease);
        mrc_abi_hint_release(qp->attrs.qp_hint);
        qp->attrs.qp_hint = NULL;
    }
    pthread_mutex_unlock(&qp->mutex);
    if (error)
        return error;
    mrc_abi_async_finish(&qp->async);
    pthread_mutex_lock(&context->mutex);
    struct mrc_abi_pd **slot = find_pd(context, qp->pd->verbs);
    (*slot)->users--;
    remove_unused_pd(slot);
    pthread_mutex_unlock(&context->mutex);
    pthread_mutex_destroy(&qp->mutex);
    free(qp);
    return 0;
}

MRC_PUBLIC int mrc_get_qpn(struct mrc_qp *qp, uint32_t *qpn)
{
    if (!qp || !qpn) {
        errno = EINVAL;
        return -1;
    }
    *qpn = qp->qpn;
    return 0;
}

static const enum ibv_qp_state verbs_states[] = {
    [MRC_PROVIDER_QP_RESET] = IBV_QPS_RESET,
    [MRC_PROVIDER_QP_INIT] = IBV_QPS_INIT,
    [MRC_PROVIDER_QP_RTR] = IBV_QPS_RTR,
    [MRC_PROVIDER_QP_RTS] = IBV_QPS_RTS,
    [MRC_PROVIDER_QP_ERROR] = IBV_QPS_ERR,
};

enum {
    INIT_VERBS = IBV_QP_PKEY_INDEX | IBV_QP_ACCESS_FLAGS,
    RTR_VERBS = IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN,
    RTS_VERBS = IBV_QP_SQ_PSN,
    STATE_VERBS = IBV_QP_STATE | IBV_QP_CUR_STATE,
    MODIFY_VERBS = INIT_VERBS | RTR_VERBS | RTS_VERBS | STATE_VERBS,
    INIT_MRC = MRC_QP_EV_PROFILE_ID | MRC_QP_CC_PROFILE_ID | MRC_QP_HINT,
    RTR_MRC = MRC_QP_MPR_DEST | MRC_QP_MAX_WIMM_DEST | MRC_QP_CAP,
    RTS_MRC = MRC_QP_MPR | MRC_QP_MAX_WIMM | MRC_QP_TIMEOUT | MRC_QP_RETRY_CNT,
    MODIFY_MRC = INIT_MRC | RTR_MRC | RTS_MRC,
};

static void apply_verbs(struct ibv_qp_attr *dst, const struct ibv_qp_attr *src, unsigned int mask)
{
    if (mask & IBV_QP_PKEY_INDEX)
        dst->pkey_index = src->pkey_index;
    if (mask & IBV_QP_ACCESS_FLAGS)
        dst->qp_access_flags = src->qp_access_flags;
    if (mask & IBV_QP_AV)
        dst->ah_attr = src->ah_attr;
    if (mask & IBV_QP_PATH_MTU)
        dst->path_mtu = src->path_mtu;
    if (mask & IBV_QP_DEST_QPN)
        dst->dest_qp_num = src->dest_qp_num;
    if (mask & IBV_QP_RQ_PSN)
        dst->rq_psn = src->rq_psn;
    if (mask & IBV_QP_SQ_PSN)
        dst->sq_psn = src->sq_psn;
}

static void apply_mrc(struct mrc_qp_attr *dst, const struct mrc_qp_attr *src, unsigned int mask)
{
    if (mask & MRC_QP_MPR)
        dst->mpr.mpr = src->mpr.mpr;
    if (mask & MRC_QP_MPR_DEST)
        dst->mpr.mpr_dest = src->mpr.mpr_dest;
    if (mask & MRC_QP_MAX_WIMM)
        dst->wimm.max_wimm = src->wimm.max_wimm;
    if (mask & MRC_QP_MAX_WIMM_DEST)
        dst->wimm.max_wimm_dest = src->wimm.max_wimm_dest;
    if (mask & MRC_QP_TIMEOUT)
        dst->timeout = src->timeout;
    if (mask & MRC_QP_RETRY_CNT)
        dst->retry = src->retry;
    if (mask & MRC_QP_EV_PROFILE_ID)
        dst->profile.ev_profile_id = src->profile.ev_profile_id;
    if (mask & MRC_QP_CC_PROFILE_ID)
        dst->profile.cc_profile_id = src->profile.cc_profile_id;
    if (mask & MRC_QP_HINT)
        dst->qp_hint = src->qp_hint;
    if (mask & MRC_QP_CAP)
        dst->cap = src->cap;
}

static int validate_attrs(const struct mrc_qp_attr *attrs)
{
    const unsigned int supported_caps = MRC_QP_CAP_TRIM_NACK_DEST_UNSUPPORTED |
        MRC_QP_CAP_DYNAMIC_MPR | MRC_QP_CAP_SVC_TIME_DEST_UNSUPPORTED;
    if (!attrs->mpr.mpr || !attrs->mpr.mpr_dest ||
        attrs->wimm.max_wimm > MRC_ABI_MAX_WIMM || attrs->wimm.max_wimm_dest > MRC_ABI_MAX_WIMM ||
        attrs->timeout > 24 || attrs->retry.linear_cnt > 7 || attrs->retry.exp_cnt > 25)
        return EINVAL;
    if (attrs->cap & ~supported_caps)
        return EOPNOTSUPP;
    return 0;
}

static int select_transition(const struct mrc_provider_qp_attr *provider,
                              const struct ibv_qp_attr *verbs, unsigned int vmask,
                              unsigned int mmask, bool attached, enum mrc_provider_qp_state *state)
{
    *state = provider->state;
    if ((vmask & IBV_QP_CUR_STATE) && verbs->cur_qp_state != verbs_states[*state])
        return EINVAL;
    if (vmask & IBV_QP_STATE) {
        size_t index = 0;
        while (index < sizeof(verbs_states) / sizeof(verbs_states[0]) &&
               verbs->qp_state != verbs_states[index])
            index++;
        if (index == sizeof(verbs_states) / sizeof(verbs_states[0]))
            return EINVAL;
        *state = (enum mrc_provider_qp_state)index;
    }
    unsigned int allowed_verbs = STATE_VERBS;
    unsigned int allowed_mrc = 0;
    switch (*state) {
    case MRC_PROVIDER_QP_INIT:
        allowed_verbs |= INIT_VERBS;
        allowed_mrc = INIT_MRC;
        break;
    case MRC_PROVIDER_QP_RTR:
        allowed_verbs |= RTR_VERBS;
        allowed_mrc = RTR_MRC;
        if (*state != provider->state && (vmask & RTR_VERBS) != RTR_VERBS)
            return EINVAL;
        break;
    case MRC_PROVIDER_QP_RTS:
        allowed_verbs |= RTS_VERBS;
        allowed_mrc = RTS_MRC;
        if (attached && *state == provider->state)
            allowed_mrc |= MRC_QP_MPR_DEST;
        if (*state != provider->state && !(vmask & IBV_QP_SQ_PSN))
            return EINVAL;
        break;
    case MRC_PROVIDER_QP_ERROR:
    case MRC_PROVIDER_QP_RESET:
        break;
    }
    return (vmask & ~allowed_verbs) || (mmask & ~allowed_mrc) ? EINVAL : 0;
}

MRC_PUBLIC int mrc_modify_qp(struct mrc_qp *qp, struct ibv_qp_attr *verbs, int vmask,
                              struct mrc_qp_attr *attrs, int mmask)
{
    if (!qp || (vmask && !verbs) || (mmask && !attrs))
        return EINVAL;
    if (((unsigned int)vmask & ~MODIFY_VERBS) || ((unsigned int)mmask & ~MODIFY_MRC))
        return EOPNOTSUPP;

    struct mrc_ctl_store_ref new_ev_lease = MRC_CTL_STORE_REF_INITIALIZER;
    struct mrc_ctl_store_ref new_cc_lease = MRC_CTL_STORE_REF_INITIALIZER;
    struct mrc_ctl_store_cc_profile new_cc_profile = {0};
    struct mrc_qp_hint *new_hint = NULL;
    pthread_mutex_lock(&qp->mutex);
    struct mrc_provider_qp_attr provider;
    enum mrc_provider_qp_state state;
    int error = -mrc_provider_qp_query(qp->provider, &provider);
    if (!error)
        error = select_transition(&provider, verbs, vmask, mmask, qp->route != NULL, &state);
    if (!error && qp->route && state != MRC_PROVIDER_QP_RESET && state != MRC_PROVIDER_QP_ERROR &&
        (state != provider.state || (vmask & ~STATE_VERBS) || mmask)) {
        error = EBUSY;
        if (state == provider.state && !(vmask & ~STATE_VERBS) && mmask == MRC_QP_MPR_DEST) {
            uint8_t units = attrs->mpr.mpr_dest;
            error = mrc_abi_set_rx_mpr(qp, units);
            if (!error)
                qp->attrs.mpr.mpr_dest = units;
        }
        goto unlock;
    }
    if (error)
        goto unlock;

    struct ibv_qp_attr updated_verbs = qp->verbs;
    struct mrc_qp_attr updated_attrs = qp->attrs;
    union ibv_gid source = qp->source_gid;
    apply_verbs(&updated_verbs, verbs, vmask);
    apply_mrc(&updated_attrs, attrs, mmask);

    error = validate_attrs(&updated_attrs);
    if (error)
        goto unlock;
    if (updated_verbs.qp_access_flags & ~(IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE)) {
        error = EOPNOTSUPP;
        goto unlock;
    }
    if (updated_verbs.path_mtu < IBV_MTU_256 || updated_verbs.path_mtu > IBV_MTU_4096) {
        error = EOPNOTSUPP;
        goto unlock;
    }
    if (updated_verbs.rq_psn > MRC_PSN_MASK || updated_verbs.sq_psn > MRC_PSN_MASK ||
        ((vmask & IBV_QP_DEST_QPN) &&
         (updated_verbs.dest_qp_num < 3 || updated_verbs.dest_qp_num > MRC_PSN_MASK))) {
        error = EINVAL;
        goto unlock;
    }
    if (vmask & IBV_QP_AV) {
        const struct ibv_ah_attr *ah = &updated_verbs.ah_attr;
        if (!ah->is_global || !ah->port_num || ah->sl >= MRC_ABI_SL_COUNT) {
            error = EINVAL;
            goto unlock;
        }
        error = ibv_query_gid(qp->context->verbs, ah->port_num, ah->grh.sgid_index, &source);
        if (error) {
            if (error < 0)
                error = errno ? errno : -error;
            goto unlock;
        }
    }

    if (provider.state == MRC_PROVIDER_QP_RESET && state == MRC_PROVIDER_QP_INIT) {
        error = mrc_abi_async_prepare(&qp->async);
        if (error)
            goto unlock;
    }
    uint64_t ev_profile_id = state == MRC_PROVIDER_QP_RESET ? 0 : updated_attrs.profile.ev_profile_id;
    bool change_ev = ev_profile_id != qp->attrs.profile.ev_profile_id;
    if (change_ev && ev_profile_id) {
        struct mrc_ctl_store store = mrc_abi_profile_store(qp->context);
        if (store.fd < 0) {
            error = ENODEV;
            goto unlock;
        }
        struct mrc_ctl_store_profile profile;
        error = -mrc_ctl_store_acquire(&store, ev_profile_id, &new_ev_lease, &profile);
        if (error)
            goto unlock;
    }

    uint64_t cc_profile_id = state == MRC_PROVIDER_QP_RESET ? 0 : updated_attrs.profile.cc_profile_id;
    bool change_cc = cc_profile_id != qp->attrs.profile.cc_profile_id;
    if (change_cc && cc_profile_id) {
        struct mrc_ctl_store store = mrc_abi_profile_store(qp->context);
        if (store.fd < 0) {
            error = ENODEV;
            goto unlock;
        }
        error = -mrc_ctl_store_acquire_cc(&store, cc_profile_id, &new_cc_lease, &new_cc_profile);
        if (error)
            goto unlock;
    }

    struct mrc_qp_hint *requested_hint = state == MRC_PROVIDER_QP_RESET ? NULL : updated_attrs.qp_hint;
    bool change_hint = requested_hint != qp->attrs.qp_hint;
    if (change_hint && requested_hint) {
        error = mrc_abi_hint_retain(qp->context, requested_hint);
        if (error)
            goto unlock;
        new_hint = requested_hint;
    }

    if (state == MRC_PROVIDER_QP_RESET) {
        error = -mrc_provider_qp_cancel(qp->provider);
        if (!error) {
            mrc_abi_release_route(qp);
            error = -mrc_provider_qp_reset(qp->provider);
        }
    } else {
        error = -mrc_provider_qp_transition(qp->provider, provider.state, state);
        /* ERROR transitions flush work before the datapath loses its state.
         * A CQ overflow may make the flush return an error after ERROR was
         * entered, but the transport must still be detached in that case. */
        if (state == MRC_PROVIDER_QP_ERROR) {
            struct mrc_provider_qp_attr stopped;
            if (mrc_provider_qp_query(qp->provider, &stopped) == 0 &&
                stopped.state == MRC_PROVIDER_QP_ERROR)
                mrc_abi_release_route(qp);
        }
    }
    if (!error && change_ev) {
        mrc_ctl_store_release(&qp->ev_lease);
        qp->ev_lease = new_ev_lease;
        new_ev_lease.fd = -1;
    }
    if (!error && change_cc) {
        mrc_ctl_store_release(&qp->cc_lease);
        qp->cc_lease = new_cc_lease;
        qp->cc_profile = new_cc_profile;
        new_cc_lease.fd = -1;
    }
    if (!error && change_hint) {
        mrc_abi_hint_release(qp->attrs.qp_hint);
        new_hint = NULL;
    }
    if (!error && state == MRC_PROVIDER_QP_RESET) {
        qp->verbs = (struct ibv_qp_attr){ .cap = qp->init.cap, .path_mtu = IBV_MTU_1024 };
        qp->attrs = default_qp_attrs();
        qp->source_gid = (union ibv_gid){0};
    } else if (!error) {
        qp->verbs = updated_verbs;
        qp->attrs = updated_attrs;
        qp->source_gid = source;
    }

unlock:
    mrc_abi_hint_release(new_hint);
    mrc_ctl_store_release(&new_ev_lease);
    mrc_ctl_store_release(&new_cc_lease);
    pthread_mutex_unlock(&qp->mutex);
    return error;
}

MRC_PUBLIC int mrc_query_qp(struct mrc_qp *qp, struct ibv_qp_attr *verbs, int vmask,
                             struct mrc_qp_attr *attrs, int mmask, struct mrc_qp_init_attr *init)
{
    if (!qp || (vmask && !verbs) || (mmask && !attrs))
        return EINVAL;
    if (((unsigned int)vmask & ~(MODIFY_VERBS | IBV_QP_CAP)) || ((unsigned int)mmask & ~MODIFY_MRC))
        return EOPNOTSUPP;
    pthread_mutex_lock(&qp->mutex);
    struct mrc_provider_qp_attr provider;
    int error = -mrc_provider_qp_query(qp->provider, &provider);
    if (!error && verbs) {
        *verbs = (struct ibv_qp_attr){0};
        apply_verbs(verbs, &qp->verbs, vmask);
        if (vmask & IBV_QP_STATE)
            verbs->qp_state = verbs_states[provider.state];
        if (vmask & IBV_QP_CUR_STATE)
            verbs->cur_qp_state = verbs_states[provider.state];
        if (vmask & IBV_QP_CAP)
            verbs->cap = qp->init.cap;
    }
    if (!error && attrs) {
        *attrs = (struct mrc_qp_attr){0};
        apply_mrc(attrs, &qp->attrs, mmask);
        if (mmask & MRC_QP_MPR)
            attrs->mpr.mpr = mrc_abi_requester_mpr(qp);
    }
    if (!error && init)
        *init = qp->init;
    pthread_mutex_unlock(&qp->mutex);
    return error;
}

static int post_write(struct mrc_qp *qp, const struct ibv_send_wr *wr)
{
    bool immediate = wr->opcode == IBV_WR_RDMA_WRITE_WITH_IMM;
    if (!immediate && wr->opcode != IBV_WR_RDMA_WRITE)
        return EOPNOTSUPP;
    if (wr->send_flags & ~(IBV_SEND_SIGNALED | IBV_SEND_SOLICITED | IBV_SEND_INLINE))
        return EOPNOTSUPP;
    bool solicited = (wr->send_flags & IBV_SEND_SOLICITED) != 0;
    if (solicited && !immediate)
        return EINVAL;
    if (wr->num_sge < 0 || (unsigned int)wr->num_sge > qp->init.cap.max_send_sge ||
        (wr->num_sge && !wr->sg_list) || (immediate && !qp->attrs.wimm.max_wimm))
        return EINVAL;

    bool inlined = (wr->send_flags & IBV_SEND_INLINE) != 0;
    if (inlined && !qp->init.cap.max_inline_data)
        return EOPNOTSUPP;
    struct mrc_provider_sge elements[MAX_SGE];
    struct mrc_provider_inline_sge inline_elements[MAX_SGE];
    uint64_t length = 0;
    for (int i = 0; i < wr->num_sge; i++) {
        const struct ibv_sge *sg = &wr->sg_list[i];
        if (inlined) {
            uintptr_t address = (uintptr_t)sg->addr;
            if ((uint64_t)address != sg->addr)
                return EOVERFLOW;
            inline_elements[i] = (struct mrc_provider_inline_sge){
                .data = (const void *)address, .length = sg->length,
            };
        } else {
            elements[i] = (struct mrc_provider_sge){
                .iova = sg->addr, .length = sg->length, .lkey = sg->lkey,
            };
        }
        length += sg->length;
    }
    if (length > MAX_WRITE_BYTES || (inlined && length > qp->init.cap.max_inline_data))
        return EMSGSIZE;
    const struct mrc_provider_write write = {
        .wr_id = wr->wr_id, .remote_address = wr->wr.rdma.remote_addr, .rkey = wr->wr.rdma.rkey,
        .length = (uint32_t)length, .with_immediate = immediate, .solicited = solicited,
        .immediate = immediate ? ntohl(wr->imm_data) : 0,
        .signaled = (wr->send_flags & IBV_SEND_SIGNALED) != 0,
    };
    uint64_t sequence;
    if (inlined)
        return -mrc_provider_qp_post_write_inline(qp->provider, &write, inline_elements, wr->num_sge, &sequence);
    return -mrc_provider_qp_post_writev(qp->provider, &write, elements, wr->num_sge, &sequence);
}

MRC_PUBLIC int mrc_post_send(struct mrc_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad_wr)
{
    if (!bad_wr)
        return EINVAL;
    *bad_wr = wr;
    if (!qp)
        return EINVAL;

    pthread_mutex_lock(&qp->mutex);
    int error = wr && qp->route && !qp->requester ? EOPNOTSUPP : 0;
    while (wr && !error) {
        error = post_write(qp, wr);
        if (!error)
            wr = wr->next;
    }
    pthread_mutex_unlock(&qp->mutex);
    *bad_wr = wr;
    return error;
}

MRC_PUBLIC int mrc_post_recv(struct mrc_qp *qp, struct ibv_recv_wr *wr, struct ibv_recv_wr **bad_wr)
{
    if (bad_wr)
        *bad_wr = wr;
    if (!qp || !bad_wr) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&qp->mutex);
    int error = wr && qp->route && !qp->responder ? EOPNOTSUPP : 0;
    while (wr && !error) {
        if (wr->num_sge < 0 || (unsigned int)wr->num_sge > qp->init.cap.max_recv_sge ||
            (wr->num_sge && !wr->sg_list)) {
            error = EINVAL;
            break;
        }
        /* WRITEIMM places data at the RETH address and consumes only a receive credit. */
        error = -mrc_provider_qp_post_recv(qp->provider, wr->wr_id);
        if (!error)
            wr = wr->next;
    }
    pthread_mutex_unlock(&qp->mutex);
    *bad_wr = wr;
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}
