#include "mrc_provider.h"
#include "mrc_qp.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct mrc_provider_write_segments {
    size_t count;
    struct mrc_provider_mr_lease leases[];
};

struct work_entry {
    struct mrc_provider_mr_lease lease;
    struct mrc_provider_write_segments *segments;
    uint8_t *inline_data;
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
    void (*on_error)(void *context, enum mrc_provider_qp_error error);
    void *error_context;
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
    object->on_error = config->on_error;
    object->error_context = config->error_context;
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

static int check_cqs_locked(struct mrc_provider_qp *qp);

int mrc_provider_qp_query(struct mrc_provider_qp *qp, struct mrc_provider_qp_attr *attr)
{
    if (!qp || !attr)
        return -EINVAL;
    pthread_mutex_lock(&qp->mutex);
    check_cqs_locked(qp);
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

static void release_write_sources(struct work_entry *work)
{
    if (work->inline_data) {
        free(work->inline_data);
        work->inline_data = NULL;
    } else if (work->segments) {
        for (size_t i = 0; i < work->segments->count; i++) {
            struct mrc_provider_mr_lease *lease = &work->segments->leases[i];
            if (lease->owner)
                mrc_provider_mr_release(lease);
        }
        free(work->segments);
        work->segments = NULL;
    } else if (work->lease.owner) {
        mrc_provider_mr_release(&work->lease);
    }
}

int mrc_provider_qp_reset(struct mrc_provider_qp *qp)
{
    if (!qp)
        return -EINVAL;
    pthread_mutex_lock(&qp->mutex);
    while (qp->send.count)
        release_write_sources(dequeue(&qp->send));
    qp->send.head = 0;
    qp->first_sequence = qp->next_sequence;
    qp->recv.head = 0;
    qp->recv.count = 0;
    qp->state = MRC_PROVIDER_QP_RESET;
    pthread_mutex_unlock(&qp->mutex);
    return 0;
}

static int flush_qp(struct mrc_provider_qp *qp, uint64_t sequence,
                    enum mrc_provider_wc_status status)
{
    qp->state = MRC_PROVIDER_QP_ERROR;
    int result = 0;
    while (qp->send.count) {
        struct work_entry *work = dequeue(&qp->send);
        release_write_sources(work);
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

static int fail_qp(struct mrc_provider_qp *qp, uint64_t sequence,
                    enum mrc_provider_wc_status status, enum mrc_provider_qp_error error)
{
    bool notify = qp->state != MRC_PROVIDER_QP_ERROR;
    int result = flush_qp(qp, sequence, status);
    if (notify && qp->on_error)
        qp->on_error(qp->error_context, error);
    return result;
}

static int check_cqs_locked(struct mrc_provider_qp *qp)
{
    if (!mrc_provider_cq_failed(qp->send.cq) && !mrc_provider_cq_failed(qp->recv.cq))
        return 0;
    if (qp->state >= MRC_PROVIDER_QP_INIT && qp->state <= MRC_PROVIDER_QP_RTS)
        fail_qp(qp, 0, MRC_PROVIDER_WC_FLUSHED, MRC_PROVIDER_QP_FATAL_ERROR);
    return -EOVERFLOW;
}

int mrc_provider_qp_check_cqs(struct mrc_provider_qp *qp)
{
    if (!qp)
        return -EINVAL;
    pthread_mutex_lock(&qp->mutex);
    int result = check_cqs_locked(qp);
    pthread_mutex_unlock(&qp->mutex);
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
        goto unlock;
    }
    if (next >= MRC_PROVIDER_QP_INIT && next <= MRC_PROVIDER_QP_RTS) {
        result = check_cqs_locked(qp);
        if (result)
            goto unlock;
    }
    if (next == MRC_PROVIDER_QP_RESET && (qp->send.count || qp->recv.count)) {
        result = -EBUSY;
    } else if (next == expected || next == MRC_PROVIDER_QP_RESET ||
               (expected < MRC_PROVIDER_QP_RTS && next == expected + 1)) {
        qp->state = next;
    } else if (next == MRC_PROVIDER_QP_ERROR && expected != MRC_PROVIDER_QP_RESET) {
        result = flush_qp(qp, 0, MRC_PROVIDER_WC_FLUSHED);
    } else {
        result = -EINVAL;
    }
unlock:
    pthread_mutex_unlock(&qp->mutex);
    return result;
}

static int reserve_send(struct mrc_provider_qp *qp, struct work_entry **work)
{
    int result = check_cqs_locked(qp);
    if (result)
        return result;
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

static int acquire_write_sources(struct mrc_provider_pd *pd, struct work_entry *work,
                                  const struct mrc_provider_sge *sges, size_t count)
{
    struct mrc_provider_mr_lease *leases = &work->lease;
    if (count > 1) {
        work->segments = calloc(1, sizeof(*work->segments) + count * sizeof(*leases));
        if (!work->segments)
            return -ENOMEM;
        work->segments->count = count;
        leases = work->segments->leases;
    }
    for (size_t i = 0; i < count; i++) {
        const struct mrc_provider_sge *sge = &sges[i];
        if (!sge->lkey && !sge->length)
            continue;
        const struct mrc_provider_mr_access access = {
            .iova = sge->iova, .length = sge->length, .key = sge->lkey,
            .operation = MRC_PROVIDER_MR_READ_LOCAL,
        };
        int result = mrc_provider_mr_acquire(pd, &access, &leases[i]);
        if (result) {
            release_write_sources(work);
            return result;
        }
    }
    return 0;
}

static int copy_inline_sources(struct work_entry *work, const struct mrc_provider_inline_sge *sges,
                                size_t count, uint32_t length)
{
    if (!length)
        return 0;
    work->inline_data = malloc(length);
    if (!work->inline_data)
        return -ENOMEM;

    uint8_t *destination = work->inline_data;
    for (size_t i = 0; i < count; i++) {
        if (!sges[i].length)
            continue;
        memcpy(destination, sges[i].data, sges[i].length);
        destination += sges[i].length;
    }
    return 0;
}

static int post_write(struct mrc_provider_qp *qp, const struct mrc_provider_write *write,
                       const struct mrc_provider_sge *sges, const struct mrc_provider_inline_sge *inline_sges,
                       size_t count, uint64_t *sequence)
{
    if (write->length > UINT64_MAX - write->remote_address)
        return -EOVERFLOW;
    if (write->solicited && !write->with_immediate)
        return -EINVAL;

    pthread_mutex_lock(&qp->mutex);
    struct work_entry *work;
    int result = reserve_send(qp, &work);
    if (!result) {
        *work = (struct work_entry){0};
        const void *data = write->local_buffer;
        if (inline_sges) {
            result = copy_inline_sources(work, inline_sges, count, write->length);
            data = work->inline_data;
        } else {
            result = acquire_write_sources(qp->pd, work, sges, count);
            if (work->lease.owner)
                data = work->lease.address;
        }
        if (!result) {
            work->write = (struct mrc_provider_write_view){
                .data = data,
                .remote_address = write->remote_address, .length = write->length,
                .rkey = write->rkey, .immediate = write->immediate,
                .with_immediate = write->with_immediate, .solicited = write->solicited,
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

int mrc_provider_qp_post_write(struct mrc_provider_qp *qp, const struct mrc_provider_write *write,
                               uint64_t *sequence)
{
    if (!qp || !write || !sequence)
        return -EINVAL;
    if (qp->pd) {
        if (write->local_buffer || (!write->lkey && (write->local_iova || write->length)))
            return -EINVAL;
    } else {
        if (write->lkey || write->local_iova || (write->length && !write->local_buffer))
            return -EINVAL;
        if (write->length > UINTPTR_MAX - (uintptr_t)write->local_buffer)
            return -EOVERFLOW;
    }

    const struct mrc_provider_sge sge = {
        .iova = write->local_iova, .length = write->length, .lkey = write->lkey,
    };
    return post_write(qp, write, &sge, NULL, write->lkey != 0, sequence);
}

int mrc_provider_qp_post_writev(struct mrc_provider_qp *qp, const struct mrc_provider_write *write,
                                const struct mrc_provider_sge *sges, size_t count, uint64_t *sequence)
{
    if (!qp || !qp->pd || !write || !sequence || (count && !sges) ||
        write->local_iova || write->local_buffer || write->lkey)
        return -EINVAL;
    if (count > (SIZE_MAX - sizeof(struct mrc_provider_write_segments)) /
                sizeof(struct mrc_provider_mr_lease))
        return -EOVERFLOW;

    uint32_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (sges[i].length && !sges[i].lkey)
            return -EINVAL;
        if (sges[i].length > UINT32_MAX - total)
            return -EOVERFLOW;
        total += sges[i].length;
    }
    if (total != write->length)
        return -EINVAL;
    return post_write(qp, write, sges, NULL, count, sequence);
}

int mrc_provider_qp_post_write_inline(struct mrc_provider_qp *qp, const struct mrc_provider_write *write,
                                      const struct mrc_provider_inline_sge *sges, size_t count, uint64_t *sequence)
{
    if (!qp || !write || !sequence || (count && !sges) ||
        write->local_iova || write->local_buffer || write->lkey)
        return -EINVAL;
    if (count > SIZE_MAX / sizeof(*sges))
        return -EOVERFLOW;

    uint32_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (sges[i].length && !sges[i].data)
            return -EINVAL;
        if (sges[i].length > UINTPTR_MAX - (uintptr_t)sges[i].data ||
            sges[i].length > UINT32_MAX - total)
            return -EOVERFLOW;
        total += sges[i].length;
    }
    if (total != write->length)
        return -EINVAL;
    return post_write(qp, write, NULL, sges, count, sequence);
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
            write->segments = work->segments;
            result = 0;
        }
    }
    pthread_mutex_unlock(&qp->mutex);
    return result;
}

const uint8_t *mrc_provider_write_slice(const struct mrc_provider_write_view *write,
                                         uint32_t offset, uint32_t length, uint8_t *scratch)
{
    if (!write || !length || offset > write->length || length > write->length - offset)
        return NULL;
    if (write->data)
        return write->data + offset;
    if (!write->segments)
        return NULL;

    size_t remaining = length;
    for (size_t i = 0; i < write->segments->count; i++) {
        const struct mrc_provider_mr_lease *lease = &write->segments->leases[i];
        if (offset >= lease->length) {
            offset -= (uint32_t)lease->length;
            continue;
        }

        const uint8_t *source = (const uint8_t *)lease->address + offset;
        size_t available = lease->length - offset;
        if (remaining == length && available >= remaining)
            return source;
        if (!scratch)
            return NULL;
        size_t copied = available < remaining ? available : remaining;
        memcpy(scratch + (length - remaining), source, copied);
        remaining -= copied;
        if (!remaining)
            return scratch;
        offset = 0;
    }
    return NULL;
}

int mrc_provider_qp_post_recv(struct mrc_provider_qp *qp, uint64_t wr_id)
{
    if (!qp)
        return -EINVAL;
    pthread_mutex_lock(&qp->mutex);
    int result = check_cqs_locked(qp);
    if (result) {
        pthread_mutex_unlock(&qp->mutex);
        return result;
    }
    result = -EINVAL;
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
            release_write_sources(work);
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
            fail_qp(qp, 0, MRC_PROVIDER_WC_FLUSHED, MRC_PROVIDER_QP_FATAL_ERROR);
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
            fail_qp(qp, 0, MRC_PROVIDER_WC_FLUSHED, MRC_PROVIDER_QP_FATAL_ERROR);
        else
            *wr_id = work->id;
    }
    pthread_mutex_unlock(&qp->mutex);
    return result;
}

static int fail_active_qp(struct mrc_provider_qp *qp, uint64_t sequence,
                          enum mrc_provider_wc_status status, enum mrc_provider_qp_error error)
{
    pthread_mutex_lock(&qp->mutex);
    int result;
    if (qp->state == MRC_PROVIDER_QP_RESET) {
        result = -EINVAL;
    } else if (sequence && (sequence < qp->first_sequence || sequence >= qp->next_sequence)) {
        result = -ENOENT;
    } else {
        result = fail_qp(qp, sequence, status, error);
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
    return fail_active_qp(qp, sequence, status, MRC_PROVIDER_QP_FATAL_ERROR);
}

int mrc_provider_qp_fail_recv(struct mrc_provider_qp *qp, enum mrc_provider_qp_error error)
{
    if (!qp || (error != MRC_PROVIDER_QP_REQUEST_ERROR && error != MRC_PROVIDER_QP_ACCESS_ERROR))
        return -EINVAL;
    return fail_active_qp(qp, 0, MRC_PROVIDER_WC_FLUSHED, error);
}
