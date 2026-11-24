#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <infiniband/driver.h>

#include "../kernel/roce_mr_uapi.h"
#include "../src/roce_mr_ipc.h"
#include "../src/roce_native_queue.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(IBVERBS_PABI_VERSION == 34, "Use libibverbs provider ABI 34");

struct roce_verbs_context {
    struct verbs_context verbs;
    struct roce_native_work *work;
    struct roce_native_queue_config config;
    struct roce_verbs_qp *early_qp;
    uint32_t next_qp;
    uint32_t cq_count;
    char *mr_socket;
    bool require_netlink;
};

struct roce_verbs_pd {
    struct ibv_pd ibv;
    atomic_size_t references;
    struct roce_verbs_mr *regions;
};

struct roce_verbs_mr {
    struct verbs_mr verbs;
    struct roce_verbs_mr *next;
    uint64_t iova;
    size_t length;
    unsigned qp_refs;
    int socket;
    bool live;
    bool dereg_pending;
};

struct roce_verbs_cq {
    struct ibv_cq ibv;
    struct roce_verbs_qp *sender;
    bool attached;
};

struct roce_verbs_qp {
    struct verbs_qp verbs;
    struct ibv_qp_init_attr init;
    struct ibv_qp_attr attributes;
    struct roce_verbs_mr *region;
    uint32_t index;
    uint32_t polled;
};

static const struct verbs_device_ops device_ops;

static struct roce_verbs_context *provider_context(struct ibv_context *ibv)
{
    return container_of(verbs_get_ctx(ibv), struct roce_verbs_context, verbs);
}

static uint64_t native_queue_bytes(const struct roce_native_queue_config *config)
{
    return (uint64_t)config->qps * config->writes * config->write_size;
}

static bool valid_queue_config(const struct roce_native_queue_config *config)
{
    if (config->version != ROCE_NATIVE_QUEUE_VERSION || (config->flags & ~ROCE_NATIVE_QUEUE_RECEIVE_ONLY) ||
        !config->qps || config->qps > ROCE_LAB_MAX_QPS || !config->workers || config->workers > config->qps ||
        !config->writes || config->writes > ROCE_PSN_MASK || !config->write_size ||
        config->local_qpn <= 1 || config->local_qpn >= ROCE_PSN_MASK ||
        config->peer_qpn <= 1 || config->peer_qpn >= ROCE_PSN_MASK ||
        config->qps > ROCE_PSN_MASK - config->local_qpn || config->qps > ROCE_PSN_MASK - config->peer_qpn ||
        config->local_psn > ROCE_PSN_MASK || config->peer_psn > ROCE_PSN_MASK ||
        config->pmtu < 256 || config->pmtu > ROCE_MAX_PMTU || (config->pmtu & (config->pmtu - 1)) ||
        !config->local_lkey || (!(config->flags & ROCE_NATIVE_QUEUE_RECEIVE_ONLY) && !config->remote_rkey) ||
        !config->local_ipv4 || !config->peer_ipv4)
        return false;

    uint64_t bytes = native_queue_bytes(config);
    return bytes - 1 <= UINT64_MAX - config->local_address &&
           bytes - 1 <= UINT64_MAX - config->remote_address;
}

static struct roce_verbs_mr *queue_mr(const struct roce_verbs_pd *pd,
                                     const struct roce_native_queue_config *config)
{
    uint64_t bytes = native_queue_bytes(config);
    bool receive_only = config->flags & ROCE_NATIVE_QUEUE_RECEIVE_ONLY;
    for (struct roce_verbs_mr *mr = pd->regions; mr; mr = mr->next)
        if (mr->live && mr->verbs.ibv_mr.lkey == config->local_lkey &&
            (!receive_only || (mr->verbs.access & IBV_ACCESS_REMOTE_WRITE)) &&
            config->local_address >= mr->iova && bytes <= mr->length &&
            config->local_address - mr->iova <= mr->length - bytes)
            return mr;
    return NULL;
}

/* The caller holds the context mutex for the early QP and its PD's MR list. */
static int bind_queue_work(struct roce_verbs_context *context, struct roce_native_work *work,
                           const struct roce_native_queue_config *config)
{
    if (context->work)
        return EBUSY;

    struct roce_verbs_qp *qp = context->early_qp;
    if (!qp) {
        context->config = *config;
        context->work = work;
        return 0;
    }

    struct ibv_qp *ibv = &qp->verbs.qp;
    if (config->qps != 1 || config->local_qpn != ibv->qp_num)
        return EINVAL;
    struct roce_verbs_pd *pd = container_of(ibv->pd, struct roce_verbs_pd, ibv);
    struct roce_verbs_mr *mr = queue_mr(pd, config);
    if (!mr)
        return EACCES;

    pthread_mutex_lock(&ibv->send_cq->mutex);
    context->config = *config;
    context->work = work;
    context->early_qp = NULL;
    context->next_qp = 1;
    qp->region = mr;
    ++mr->qp_refs;
    pthread_mutex_unlock(&ibv->send_cq->mutex);
    return 0;
}

int roce_dpdk_bind_queues(struct ibv_context *ibv, int fd)
{
    if (!ibv || !ibv->device || verbs_get_device(ibv->device)->ops != &device_ops)
        return EINVAL;

    int seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0)
        return errno;
    const int required_seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    if ((seals & required_seals) != required_seals)
        return EINVAL;
    struct stat information;
    if (fstat(fd, &information) < 0)
        return errno;
    if (!S_ISREG(information.st_mode) || information.st_size != (off_t)sizeof(struct roce_native_work))
        return EINVAL;

    struct roce_native_work *work = mmap(NULL, sizeof(*work), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (work == MAP_FAILED)
        return errno;
    struct roce_native_queue_config config = work->config;
    int error = EINVAL;
    if (!valid_queue_config(&config))
        goto unmap;
    error = EOPNOTSUPP;
    if (!atomic_is_lock_free(&work->qps[0].posted) || !atomic_is_lock_free(&work->doorbells[0].slots[0]))
        goto unmap;
    error = EBUSY;
    for (uint32_t i = 0; i < config.qps; ++i)
        if (atomic_load_explicit(&work->qps[i].posted, memory_order_acquire) ||
            atomic_load_explicit(&work->qps[i].admitted, memory_order_acquire) ||
            atomic_load_explicit(&work->qps[i].acknowledged, memory_order_acquire) ||
            atomic_load_explicit(&work->qps[i].retired, memory_order_acquire))
            goto unmap;

    struct roce_verbs_context *context = provider_context(ibv);
    pthread_mutex_lock(&ibv->mutex);
    error = bind_queue_work(context, work, &config);
    pthread_mutex_unlock(&ibv->mutex);
    if (!error)
        return 0;

unmap:
    munmap(work, sizeof(*work));
    return error;
}

static int query_device(struct ibv_context *context, const struct ibv_query_device_ex_input *input,
                        struct ibv_device_attr_ex *attributes, size_t length)
{
    struct ib_uverbs_ex_query_device_resp response = {0};
    size_t capacity = sizeof(response);
    int error = ibv_cmd_query_device_any(context, input, attributes, length, &response, &capacity);
    if (error)
        return error;
    memset(attributes->orig_attr.fw_ver, 0, sizeof(attributes->orig_attr.fw_ver));
    const struct roce_verbs_context *provider = provider_context(context);
    long page_size = sysconf(_SC_PAGESIZE);
    if (provider->mr_socket && page_size > 0) {
        attributes->orig_attr.max_mr = ROCE_MR_IPC_MAX_REGISTRATIONS;
        attributes->orig_attr.max_mr_size = (uint64_t)INT_MAX * (uint64_t)page_size;
    }
    pthread_mutex_lock(&context->mutex);
    if (provider->work || provider->mr_socket) {
        attributes->orig_attr.max_qp = provider->work ? provider->config.qps : 1;
        attributes->orig_attr.max_cq = 2 * attributes->orig_attr.max_qp;
        attributes->orig_attr.max_qp_wr = ROCE_NATIVE_QUEUE_DEPTH;
        attributes->orig_attr.max_cqe = ROCE_NATIVE_QUEUE_DEPTH;
        attributes->orig_attr.max_sge = 1;
    }
    pthread_mutex_unlock(&context->mutex);
    return 0;
}

static int query_port(struct ibv_context *context, uint8_t port, struct ibv_port_attr *attributes)
{
    struct ibv_query_port command = {0};
    return ibv_cmd_query_port(context, port, attributes, &command, sizeof(command));
}

static struct ibv_pd *alloc_pd(struct ibv_context *context)
{
    struct ibv_alloc_pd command = {0};
    struct ib_uverbs_alloc_pd_resp response = {0};
    struct roce_verbs_pd *pd = calloc(1, sizeof(*pd));
    if (!pd)
        return NULL;
    atomic_init(&pd->references, 0);
    int error = ibv_cmd_alloc_pd(context, &pd->ibv, &command, sizeof(command), &response, sizeof(response));
    if (error) {
        free(pd);
        errno = error;
        return NULL;
    }
    return &pd->ibv;
}

static int dealloc_pd(struct ibv_pd *ibv)
{
    struct roce_verbs_pd *pd = container_of(ibv, struct roce_verbs_pd, ibv);
    if (atomic_load_explicit(&pd->references, memory_order_acquire))
        return EBUSY;

    int error = ibv_cmd_dealloc_pd(ibv);
    if (!error)
        free(pd);
    return error;
}

static int mr_registration(void *address, size_t length, uint64_t iova, int access,
                           struct roce_mr_register *pin, struct roce_mr_request *request)
{
    if (access & ~(IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE))
        return EOPNOTSUPP;
    if ((access & IBV_ACCESS_REMOTE_WRITE) && !(access & IBV_ACCESS_LOCAL_WRITE))
        return EINVAL;

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || !address || !length)
        return EINVAL;

    uintptr_t start = (uintptr_t)address;
    size_t page = (size_t)page_size;
    size_t offset = start % page;
    if (iova % page != offset || length > SIZE_MAX - offset)
        return EINVAL;

    size_t span = offset + length;
    size_t pad = (page - span % page) % page;
    uintptr_t base = start - offset;
    if (span > SIZE_MAX - pad || span + pad > UINTPTR_MAX - base || length - 1 > UINT64_MAX - iova)
        return EINVAL;

    *pin = (struct roce_mr_register) {
        .address = base,
        .length = span + pad,
        .access = (access & IBV_ACCESS_LOCAL_WRITE) ? ROCE_MR_ACCESS_LOCAL_WRITE : 0,
    };
    *request = (struct roce_mr_request) {
        .version = ROCE_MR_IPC_VERSION,
        .access = ((access & IBV_ACCESS_LOCAL_WRITE) ? ROCE_MR_IPC_WRITE : 0) |
                  ((access & IBV_ACCESS_REMOTE_WRITE) ? ROCE_MR_IPC_REMOTE_WRITE : 0),
        .iova = iova,
        .offset = offset,
        .length = length,
        .mapped_length = pin->length,
    };
    return 0;
}

static int pin_mapping(const struct roce_mr_register *request)
{
    int device = open(ROCE_MR_DEVICE, O_RDWR | O_CLOEXEC);
    if (device < 0)
        return -1;

    int fd = ioctl(device, ROCE_MR_REGISTER, request);
    int error = errno;
    close(device);
    errno = error;
    return fd;
}

static int offer_ticket(int socket, const struct roce_mr_request *request, int mapping,
                        bool *unsupported)
{
    struct roce_mr_request message = *request;
    message.version = ROCE_MR_IPC_TICKET_VERSION;
    int error = roce_mr_ipc_send_registration(socket, &message, -1);
    if (error)
        return error;
    struct roce_mr_ticket_ready ready;
    error = roce_mr_ipc_read_ticket_ready(socket, &ready);
    if (error)
        return error;
    if (ready.error) {
        *unsupported = ready.error == EOPNOTSUPP;
        return ready.error;
    }
    const struct roce_mr_export_ticket ticket = {
        .worker_socket = socket, .portid = ready.portid, .cookie = ready.cookie,
    };
    return ioctl(mapping, ROCE_MR_EXPORT_TICKET, &ticket) < 0 ? errno : 0;
}

static int send_mapping(const struct roce_verbs_context *context, const struct roce_mr_request *request,
                         int mapping, struct roce_verbs_mr *mr)
{
    mr->socket = roce_mr_ipc_connect(context->mr_socket);
    if (mr->socket < 0)
        return errno;
    bool unsupported = false;
    int error = offer_ticket(mr->socket, request, mapping, &unsupported);
    if (!unsupported || context->require_netlink)
        return error;
    close(mr->socket);
    mr->socket = roce_mr_ipc_connect(context->mr_socket);
    if (mr->socket < 0)
        return errno;
    return roce_mr_ipc_send_registration(mr->socket, request, mapping);
}

static struct ibv_mr *reg_mr(struct ibv_pd *ibv, void *address, size_t length, uint64_t iova, int access)
{
    struct roce_verbs_context *context =
        container_of(verbs_get_ctx(ibv->context), struct roce_verbs_context, verbs);
    struct roce_verbs_pd *pd = container_of(ibv, struct roce_verbs_pd, ibv);
    struct roce_mr_register pin;
    struct roce_mr_request request;
    if (!context->mr_socket) {
        errno = EOPNOTSUPP;
        return NULL;
    }

    int error = mr_registration(address, length, iova, access, &pin, &request);
    if (error) {
        errno = error;
        return NULL;
    }

    struct roce_verbs_mr *mr = calloc(1, sizeof(*mr));
    if (!mr)
        return NULL;
    atomic_fetch_add_explicit(&pd->references, 1, memory_order_relaxed);

    int mapping = pin_mapping(&pin);
    if (mapping < 0) {
        error = errno;
        goto free_mr;
    }

    struct roce_mr_reply reply;
    error = send_mapping(context, &request, mapping, mr);
    if (!error)
        error = roce_mr_ipc_read_reply(mr->socket, &reply);
    /* Keep the export alive until the worker has imported its ticket. */
    close(mapping);
    if (!error)
        error = reply.error;
    if (error)
        goto close_socket;

    mr->verbs.mr_type = IBV_MR_TYPE_MR;
    mr->verbs.access = access;
    mr->verbs.ibv_mr.lkey = reply.lkey;
    mr->verbs.ibv_mr.rkey = reply.rkey;
    mr->iova = iova;
    mr->length = length;
    mr->live = true;
    pthread_mutex_lock(&ibv->context->mutex);
    mr->next = pd->regions;
    pd->regions = mr;
    pthread_mutex_unlock(&ibv->context->mutex);
    return &mr->verbs.ibv_mr;

close_socket:
    if (mr->socket >= 0)
        close(mr->socket);
free_mr:
    atomic_fetch_sub_explicit(&pd->references, 1, memory_order_release);
    free(mr);
    errno = error;
    return NULL;
}

static int dereg_mr(struct verbs_mr *verbs)
{
    struct roce_verbs_mr *mr = container_of(verbs, struct roce_verbs_mr, verbs);
    struct roce_verbs_pd *pd = container_of(verbs->ibv_mr.pd, struct roce_verbs_pd, ibv);
    pthread_mutex_t *mutex = &pd->ibv.context->mutex;
    pthread_mutex_lock(mutex);
    if (mr->qp_refs) {
        pthread_mutex_unlock(mutex);
        return EBUSY;
    }
    mr->live = false;
    pthread_mutex_unlock(mutex);

    /* A receive timeout leaves the original request pending on this channel. */
    if (!mr->dereg_pending) {
        const char command = ROCE_MR_IPC_DEREGISTER;
        ssize_t sent;
        do {
            sent = send(mr->socket, &command, sizeof(command), MSG_NOSIGNAL);
        } while (sent < 0 && errno == EINTR);
        if (sent < 0)
            return errno;
        if (sent != sizeof(command))
            return EIO;
        mr->dereg_pending = true;
    }

    struct roce_mr_reply reply;
    int error = roce_mr_ipc_read_reply(mr->socket, &reply);
    if (error)
        return error;
    mr->dereg_pending = false;
    pthread_mutex_lock(mutex);
    if (reply.error) {
        mr->live = true;
        pthread_mutex_unlock(mutex);
        return reply.error;
    }

    struct roce_verbs_mr **link = &pd->regions;
    while (*link != mr)
        link = &(*link)->next;
    *link = mr->next;
    pthread_mutex_unlock(mutex);

    close(mr->socket);
    atomic_fetch_sub_explicit(&pd->references, 1, memory_order_release);
    free(mr);
    return 0;
}

static struct ibv_cq *create_cq(struct ibv_context *ibv, int cqe, struct ibv_comp_channel *channel,
                               int comp_vector)
{
    struct roce_verbs_context *context = provider_context(ibv);
    struct roce_verbs_cq *cq = calloc(1, sizeof(*cq));
    if (!cq)
        return NULL;

    pthread_mutex_lock(&ibv->mutex);
    uint32_t qp_capacity = context->work ? context->config.qps : 1;
    int error = 0;
    if ((!context->work && !context->mr_socket) || channel || comp_vector)
        error = EOPNOTSUPP;
    else if (cqe <= 0 || cqe > ROCE_NATIVE_QUEUE_DEPTH)
        error = EINVAL;
    else if (context->cq_count == 2 * qp_capacity)
        error = ENOSPC;
    else
        ++context->cq_count;
    pthread_mutex_unlock(&ibv->mutex);
    if (error) {
        free(cq);
        errno = error;
        return NULL;
    }

    cq->ibv.cqe = cqe;
    return &cq->ibv;
}

static int destroy_cq(struct ibv_cq *ibv)
{
    struct roce_verbs_cq *cq = container_of(ibv, struct roce_verbs_cq, ibv);
    struct ibv_context *context = ibv->context;
    pthread_mutex_lock(&context->mutex);
    bool attached = cq->attached;
    if (!attached)
        --provider_context(context)->cq_count;
    pthread_mutex_unlock(&context->mutex);
    if (attached)
        return EBUSY;

    pthread_cond_destroy(&ibv->cond);
    pthread_mutex_destroy(&ibv->mutex);
    free(cq);
    return 0;
}

static int attach_qp(struct roce_verbs_qp *qp, struct ibv_pd *ibv_pd,
                     struct roce_verbs_cq *send_cq, struct roce_verbs_cq *recv_cq)
{
    struct ibv_context *ibv = ibv_pd->context;
    struct roce_verbs_context *context = provider_context(ibv);
    struct roce_verbs_pd *pd = container_of(ibv_pd, struct roce_verbs_pd, ibv);
    int error = 0;
    pthread_mutex_lock(&ibv->mutex);
    if (!context->work && !context->mr_socket) {
        error = EOPNOTSUPP;
    } else if (context->early_qp || (context->work && context->next_qp == context->config.qps)) {
        error = ENOSPC;
    } else if (send_cq->attached || recv_cq->attached) {
        error = EBUSY;
    } else {
        if (context->work) {
            qp->region = queue_mr(pd, &context->config);
            if (!qp->region) {
                error = EACCES;
                goto unlock;
            }
            ++qp->region->qp_refs;
            qp->index = context->next_qp++;
            qp->verbs.qp.qp_num = context->config.local_qpn + qp->index;
        } else {
            uint32_t qpn;
            if (getentropy(&qpn, sizeof(qpn)) < 0) {
                error = errno;
                goto unlock;
            }
            qp->verbs.qp.qp_num = 2 + qpn % (ROCE_PSN_MASK - 2);
            context->early_qp = qp;
        }
        pthread_mutex_lock(&send_cq->ibv.mutex);
        send_cq->sender = qp;
        send_cq->attached = recv_cq->attached = true;
        atomic_fetch_add_explicit(&pd->references, 1, memory_order_relaxed);
        pthread_mutex_unlock(&send_cq->ibv.mutex);
    }
unlock:
    pthread_mutex_unlock(&ibv->mutex);
    return error;
}

static struct ibv_qp *create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *attr)
{
    int error;
    if (!attr || !attr->send_cq || !attr->recv_cq || attr->send_cq->context != pd->context ||
        attr->recv_cq->context != pd->context || !attr->cap.max_send_wr ||
        attr->cap.max_send_wr > ROCE_NATIVE_QUEUE_DEPTH || (attr->sq_sig_all != 0 && attr->sq_sig_all != 1)) {
        errno = EINVAL;
        return NULL;
    }
    if (attr->qp_type != IBV_QPT_RC || attr->srq || attr->cap.max_send_sge != 1 ||
        attr->cap.max_recv_wr || attr->cap.max_recv_sge || attr->cap.max_inline_data) {
        errno = EOPNOTSUPP;
        return NULL;
    }

    struct roce_verbs_qp *qp = calloc(1, sizeof(*qp));
    if (!qp)
        return NULL;
    qp->init = *attr;
    qp->attributes = (struct ibv_qp_attr) { .qp_state = IBV_QPS_RESET, .cap = attr->cap };
    struct ibv_qp *ibv = &qp->verbs.qp;
    *ibv = (struct ibv_qp) {
        .context = pd->context, .qp_context = attr->qp_context, .pd = pd,
        .send_cq = attr->send_cq, .recv_cq = attr->recv_cq,
        .qp_type = IBV_QPT_RC, .state = IBV_QPS_RESET,
    };
    error = pthread_mutex_init(&ibv->mutex, NULL);
    if (error)
        goto free_qp;
    error = pthread_cond_init(&ibv->cond, NULL);
    if (error)
        goto destroy_mutex;

    struct roce_verbs_cq *send_cq = container_of(attr->send_cq, struct roce_verbs_cq, ibv);
    struct roce_verbs_cq *recv_cq = container_of(attr->recv_cq, struct roce_verbs_cq, ibv);
    error = attach_qp(qp, pd, send_cq, recv_cq);
    if (!error)
        return ibv;

    pthread_cond_destroy(&ibv->cond);
destroy_mutex:
    pthread_mutex_destroy(&ibv->mutex);
free_qp:
    free(qp);
    errno = error;
    return NULL;
}

static struct roce_native_sq *qp_sq(const struct roce_verbs_qp *qp)
{
    /* The send CQ mutex also protects publication of an early QP's region. */
    if (!qp->region)
        return NULL;
    return &provider_context(qp->verbs.qp.context)->work->qps[qp->index];
}

static int destroy_qp(struct ibv_qp *ibv)
{
    struct roce_verbs_qp *qp = container_of(ibv, struct roce_verbs_qp, verbs.qp);
    struct roce_verbs_pd *pd = container_of(ibv->pd, struct roce_verbs_pd, ibv);
    struct roce_verbs_context *context = provider_context(ibv->context);
    struct roce_verbs_cq *send_cq = container_of(ibv->send_cq, struct roce_verbs_cq, ibv);
    struct roce_verbs_cq *recv_cq = container_of(ibv->recv_cq, struct roce_verbs_cq, ibv);

    pthread_mutex_lock(&ibv->context->mutex);
    pthread_mutex_lock(&send_cq->ibv.mutex);
    const struct roce_native_sq *sq = qp_sq(qp);
    bool pending = sq &&
        roce_native_posted_count(atomic_load_explicit(&sq->posted, memory_order_relaxed)) != qp->polled;
    if (!pending) {
        send_cq->sender = NULL;
        send_cq->attached = recv_cq->attached = false;
        if (qp->region)
            --qp->region->qp_refs;
        else
            context->early_qp = NULL;
        atomic_fetch_sub_explicit(&pd->references, 1, memory_order_release);
    }
    pthread_mutex_unlock(&send_cq->ibv.mutex);
    pthread_mutex_unlock(&ibv->context->mutex);
    if (pending)
        return EBUSY;

    pthread_cond_destroy(&ibv->cond);
    pthread_mutex_destroy(&ibv->mutex);
    free(qp);
    return 0;
}

enum {
    QP_INIT_MASK = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS,
    QP_RTR_MASK = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                  IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER,
    QP_RTS_MASK = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                  IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC,
};

static bool matches_peer(const struct ibv_ah_attr *ah, uint32_t peer_ipv4)
{
    const uint8_t gid[16] = {
        [10] = 0xff, 0xff, (uint8_t)(peer_ipv4 >> 24), (uint8_t)(peer_ipv4 >> 16),
        (uint8_t)(peer_ipv4 >> 8), (uint8_t)peer_ipv4,
    };
    return ah->port_num == 1 && ah->is_global && !memcmp(ah->grh.dgid.raw, gid, sizeof(gid));
}

static int change_qp_state(struct roce_verbs_qp *qp, const struct ibv_qp_attr *attr, int mask)
{
    const struct roce_native_queue_config *config = &provider_context(qp->verbs.qp.context)->config;
    struct ibv_qp_attr *current = &qp->attributes;
    if (mask & IBV_QP_CUR_STATE) {
        if (attr->cur_qp_state != current->qp_state)
            return EINVAL;
        mask &= ~IBV_QP_CUR_STATE;
    }
    if (!(mask & IBV_QP_STATE))
        return EOPNOTSUPP;

    switch (attr->qp_state) {
    case IBV_QPS_INIT:
        if (current->qp_state != IBV_QPS_RESET || mask != QP_INIT_MASK || attr->port_num != 1 ||
            attr->pkey_index || (attr->qp_access_flags & ~IBV_ACCESS_REMOTE_WRITE))
            return EINVAL;
        current->port_num = attr->port_num;
        current->pkey_index = attr->pkey_index;
        current->qp_access_flags = attr->qp_access_flags;
        break;
    case IBV_QPS_RTR:
        if (!qp->region)
            return EAGAIN;
        if (current->qp_state != IBV_QPS_INIT || mask != QP_RTR_MASK ||
            attr->path_mtu < IBV_MTU_256 || attr->path_mtu > IBV_MTU_4096 ||
            (UINT32_C(128) << attr->path_mtu) != config->pmtu || attr->dest_qp_num != config->peer_qpn + qp->index ||
            attr->rq_psn != roce_native_psn(config->peer_psn, qp->index) || attr->max_dest_rd_atomic ||
            attr->min_rnr_timer > 31 || !matches_peer(&attr->ah_attr, config->peer_ipv4))
            return EINVAL;
        current->path_mtu = attr->path_mtu;
        current->dest_qp_num = attr->dest_qp_num;
        current->rq_psn = attr->rq_psn;
        current->min_rnr_timer = attr->min_rnr_timer;
        current->ah_attr = attr->ah_attr;
        break;
    case IBV_QPS_RTS:
        if (!qp->region)
            return EAGAIN;
        if (current->qp_state != IBV_QPS_RTR || mask != QP_RTS_MASK || attr->max_rd_atomic ||
            attr->sq_psn != roce_native_psn(config->local_psn, qp->index) ||
            attr->timeout > 31 || attr->retry_cnt > 7 || attr->rnr_retry > 7)
            return EINVAL;
        current->sq_psn = attr->sq_psn;
        current->timeout = attr->timeout;
        current->retry_cnt = attr->retry_cnt;
        current->rnr_retry = attr->rnr_retry;
        break;
    case IBV_QPS_ERR:
        if (mask != IBV_QP_STATE)
            return EINVAL;
        break;
    default:
        return EOPNOTSUPP;
    }
    current->cur_qp_state = current->qp_state = attr->qp_state;
    return 0;
}

static int modify_qp(struct ibv_qp *ibv, struct ibv_qp_attr *attr, int mask)
{
    struct roce_verbs_qp *qp = container_of(ibv, struct roce_verbs_qp, verbs.qp);
    if (!attr)
        return EINVAL;
    pthread_mutex_lock(&ibv->send_cq->mutex);
    int error = change_qp_state(qp, attr, mask);
    pthread_mutex_unlock(&ibv->send_cq->mutex);
    return error;
}

static int query_qp(struct ibv_qp *ibv, struct ibv_qp_attr *attr, int mask, struct ibv_qp_init_attr *init)
{
    struct roce_verbs_qp *qp = container_of(ibv, struct roce_verbs_qp, verbs.qp);
    const int available = QP_INIT_MASK | QP_RTR_MASK | QP_RTS_MASK | IBV_QP_CUR_STATE | IBV_QP_CAP;
    if (!attr || !init)
        return EINVAL;
    if (mask & ~available)
        return EOPNOTSUPP;

    pthread_mutex_lock(&ibv->send_cq->mutex);
    *attr = qp->attributes;
    *init = qp->init;
    pthread_mutex_unlock(&ibv->send_cq->mutex);
    return 0;
}

static int prepare_native_write(struct roce_native_write *entry, const struct ibv_send_wr *wr,
                                const struct roce_verbs_qp *qp, uint32_t ordinal)
{
    const struct roce_native_queue_config *config = &provider_context(qp->verbs.qp.context)->config;
    if (wr->opcode != IBV_WR_RDMA_WRITE || (wr->send_flags & ~IBV_SEND_SIGNALED) ||
        (!(wr->send_flags & IBV_SEND_SIGNALED) && !qp->init.sq_sig_all))
        return EOPNOTSUPP;
    if (wr->num_sge != 1 || !wr->sg_list)
        return EINVAL;

    const struct ibv_sge *sge = wr->sg_list;
    uint64_t offset = ((uint64_t)qp->index * config->writes + ordinal) * config->write_size;
    if (sge->addr != config->local_address + offset || wr->wr.rdma.remote_addr != config->remote_address + offset ||
        sge->length != config->write_size || sge->lkey != config->local_lkey || wr->wr.rdma.rkey != config->remote_rkey)
        return EINVAL;

    *entry = (struct roce_native_write) {
        .wr_id = wr->wr_id, .local_address = sge->addr, .remote_address = wr->wr.rdma.remote_addr,
        .length = sge->length, .lkey = sge->lkey, .rkey = wr->wr.rdma.rkey,
    };
    return 0;
}

static int post_send(struct ibv_qp *ibv, struct ibv_send_wr *wr, struct ibv_send_wr **bad_wr)
{
    struct roce_verbs_qp *qp = container_of(ibv, struct roce_verbs_qp, verbs.qp);
    struct roce_verbs_context *context = provider_context(ibv->context);
    const struct roce_native_queue_config *config = &context->config;
    struct ibv_send_wr *next = wr;
    int error = 0;

    pthread_mutex_lock(&ibv->send_cq->mutex);
    struct roce_native_sq *sq = qp_sq(qp);
    if (!sq) {
        error = EAGAIN;
        goto unlock;
    }
    if (config->flags & ROCE_NATIVE_QUEUE_RECEIVE_ONLY) {
        error = EOPNOTSUPP;
        goto unlock;
    }
    uint32_t first = atomic_load_explicit(&sq->posted, memory_order_relaxed);
    if (first & ROCE_NATIVE_SQ_CLOSED) {
        error = ESHUTDOWN;
        goto unlock;
    }
    if (qp->attributes.qp_state != IBV_QPS_RTS) {
        error = EINVAL;
        goto unlock;
    }

    uint32_t limit = qp->init.cap.max_send_wr;
    if (limit > (uint32_t)ibv->send_cq->cqe)
        limit = (uint32_t)ibv->send_cq->cqe;
    uint32_t posted = first;
    for (; next; next = next->next) {
        if (posted >= config->writes || posted - qp->polled >= limit) {
            error = ENOMEM;
            break;
        }
        uint32_t slot = posted % ROCE_NATIVE_QUEUE_DEPTH;
        error = prepare_native_write(&sq->entries[slot], next, qp, posted);
        if (error)
            break;
        sq->status[slot] = ROCE_NATIVE_WC_SUCCESS;
        ++posted;
    }
    if (posted != first) {
        if (!atomic_compare_exchange_strong_explicit(&sq->posted, &first, posted,
                                                     memory_order_release, memory_order_relaxed)) {
            error = ESHUTDOWN;
            next = wr;
        } else {
            uint32_t worker = ((qp->index + 1) * config->workers - 1) / config->qps;
            uint64_t item = (uint64_t)(qp->index + 1) << 32 | posted;
            roce_native_notify(&context->work->doorbells[worker], item);
        }
    }
unlock:
    pthread_mutex_unlock(&ibv->send_cq->mutex);
    if (bad_wr)
        *bad_wr = next;
    return error;
}

static enum ibv_wc_status completion_status(uint8_t status)
{
    switch (status) {
    case ROCE_NATIVE_WC_SUCCESS:
        return IBV_WC_SUCCESS;
    case ROCE_NATIVE_WC_LOCAL_LENGTH:
        return IBV_WC_LOC_LEN_ERR;
    case ROCE_NATIVE_WC_LOCAL_PROTECTION:
        return IBV_WC_LOC_PROT_ERR;
    case ROCE_NATIVE_WC_REMOTE_ACCESS:
        return IBV_WC_REM_ACCESS_ERR;
    case ROCE_NATIVE_WC_REMOTE_INVALID:
        return IBV_WC_REM_INV_REQ_ERR;
    case ROCE_NATIVE_WC_REMOTE_OPERATION:
        return IBV_WC_REM_OP_ERR;
    case ROCE_NATIVE_WC_RETRY_EXCEEDED:
        return IBV_WC_RETRY_EXC_ERR;
    case ROCE_NATIVE_WC_FLUSHED:
        return IBV_WC_WR_FLUSH_ERR;
    default:
        return IBV_WC_GENERAL_ERR;
    }
}

static int poll_cq(struct ibv_cq *ibv, int count, struct ibv_wc *wc)
{
    if (count < 0 || (count && !wc))
        return -EINVAL;
    if (!count)
        return 0;
    struct roce_verbs_cq *cq = container_of(ibv, struct roce_verbs_cq, ibv);

    pthread_mutex_lock(&ibv->mutex);
    struct roce_verbs_qp *qp = cq->sender;
    const struct roce_native_sq *sq = qp ? qp_sq(qp) : NULL;
    int done = 0;
    if (sq) {
        uint32_t retired = atomic_load_explicit(&sq->retired, memory_order_acquire);
        uint32_t posted = roce_native_posted_count(atomic_load_explicit(&sq->posted, memory_order_relaxed));
        if (retired < qp->polled || retired > posted) {
            done = -EIO;
        } else {
            while (done < count && qp->polled < retired) {
                size_t index = qp->polled++ % ROCE_NATIVE_QUEUE_DEPTH;
                enum ibv_wc_status status = completion_status(sq->status[index]);
                wc[done++] = (struct ibv_wc) {
                    .wr_id = sq->entries[index].wr_id, .status = status, .opcode = IBV_WC_RDMA_WRITE,
                    .qp_num = qp->verbs.qp.qp_num, .byte_len = status == IBV_WC_SUCCESS ? sq->entries[index].length : 0,
                };
            }
        }
    }
    pthread_mutex_unlock(&ibv->mutex);
    return done;
}

static void free_context(struct ibv_context *ibv)
{
    struct roce_verbs_context *context = provider_context(ibv);
    if (context->work)
        munmap(context->work, sizeof(*context->work));
    verbs_uninit_context(&context->verbs);
    free(context->mr_socket);
    free(context);
}

static const struct verbs_context_ops context_ops = {
    .query_device_ex = query_device,
    .query_port = query_port,
    .alloc_pd = alloc_pd,
    .dealloc_pd = dealloc_pd,
    .reg_mr = reg_mr,
    .dereg_mr = dereg_mr,
    .create_cq = create_cq,
    .destroy_cq = destroy_cq,
    .poll_cq = poll_cq,
    .create_qp = create_qp,
    .modify_qp = modify_qp,
    .query_qp = query_qp,
    .destroy_qp = destroy_qp,
    .post_send = post_send,
    .free_context = free_context,
};

static struct verbs_context *alloc_context(struct ibv_device *device, int command_fd,
                                           void *private_data)
{
    (void)private_data;
    struct roce_verbs_context *context =
        verbs_init_and_alloc_context(device, command_fd, context, verbs, RDMA_DRIVER_UNKNOWN);
    if (!context)
        return NULL;

    struct ibv_get_context command = {0};
    struct ib_uverbs_get_context_resp response = {0};
    int error = ibv_cmd_get_context(&context->verbs, &command, sizeof(command), &response, sizeof(response));
    if (error) {
        free_context(&context->verbs.context);
        errno = error;
        return NULL;
    }
    const char *socket = secure_getenv("ROCE_DPDK_MR_SOCKET");
    const char *require_netlink = secure_getenv("ROCE_DPDK_MR_REQUIRE_NETLINK");
    context->require_netlink = require_netlink && strcmp(require_netlink, "1") == 0;
    if (socket && *socket) {
        context->mr_socket = strdup(socket);
        if (!context->mr_socket) {
            error = errno;
            free_context(&context->verbs.context);
            errno = error;
            return NULL;
        }
    }
    verbs_set_ops(&context->verbs, &context_ops);
    return &context->verbs;
}

static bool match_device(struct verbs_sysfs_dev *device)
{
    char description[64];
    return device->match &&
           ibv_read_sysfs_file(device->ibdev_path, "node_desc", description, sizeof(description)) > 0 &&
           strcmp(description, "RoCEv2 DPDK") == 0;
}

static struct verbs_device *alloc_device(struct verbs_sysfs_dev *device)
{
    (void)device;
    return calloc(1, sizeof(struct verbs_device));
}

static void free_device(struct verbs_device *device)
{
    free(device);
}

static const struct verbs_match_ent match[] = {
    VERBS_NAME_MATCH("roce_dpdk", NULL),
    {},
};

static const struct verbs_device_ops device_ops = {
    .name = "roce_dpdk",
    .match_min_abi_version = 1,
    .match_max_abi_version = 1,
    .match_table = match,
    .match_device = match_device,
    .alloc_context = alloc_context,
    .alloc_device = alloc_device,
    .uninit_device = free_device,
};

PROVIDER_DRIVER(roce_dpdk, device_ops);
