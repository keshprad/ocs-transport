#include "mrc_provider.h"
#include "mrc_qp.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

struct work_entry {
    struct mrc_provider_mr_lease lease;
    struct mrc_provider_write_view write;
    uint64_t id;
    bool signaled;
    bool has_write;
};

struct work_queue {
    struct mrc_provider_cq *cq;
    struct work_entry *entries;
    size_t capacity;
    size_t head;
    size_t count;
};

struct mrc_provider_qp {
    pthread_mutex_t mutex;
    struct mrc_provider_pd *pd;
    struct work_queue send;
    struct work_queue recv;
    enum mrc_provider_qp_state state;
    uint64_t first_sequence;
    uint64_t next_sequence;
    uint32_t qpn;
    bool sq_sig_all;
    struct work_entry entries[];
};

int mrc_provider_qp_create(const struct mrc_provider_qp_config *config,
                           struct mrc_provider_qp **qp)
{
    if (!config || !qp || config->qpn < 3 || config->qpn > MRC_PSN_MASK ||
        (!config->send_capacity && !config->recv_capacity) ||
        (!!config->send_capacity != !!config->send_cq) ||
        (!!config->recv_capacity != !!config->recv_cq))
        return -EINVAL;
    size_t limit = (SIZE_MAX - sizeof(struct mrc_provider_qp)) / sizeof(struct work_entry);
    if (config->send_capacity > limit || config->recv_capacity > limit - config->send_capacity)
        return -EOVERFLOW;

    size_t entries = config->send_capacity + config->recv_capacity;
    struct mrc_provider_qp *object = calloc(1, sizeof(*object) + entries * sizeof(struct work_entry));
    if (!object)
        return -ENOMEM;
    int result = -pthread_mutex_init(&object->mutex, NULL);
    if (result)
        goto free_object;
    if (config->pd && (result = mrc_provider_pd_retain(config->pd)))
        goto destroy_mutex;
    if (config->send_cq && (result = mrc_provider_cq_retain(config->send_cq)))
        goto release_pd;
    if (config->recv_cq && (result = mrc_provider_cq_retain(config->recv_cq)))
        goto release_send_cq;

    object->pd = config->pd;
    object->qpn = config->qpn;
    object->sq_sig_all = config->sq_sig_all;
    object->first_sequence = 1;
    object->next_sequence = 1;
    object->send = (struct work_queue){
        .cq = config->send_cq, .entries = object->entries, .capacity = config->send_capacity,
    };
    object->recv = (struct work_queue){
        .cq = config->recv_cq, .entries = object->entries + config->send_capacity,
        .capacity = config->recv_capacity,
    };
    *qp = object;
    return 0;

release_send_cq:
    if (config->send_cq)
        mrc_provider_cq_release(config->send_cq);
release_pd:
    if (config->pd)
        mrc_provider_pd_release(config->pd);
destroy_mutex:
    pthread_mutex_destroy(&object->mutex);
free_object:
    free(object);
    return result;
}

int mrc_provider_qp_query(struct mrc_provider_qp *qp, struct mrc_provider_qp_attr *attr)
{
    if (!qp || !attr)
        return -EINVAL;
    pthread_mutex_lock(&qp->mutex);
    *attr = (struct mrc_provider_qp_attr){
        .state = qp->state, .qpn = qp->qpn, .send_pending = qp->send.count,
        .recv_pending = qp->recv.count, .send_first_sequence = qp->first_sequence,
    };
    pthread_mutex_unlock(&qp->mutex);
    return 0;
}

int mrc_provider_qp_destroy(struct mrc_provider_qp *qp)
{
    if (!qp)
        return -EINVAL;
    pthread_mutex_lock(&qp->mutex);
    bool busy = qp->send.count || qp->recv.count;
    pthread_mutex_unlock(&qp->mutex);
    if (busy)
        return -EBUSY;
    if (qp->recv.cq)
        mrc_provider_cq_release(qp->recv.cq);
    if (qp->send.cq)
        mrc_provider_cq_release(qp->send.cq);
    if (qp->pd)
        mrc_provider_pd_release(qp->pd);
    pthread_mutex_destroy(&qp->mutex);
    free(qp);
    return 0;
}

static struct work_entry *next_entry(struct work_queue *queue)
{
    if (queue->count == queue->capacity)
        return NULL;
    return &queue->entries[(queue->head + queue->count) % queue->capacity];
}

static struct work_entry *dequeue(struct work_queue *queue)
{
    struct work_entry *entry = &queue->entries[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    return entry;
}

static bool valid_state(enum mrc_provider_qp_state state)
{
    return (unsigned int)state <= MRC_PROVIDER_QP_ERROR;
}

static int flush_qp(struct mrc_provider_qp *qp, uint64_t sequence,
                    enum mrc_provider_wc_status status)
{
    qp->state = MRC_PROVIDER_QP_ERROR;
    int result = 0;
    while (qp->send.count) {
        struct work_entry *work = dequeue(&qp->send);
        if (work->lease.owner)
            mrc_provider_mr_release(&work->lease);
        const struct mrc_provider_wc wc = {
            .wr_id = work->id, .qpn = qp->qpn, .opcode = MRC_PROVIDER_WC_WRITE,
            .status = qp->first_sequence++ == sequence ? status : MRC_PROVIDER_WC_FLUSHED,
        };
        int error = mrc_provider_cq_push(qp->send.cq, &wc, false);
        if (!result)
            result = error;
    }
    while (qp->recv.count) {
        struct work_entry *work = dequeue(&qp->recv);
        const struct mrc_provider_wc wc = {
            .wr_id = work->id, .qpn = qp->qpn, .opcode = MRC_PROVIDER_WC_RECV_WRITE_IMM,
            .status = MRC_PROVIDER_WC_FLUSHED,
        };
        int error = mrc_provider_cq_push(qp->recv.cq, &wc, false);
        if (!result)
            result = error;
    }
    return result;
}

int mrc_provider_qp_transition(struct mrc_provider_qp *qp, enum mrc_provider_qp_state expected,
                               enum mrc_provider_qp_state next)
{
    if (!qp || !valid_state(expected) || !valid_state(next))
        return -EINVAL;
    pthread_mutex_lock(&qp->mutex);
    int result = 0;
    if (qp->state != expected) {
        result = -EAGAIN;
    } else if (next == MRC_PROVIDER_QP_RESET && (qp->send.count || qp->recv.count)) {
        result = -EBUSY;
    } else if (next == expected || next == MRC_PROVIDER_QP_RESET ||
               (expected < MRC_PROVIDER_QP_RTS && next == expected + 1)) {
        qp->state = next;
    } else if (next == MRC_PROVIDER_QP_ERROR && expected != MRC_PROVIDER_QP_RESET) {
        result = flush_qp(qp, 0, MRC_PROVIDER_WC_FLUSHED);
    } else {
        result = -EINVAL;
    }
    pthread_mutex_unlock(&qp->mutex);
    return result;
}

static int reserve_send(struct mrc_provider_qp *qp, struct work_entry **work)
{
    if (qp->state != MRC_PROVIDER_QP_RTS)
        return -EINVAL;
    if (qp->next_sequence == UINT64_MAX)
        return -EOVERFLOW;
    *work = next_entry(&qp->send);
    return *work ? 0 : -ENOSPC;
}

int mrc_provider_qp_post_send(struct mrc_provider_qp *qp, uint64_t wr_id, uint32_t length,
                              bool signaled, uint64_t *sequence)
{
    if (!qp || !sequence)
        return -EINVAL;
    pthread_mutex_lock(&qp->mutex);
    struct work_entry *work;
    int result = reserve_send(qp, &work);
    if (!result) {
        *work = (struct work_entry){ .id = wr_id, .write.length = length,
                                     .signaled = signaled || qp->sq_sig_all };
        qp->send.count++;
        *sequence = qp->next_sequence++;
    }
    pthread_mutex_unlock(&qp->mutex);
    return result;
}

int mrc_provider_qp_post_write(struct mrc_provider_qp *qp, const struct mrc_provider_write *write,
                               uint64_t *sequence)
{
    if (!qp || !write || !sequence)
        return -EINVAL;
    if (write->length > UINT64_MAX - write->remote_address)
        return -EOVERFLOW;
    if (qp->pd) {
        if (write->local_buffer || (!write->lkey && (write->local_iova || write->length)))
            return -EINVAL;
    } else {
        if (write->lkey || write->local_iova || (write->length && !write->local_buffer))
            return -EINVAL;
        if (write->length > UINTPTR_MAX - (uintptr_t)write->local_buffer)
            return -EOVERFLOW;
    }

    pthread_mutex_lock(&qp->mutex);
    struct work_entry *work;
    int result = reserve_send(qp, &work);
    if (!result) {
        *work = (struct work_entry){0};
        if (write->lkey) {
            const struct mrc_provider_mr_access access = {
                .iova = write->local_iova, .length = write->length, .key = write->lkey,
                .operation = MRC_PROVIDER_MR_READ_LOCAL,
            };
            result = mrc_provider_mr_acquire(qp->pd, &access, &work->lease);
        }
        if (!result) {
            work->write = (struct mrc_provider_write_view){
                .data = work->lease.owner ? work->lease.address : write->local_buffer,
                .remote_address = write->remote_address, .length = write->length,
                .rkey = write->rkey, .immediate = write->immediate,
                .with_immediate = write->with_immediate,
            };
            work->id = write->wr_id;
            work->signaled = write->signaled || qp->sq_sig_all;
            work->has_write = true;
            qp->send.count++;
            *sequence = qp->next_sequence++;
        }
    }
    pthread_mutex_unlock(&qp->mutex);
    return result;
}

int mrc_provider_qp_get_write(struct mrc_provider_qp *qp, uint64_t sequence,
                              struct mrc_provider_write_view *write)
{
    if (!qp || !write)
        return -EINVAL;
    pthread_mutex_lock(&qp->mutex);
    int result = -ENOENT;
    if (sequence >= qp->first_sequence && sequence < qp->next_sequence) {
        size_t offset = (size_t)(sequence - qp->first_sequence);
        struct work_entry *work = &qp->send.entries[(qp->send.head + offset) % qp->send.capacity];
        if (work->has_write) {
            *write = work->write;
            result = 0;
        }
    }
    pthread_mutex_unlock(&qp->mutex);
    return result;
}

int mrc_provider_qp_post_recv(struct mrc_provider_qp *qp, uint64_t wr_id)
{
    if (!qp)
        return -EINVAL;
    pthread_mutex_lock(&qp->mutex);
    int result = -EINVAL;
    if (qp->state >= MRC_PROVIDER_QP_INIT && qp->state <= MRC_PROVIDER_QP_RTS) {
        struct work_entry *work = next_entry(&qp->recv);
        if (work) {
            *work = (struct work_entry){ .id = wr_id };
            qp->recv.count++;
            result = 0;
        } else {
            result = -ENOSPC;
        }
    }
    pthread_mutex_unlock(&qp->mutex);
    return result;
}

int mrc_provider_qp_complete_send(struct mrc_provider_qp *qp, uint64_t sequence)
{
    if (!qp)
        return -EINVAL;
    pthread_mutex_lock(&qp->mutex);
    int result = 0;
    if (qp->state != MRC_PROVIDER_QP_RTS || sequence < qp->first_sequence ||
        sequence >= qp->next_sequence) {
        result = -EINVAL;
    } else {
        while (!result && qp->first_sequence <= sequence) {
            struct work_entry *work = dequeue(&qp->send);
            if (work->lease.owner)
                mrc_provider_mr_release(&work->lease);
            qp->first_sequence++;
            if (work->signaled) {
                const struct mrc_provider_wc wc = {
                    .wr_id = work->id, .qpn = qp->qpn, .opcode = MRC_PROVIDER_WC_WRITE,
                    .status = MRC_PROVIDER_WC_SUCCESS, .byte_length = work->write.length,
                };
                result = mrc_provider_cq_push(qp->send.cq, &wc, false);
            }
        }
        if (result)
            flush_qp(qp, 0, MRC_PROVIDER_WC_FLUSHED);
    }
    pthread_mutex_unlock(&qp->mutex);
    return result;
}

int mrc_provider_qp_complete_recv(struct mrc_provider_qp *qp, uint32_t length,
                                  uint32_t immediate, bool solicited, uint64_t *wr_id)
{
    if (!qp || !wr_id)
        return -EINVAL;
    pthread_mutex_lock(&qp->mutex);
    int result;
    if (qp->state != MRC_PROVIDER_QP_RTR && qp->state != MRC_PROVIDER_QP_RTS) {
        result = -EINVAL;
    } else if (!qp->recv.count) {
        result = -ENOBUFS;
    } else {
        struct work_entry *work = dequeue(&qp->recv);
        const struct mrc_provider_wc wc = {
            .wr_id = work->id, .qpn = qp->qpn, .opcode = MRC_PROVIDER_WC_RECV_WRITE_IMM,
            .status = MRC_PROVIDER_WC_SUCCESS, .byte_length = length, .with_immediate = true,
            .immediate = immediate,
        };
        result = mrc_provider_cq_push(qp->recv.cq, &wc, solicited);
        if (result)
            flush_qp(qp, 0, MRC_PROVIDER_WC_FLUSHED);
        else
            *wr_id = work->id;
    }
    pthread_mutex_unlock(&qp->mutex);
    return result;
}

int mrc_provider_qp_fail(struct mrc_provider_qp *qp, uint64_t sequence,
                         enum mrc_provider_wc_status status)
{
    if (!qp || (sequence ? (status <= MRC_PROVIDER_WC_SUCCESS || status >= MRC_PROVIDER_WC_FLUSHED)
                         : status != MRC_PROVIDER_WC_FLUSHED))
        return -EINVAL;
    pthread_mutex_lock(&qp->mutex);
    int result;
    if (qp->state == MRC_PROVIDER_QP_RESET) {
        result = -EINVAL;
    } else if (sequence && (sequence < qp->first_sequence || sequence >= qp->next_sequence)) {
        result = -ENOENT;
    } else {
        result = flush_qp(qp, sequence, status);
    }
    pthread_mutex_unlock(&qp->mutex);
    return result;
}
