#include "mrc_abi.h"
#include "mrc_qp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>

enum {
    MAX_SEND_SGE = 16,
    MAX_QUEUE_DEPTH = 4096,
    MAX_REGISTERED_MRS = 4096,
    MAX_WRITE_BYTES = 1U << 30,
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
        (attr->send_cq && attr->send_cq->context != context) ||
        (attr->recv_cq && attr->recv_cq->context != context))
        return EINVAL;
    if (attr->protocol_version != MRC_PROTOCOL_VERSION_0 &&
        attr->protocol_version != MRC_PROTOCOL_VERSION_1)
        return EPROTONOSUPPORT;
    if (attr->cap.max_send_wr > MAX_QUEUE_DEPTH || attr->cap.max_recv_wr > MAX_QUEUE_DEPTH ||
        attr->cap.max_send_sge > MAX_SEND_SGE || attr->cap.max_recv_sge ||
        attr->cap.max_inline_data || attr->send_ops_flags)
        return EOPNOTSUPP;
    return 0;
}

static struct mrc_qp_attr default_qp_attrs(void)
{
    return (struct mrc_qp_attr){
        .mpr = { .mpr = MRC_ABI_MAX_MPR, .mpr_dest = MRC_ABI_MAX_MPR },
        .timeout = 16, .retry = { .linear_cnt = 3, .exp_cnt = 4 },
    };
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
    qp->init.protocol_version = MRC_PROTOCOL_VERSION_1;
    qp->verbs = (struct ibv_qp_attr){ .cap = qp->init.cap, .path_mtu = IBV_MTU_1024 };
    qp->attrs = default_qp_attrs();
    *init = qp->init;
    return qp;

destroy_mutex:
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
    mrc_abi_release_route(qp);
    int error = -mrc_provider_qp_reset(qp->provider);
    if (!error)
        error = -mrc_provider_qp_destroy(qp->provider);
    pthread_mutex_unlock(&qp->mutex);
    if (error)
        return error;
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
    if (!attrs->mpr.mpr || attrs->mpr.mpr > MRC_ABI_MAX_MPR ||
        !attrs->mpr.mpr_dest || attrs->mpr.mpr_dest > MRC_ABI_MAX_MPR ||
        attrs->wimm.max_wimm > MRC_ABI_MAX_WIMM || attrs->wimm.max_wimm_dest > MRC_ABI_MAX_WIMM ||
        attrs->timeout > 24 || attrs->retry.linear_cnt > 7 || attrs->retry.exp_cnt > 25)
        return EINVAL;
    if ((attrs->cap & ~supported_caps) || attrs->profile.ev_profile_id ||
        attrs->profile.cc_profile_id || attrs->qp_hint)
        return EOPNOTSUPP;
    return 0;
}

static int select_transition(const struct mrc_provider_qp_attr *provider,
                              const struct ibv_qp_attr *verbs, unsigned int vmask,
                              unsigned int mmask, enum mrc_provider_qp_state *state)
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

    pthread_mutex_lock(&qp->mutex);
    struct mrc_provider_qp_attr provider;
    enum mrc_provider_qp_state state;
    int error = -mrc_provider_qp_query(qp->provider, &provider);
    if (!error)
        error = select_transition(&provider, verbs, vmask, mmask, &state);
    if (!error && qp->route && state != MRC_PROVIDER_QP_RESET && state != MRC_PROVIDER_QP_ERROR &&
        (state != provider.state || (vmask & ~STATE_VERBS) || mmask))
        error = EBUSY;
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
    if (updated_verbs.path_mtu != IBV_MTU_1024) {
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
        if (!ah->is_global || !ah->port_num) {
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

    if (state == MRC_PROVIDER_QP_RESET || state == MRC_PROVIDER_QP_ERROR)
        mrc_abi_release_route(qp);
    error = state == MRC_PROVIDER_QP_RESET ? -mrc_provider_qp_reset(qp->provider)
        : -mrc_provider_qp_transition(qp->provider, provider.state, state);
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
    if (wr->send_flags & ~(IBV_SEND_SIGNALED | IBV_SEND_SOLICITED))
        return EOPNOTSUPP;
    bool solicited = (wr->send_flags & IBV_SEND_SOLICITED) != 0;
    if (solicited && !immediate)
        return EINVAL;
    if (wr->num_sge < 0 || (unsigned int)wr->num_sge > qp->init.cap.max_send_sge ||
        (wr->num_sge && !wr->sg_list) || (immediate && !qp->attrs.wimm.max_wimm))
        return EINVAL;

    struct mrc_provider_sge elements[MAX_SEND_SGE];
    uint64_t length = 0;
    for (int i = 0; i < wr->num_sge; i++) {
        const struct ibv_sge *sg = &wr->sg_list[i];
        elements[i] = (struct mrc_provider_sge){ .iova = sg->addr, .length = sg->length, .lkey = sg->lkey };
        length += sg->length;
    }
    if (length > MAX_WRITE_BYTES)
        return EMSGSIZE;
    const struct mrc_provider_write write = {
        .wr_id = wr->wr_id, .remote_address = wr->wr.rdma.remote_addr, .rkey = wr->wr.rdma.rkey,
        .length = (uint32_t)length, .with_immediate = immediate, .solicited = solicited,
        .immediate = immediate ? ntohl(wr->imm_data) : 0,
        .signaled = (wr->send_flags & IBV_SEND_SIGNALED) != 0,
    };
    uint64_t sequence;
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
        error = wr->num_sge ? EINVAL : -mrc_provider_qp_post_recv(qp->provider, wr->wr_id);
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
