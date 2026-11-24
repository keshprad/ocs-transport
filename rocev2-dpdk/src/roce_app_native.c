#define _GNU_SOURCE
#include "roce_app_native.h"
#include "roce_native_queue.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

static enum ibv_mtu verbs_mtu(uint32_t pmtu)
{
    switch (pmtu) {
    case 256: return IBV_MTU_256;
    case 512: return IBV_MTU_512;
    case 1024: return IBV_MTU_1024;
    case 2048: return IBV_MTU_2048;
    case 4096: return IBV_MTU_4096;
    }
    return 0;
}

static bool valid_config(const struct roce_native_queue_config *config, bool receiver)
{
    if (config->version != ROCE_NATIVE_QUEUE_VERSION || !config->qps ||
        config->qps > ROCE_LAB_MAX_QPS || !config->workers || config->workers > config->qps ||
        !config->writes || config->writes > ROCE_PSN_MASK || !config->write_size ||
        config->flags != (receiver ? ROCE_NATIVE_QUEUE_RECEIVE_ONLY : 0) || !verbs_mtu(config->pmtu) ||
        config->local_qpn < 2 || config->local_qpn > ROCE_PSN_MASK - config->qps ||
        config->peer_qpn < 2 || config->peer_qpn > ROCE_PSN_MASK - config->qps ||
        config->local_psn > ROCE_PSN_MASK || config->peer_psn > ROCE_PSN_MASK ||
        !config->local_lkey || !config->local_ipv4 || !config->peer_ipv4)
        return false;
    if (receiver ? config->qps != 1 : !config->remote_rkey)
        return false;

    uint64_t bytes = (uint64_t)config->qps * config->writes * config->write_size;
    return bytes - 1 <= UINT64_MAX - config->local_address &&
           bytes - 1 <= UINT64_MAX - config->remote_address;
}

static const struct roce_native_work *map_native_work(int fd, bool receiver,
                                                     struct roce_native_queue_config *config)
{
    const int required_seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    int seals = fcntl(fd, F_GET_SEALS);
    struct stat status;
    if (seals < 0 || fstat(fd, &status))
        return MAP_FAILED;
    if ((seals & required_seals) != required_seals || !S_ISREG(status.st_mode) ||
        status.st_size != (off_t)sizeof(struct roce_native_work)) {
        errno = EPROTO;
        return MAP_FAILED;
    }

    const struct roce_native_work *work = mmap(NULL, sizeof(*work), PROT_READ, MAP_SHARED, fd, 0);
    if (work == MAP_FAILED)
        return MAP_FAILED;
    *config = work->config;
    if (valid_config(config, receiver)) {
        uint32_t i = 0;
        for (; i < config->qps; ++i) {
            const struct roce_native_sq *sq = &work->qps[i];
            if (!atomic_is_lock_free(&sq->admitted) ||
                atomic_load_explicit(&sq->posted, memory_order_acquire) ||
                atomic_load_explicit(&sq->admitted, memory_order_acquire) ||
                atomic_load_explicit(&sq->retired, memory_order_acquire))
                break;
        }
        if (i == config->qps)
            return work;
    }
    munmap((void *)work, sizeof(*work));
    errno = EPROTO;
    return MAP_FAILED;
}

static union ibv_gid peer_gid(uint32_t ipv4)
{
    union ibv_gid gid = { .raw = { [10] = 0xff, [11] = 0xff } };
    uint32_t network = htonl(ipv4);
    memcpy(gid.raw + 12, &network, sizeof(network));
    return gid;
}

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
    int error;
    if (resources->qp) {
        struct ibv_qp_attr attr = {
            .qp_state = IBV_QPS_INIT, .port_num = 1,
            .qp_access_flags = receiver ? IBV_ACCESS_REMOTE_WRITE : 0,
        };
        error = ibv_modify_qp(resources->qp, &attr,
                              IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    } else
        error = errno ? errno : EIO;
    if (error)
        roce_app_destroy_native_qp(resources);
    return error;
}

static int connect_native_qp(struct ibv_qp *qp, const struct roce_native_queue_config *config,
                             uint32_t index)
{
    if (qp->qp_num != config->local_qpn + index)
        return EPROTO;
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR, .path_mtu = verbs_mtu(config->pmtu),
        .dest_qp_num = config->peer_qpn + index,
        .rq_psn = roce_native_psn(config->peer_psn, index), .min_rnr_timer = 12,
        .ah_attr = {
            .port_num = 1, .is_global = 1,
            .grh = { .dgid = peer_gid(config->peer_ipv4), .hop_limit = 64 },
        },
    };
    int error = ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                              IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                              IBV_QP_MIN_RNR_TIMER);
    if (error)
        return error;
    attr = (struct ibv_qp_attr) {
        .qp_state = IBV_QPS_RTS, .sq_psn = roce_native_psn(config->local_psn, index),
        .timeout = 14, .retry_cnt = ROCE_LAB_RETRIES,
    };
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                         IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
}

static int bind_native_work(struct ibv_context *context, int queue_fd)
{
    void *provider = dlopen("libroce_dpdk-rdmav34.so", RTLD_NOLOAD | RTLD_NOW);
    if (!provider)
        return ENOTSUP;
    typedef int (*bind_queues_fn)(struct ibv_context *, int);
    bind_queues_fn bind_queues = (bind_queues_fn)dlsym(provider, "roce_dpdk_bind_queues");
    int error = bind_queues ? bind_queues(context, queue_fd) : ENOTSUP;
    if (dlclose(provider) && !error)
        error = EIO;
    return error;
}

static bool valid_native_qp(const struct roce_app_native_qp *resources)
{
    return resources && resources->qp && resources->cq &&
           resources->qp->send_cq == resources->cq && resources->qp->recv_cq == resources->cq;
}

int roce_app_configure_native_qp(struct roce_app_native_qp *resources, int queue_fd, bool receiver)
{
    if (!valid_native_qp(resources) || resources->qp->state != IBV_QPS_INIT)
        return EINVAL;
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init;
    int error = ibv_query_qp(resources->qp, &attr, IBV_QP_ACCESS_FLAGS, &init);
    if (error)
        return error;
    if (!!(attr.qp_access_flags & IBV_ACCESS_REMOTE_WRITE) != receiver)
        return EINVAL;
    struct roce_native_queue_config config;
    const struct roce_native_work *work = map_native_work(queue_fd, receiver, &config);
    if (work == MAP_FAILED)
        return errno;
    if (munmap((void *)work, sizeof(*work)))
        return errno;
    if (config.qps != 1 || resources->qp->qp_num != config.local_qpn)
        return EPROTO;
    error = bind_native_work(resources->qp->context, queue_fd);
    return error ? error : connect_native_qp(resources->qp, &config, 0);
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
        } else if (writes <= run->capacity && posted < writes) {
            /* Keep the admission check for a transfer that fits in one queue. */
            while (atomic_load_explicit(&state->shared->admitted, memory_order_acquire) < posted) {
                error = wait_for_worker(run);
                if (error && atomic_load_explicit(&state->shared->admitted, memory_order_acquire) < posted)
                    return error;
            }
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
    const struct roce_native_work *work = map_native_work(queue_fd, false, &run.config);
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
        error = bind_native_work(pd->context, queue_fd);
        for (uint32_t i = 0; !error && i < run.config.qps; ++i) {
            queues[i] = (struct native_qp) { .run = &run, .shared = &work->qps[i], .index = i };
            error = roce_app_create_native_qp(pd, run.capacity, false, &queues[i].resources);
            if (!error)
                error = connect_native_qp(queues[i].resources.qp, &run.config, i);
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
