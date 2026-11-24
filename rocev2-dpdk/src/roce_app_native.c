#define _GNU_SOURCE
#include "roce_app_native.h"
#include "roce_native_connection.h"
#include "roce_native_queue.h"

#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

enum { NATIVE_COMPLETION_BATCH = 32 };

struct native_run {
    struct roce_native_queue_config config;
    uint64_t deadline;
    uint32_t post_batch;
    uint32_t capacity;
    int socket;
    bool scatter_gather;
    _Atomic bool stopped;
};

struct native_qp {
    struct native_run *run;
    const struct roce_native_sq *shared;
    struct roce_app_native_qp resources;
    uint32_t index;
    uint32_t completed;
    int error;
};

int roce_app_destroy_native_qp(struct roce_app_native_qp *resources)
{
    if (!resources)
        return EINVAL;
    if (resources->qp) {
        int error = ibv_destroy_qp(resources->qp);
        if (error)
            return error;
        resources->qp = NULL;
    }
    if (resources->cq) {
        int error = ibv_destroy_cq(resources->cq);
        if (error)
            return error;
        resources->cq = NULL;
    }
    return 0;
}

int roce_app_create_native_qp(struct ibv_pd *pd, uint32_t depth, bool receiver,
                              struct roce_app_native_qp *resources)
{
    if (!pd || !pd->context || !resources || !depth || depth > ROCE_NATIVE_QUEUE_DEPTH)
        return EINVAL;
    if (resources->qp || resources->cq)
        return EBUSY;
    resources->cq = ibv_create_cq(pd->context, (int)depth, NULL, NULL, 0);
    if (!resources->cq)
        return errno ? errno : EIO;
    struct ibv_qp_init_attr init = {
        .send_cq = resources->cq, .recv_cq = resources->cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = depth, .max_send_sge = 1 },
    };
    resources->qp = ibv_create_qp(pd, &init);
    int error = resources->qp ? roce_native_init_qp(resources->qp, receiver) : (errno ? errno : EIO);
    if (error)
        roce_app_destroy_native_qp(resources);
    return error;
}

static bool valid_native_qp(const struct roce_app_native_qp *resources)
{
    return resources && resources->qp && resources->cq &&
           resources->qp->send_cq == resources->cq && resources->qp->recv_cq == resources->cq;
}

static uint64_t write_id(const struct native_qp *state, uint32_t ordinal)
{
    return (uint64_t)state->resources.qp->qp_num << 32 | (ordinal + 1);
}

static int post_batch(struct native_qp *state, uint32_t first, uint32_t count)
{
    const struct roce_native_queue_config *config = &state->run->config;
    struct ibv_send_wr writes[ROCE_NATIVE_QUEUE_DEPTH];
    struct ibv_sge segments[ROCE_NATIVE_QUEUE_DEPTH];
    uint64_t offset = ((uint64_t)state->index * config->writes + first) * config->write_size;
    for (uint32_t i = 0; i < count; ++i, offset += config->write_size) {
        segments[i] = (struct ibv_sge) {
            .addr = config->local_address + offset, .length = config->write_size,
            .lkey = config->local_lkey,
        };
        writes[i] = (struct ibv_send_wr) {
            .wr_id = write_id(state, first + i),
            .next = i + 1 < count ? &writes[i + 1] : NULL,
            .sg_list = &segments[i], .num_sge = 1,
            .opcode = IBV_WR_RDMA_WRITE, .send_flags = IBV_SEND_SIGNALED,
            .wr.rdma = { .remote_addr = config->remote_address + offset, .rkey = config->remote_rkey },
        };
    }
    struct ibv_send_wr *bad = NULL;
    return ibv_post_send(state->resources.qp, writes, &bad);
}

static int wait_for_worker(const struct native_run *run)
{
    if (atomic_load_explicit(&run->stopped, memory_order_acquire))
        return ECANCELED;
    if (roce_now_ns() >= run->deadline)
        return ETIMEDOUT;
    struct pollfd peer = { .fd = run->socket, .events = POLLRDHUP };
    int ready = poll(&peer, 1, 1);
    if (ready > 0)
        return ECONNRESET;
    return ready < 0 && errno != EINTR ? errno : 0;
}

static int poll_completions(struct native_qp *state, uint32_t posted, bool drain)
{
    struct ibv_wc completions[NATIVE_COMPLETION_BATCH];
    uint32_t target = drain ? posted : state->completed + 1;
    int wait_error = 0;
    while (state->completed < target) {
        int count = ibv_poll_cq(state->resources.cq, NATIVE_COMPLETION_BATCH, completions);
        if (count < 0)
            return EIO;
        if ((uint32_t)count > posted - state->completed || count > NATIVE_COMPLETION_BATCH)
            return EPROTO;
        if (!count) {
            if (wait_error)
                return wait_error;
            wait_error = wait_for_worker(state->run);
            /* The peer may have published completions before closing its socket. */
            continue;
        }
        for (int i = 0; i < count; ++i) {
            const struct ibv_wc *wc = &completions[i];
            if (wc->status != IBV_WC_SUCCESS)
                return EIO;
            if (wc->opcode != IBV_WC_RDMA_WRITE || wc->qp_num != state->resources.qp->qp_num ||
                wc->wr_id != write_id(state, state->completed))
                return EPROTO;
            ++state->completed;
        }
    }
    return 0;
}

static int produce_batches(struct native_qp *state)
{
    struct native_run *run = state->run;
    uint32_t writes = run->config.writes;
    for (uint32_t posted = 0; posted < writes;) {
        if (atomic_load_explicit(&run->stopped, memory_order_acquire))
            return ECANCELED;
        uint32_t count = writes - posted;
        if (count > run->post_batch)
            count = run->post_batch;
        int error;
        if (run->scatter_gather) {
            if (posted - state->completed == run->capacity) {
                error = poll_completions(state, posted, false);
                if (error)
                    return error;
            }
            /* Refill as credits arrive so the PMD can keep processing TX. */
            uint32_t available = run->capacity - (posted - state->completed);
            if (count > available)
                count = available;
        }
        error = post_batch(state, posted, count);
        if (error)
            return error;
        posted += count;
        if (!run->scatter_gather) {
            error = poll_completions(state, posted, true);
            if (error)
                return error;
        }
    }
    return 0;
}

static void *produce_native(void *opaque)
{
    struct native_qp *state = opaque;
    state->error = produce_batches(state);
    if (state->error)
        atomic_store_explicit(&state->run->stopped, true, memory_order_release);
    return NULL;
}

static int submit_writes(struct native_run *run, struct native_qp *queues)
{
    pthread_t threads[ROCE_LAB_MAX_QPS];
    uint32_t created = 0;
    int error = 0;
    for (; created < run->config.qps; ++created) {
        error = pthread_create(&threads[created], NULL, produce_native, &queues[created]);
        if (error) {
            atomic_store_explicit(&run->stopped, true, memory_order_release);
            break;
        }
    }
    for (uint32_t i = 0; i < created; ++i) {
        pthread_join(threads[i], NULL);
        if (!error && queues[i].error != ECANCELED)
            error = queues[i].error;
    }
    if (!error && atomic_load_explicit(&run->stopped, memory_order_acquire))
        error = ECANCELED;
    return error;
}

static int finish_writes(struct native_run *run, struct native_qp *queues)
{
    if (roce_lab_receive(run->socket, 'C', false) != 1)
        return EPROTO;
    for (uint32_t i = 0; i < run->config.qps; ++i) {
        int error = poll_completions(&queues[i], run->config.writes, true);
        if (error)
            return error;
        struct ibv_wc unexpected;
        int count = ibv_poll_cq(queues[i].resources.cq, 1, &unexpected);
        if (count)
            return count < 0 ? EIO : EPROTO;
    }
    return 0;
}

static void abort_native_qp(struct native_qp *state, uint64_t deadline)
{
    if (!state->resources.qp)
        return;
    struct ibv_qp_attr attr = { .qp_state = IBV_QPS_ERR };
    ibv_modify_qp(state->resources.qp, &attr, IBV_QP_STATE);
    uint32_t posted = roce_native_posted_count(atomic_load_explicit(&state->shared->posted, memory_order_acquire));
    while (posted && roce_now_ns() < deadline) {
        uint32_t retired = atomic_load_explicit(&state->shared->retired, memory_order_acquire);
        struct ibv_wc discarded[NATIVE_COMPLETION_BATCH];
        int completed = ibv_poll_cq(state->resources.cq, NATIVE_COMPLETION_BATCH, discarded);
        if (completed < 0 || (!completed && retired == posted))
            break;
        if (!completed)
            usleep(1000);
    }
}

static int release_queues(struct native_qp *queues, uint32_t count, bool aborting)
{
    uint64_t deadline = roce_now_ns() + UINT64_C(1000000000);
    int first_error = 0;
    while (count) {
        struct native_qp *state = &queues[--count];
        if (aborting)
            abort_native_qp(state, deadline);
        int error = roce_app_destroy_native_qp(&state->resources);
        if (!first_error && error)
            first_error = error;
    }
    return first_error;
}

bool roce_app_run_native_verbs(struct ibv_pd *pd, int queue_fd, int app_socket,
                               uint32_t post_batch, bool scatter_gather,
                               int (*submitted)(void *), void *context,
                               struct roce_app_native_qp *early_qp)
{
    if (!pd || !pd->context) {
        shutdown(app_socket, SHUT_RDWR);
        errno = EINVAL;
        return false;
    }
    struct native_run run = { .socket = app_socket, .post_batch = post_batch,
                              .scatter_gather = scatter_gather };
    atomic_init(&run.stopped, false);
    const struct roce_native_work *work = roce_native_map_work(queue_fd, false, &run.config);
    if (work == MAP_FAILED) {
        int error = errno;
        shutdown(app_socket, SHUT_RDWR);
        errno = error;
        return false;
    }

    struct native_qp queues[ROCE_LAB_MAX_QPS] = {0};
    run.capacity = roce_native_queue_capacity(run.config.writes);
    int error = 0;
    if (!post_batch || post_batch > run.capacity) {
        error = EPROTO;
    } else if (early_qp) {
        uint32_t need = scatter_gather ? run.capacity : post_batch;
        if (run.config.qps != 1 || !valid_native_qp(early_qp) || early_qp->qp->pd != pd ||
            early_qp->qp->state != IBV_QPS_RTS || early_qp->qp->qp_num != run.config.local_qpn ||
            early_qp->cq->cqe < (int)need) {
            error = EPROTO;
        } else {
            queues[0] = (struct native_qp) { .run = &run, .shared = &work->qps[0], .resources = *early_qp };
            struct ibv_qp_attr attr;
            struct ibv_qp_init_attr init;
            error = ibv_query_qp(early_qp->qp, &attr, IBV_QP_CAP, &init);
            if (!error && init.cap.max_send_wr < need)
                error = EINVAL;
        }
    } else {
        error = roce_native_bind_work(pd->context, queue_fd);
        for (uint32_t i = 0; !error && i < run.config.qps; ++i) {
            queues[i] = (struct native_qp) { .run = &run, .shared = &work->qps[i], .index = i };
            error = roce_app_create_native_qp(pd, run.capacity, false, &queues[i].resources);
            if (!error)
                error = roce_native_connect_qp(queues[i].resources.qp, &run.config, i);
        }
    }
    run.deadline = roce_now_ns() + ROCE_LAB_TIMEOUT * UINT64_C(1000000000);
    if (!error)
        error = submit_writes(&run, queues);
    if (!error && submitted)
        error = submitted(context);
    if (!error)
        error = finish_writes(&run, queues);
    if (error)
        shutdown(app_socket, SHUT_RDWR);

    int cleanup = 0;
    if (!early_qp)
        cleanup = release_queues(queues, run.config.qps, error != 0);
    else if (error)
        abort_native_qp(&queues[0], roce_now_ns() + UINT64_C(1000000000));
    if (munmap((void *)work, sizeof(*work)) && !cleanup)
        cleanup = errno;
    if (error || cleanup) {
        if (!error)
            shutdown(app_socket, SHUT_RDWR);
        errno = error ? error : cleanup;
        return false;
    }
    printf("Application observed %u WRITE completions per QP via libibverbs\n", run.config.writes);
    return true;
}
