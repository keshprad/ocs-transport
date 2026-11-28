#define _GNU_SOURCE
#include "roce_app.h"
#include "roce_app_native.h"
#include "roce_app_rdmacm.h"
#include "roce_doorbell.h"
#include "roce_fd.h"
#include "roce_lab.h"
#include "roce_mr_ipc.h"
#include "roce_native_connection.h"
#include "../kernel/roce_mr_uapi.h"

#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <linux/magic.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

enum {
    APP_VERSION = 4,
    APP_MEMFD = 1,
    APP_PINNED = 2,
    APP_DOORBELL = 3,
    APP_USE_CM = 1u << 0,
    APP_USE_VERBS_MR = 1u << 1,
    APP_WRITE_DMA_ALIAS = 1u << 2,
    APP_SCATTER_GATHER = 1u << 3,
    APP_NATIVE_QUEUES = 1u << 4,
    APP_RDMACM = 1u << 5,
    APP_TWO_SGE_WRITES = 1u << 6,
    APP_MULTI_MR_WRITES = 1u << 7,
    APP_MULTI_MR_TARGETS = 1u << 8,
    APP_CONNECT = 'A',
    APP_LISTEN = 'L',
    APP_ESTABLISHED = 'E',
    APP_DEREGISTERED = 'F',
    APP_SEALS = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL,
};

struct app_workload {
    uint64_t bytes;
    uint32_t version;
    uint32_t sender;
    uint32_t qps;
    uint32_t workers;
    uint32_t writes;
    uint32_t flags;
    uint32_t post_batch;
    uint32_t reserved;
};

static size_t mapping_length(uint64_t bytes)
{
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || !bytes || bytes > SIZE_MAX - (size_t)page_size)
        return 0;
    size_t mask = (size_t)page_size - 1;
    return ((size_t)bytes + mask) & ~mask;
}

static int open_socket(const char *path, bool listener)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    size_t length = strlen(path);
    if (!length || length >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(address.sun_path, path, length + 1);
    socklen_t address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    if (!listener) {
        if (connect(fd, (struct sockaddr *)&address, address_length) == 0)
            return fd;
    } else if (bind(fd, (struct sockaddr *)&address, address_length) == 0) {
        if (chmod(path, S_IRUSR | S_IWUSR) == 0 && listen(fd, 1) == 0)
            return fd;
        int error = errno;
        unlink(path);
        errno = error;
    }
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}

static bool configure_peer(int fd, pid_t *pid)
{
    struct ucred credentials;
    socklen_t length = sizeof(credentials);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0)
        return false;
    if (length != sizeof(credentials) || credentials.uid != geteuid()) {
        errno = EACCES;
        return false;
    }
    if (pid)
        *pid = credentials.pid;
    const struct timeval timeout = { .tv_sec = ROCE_LAB_TIMEOUT };
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
}

static int accept_client(const char *path)
{
    int listener = open_socket(path, true);
    if (listener < 0)
        return -1;
    puts("application listener ready");
    fflush(stdout);
    int fd = -1;
    if (roce_lab_wait(listener, POLLIN))
        fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    int error = errno;
    close(listener);
    unlink(path);
    errno = error;
    return fd;
}

static bool send_mapping(int socket, int fd, uint8_t kind)
{
    return roce_send_fd(socket, &kind, sizeof(kind), fd);
}

static int receive_mapping(int socket, uint8_t *kind)
{
    return roce_receive_fd(socket, kind, sizeof(*kind));
}

static bool send_app_message(int socket, const void *message, size_t length)
{
    ssize_t sent;
    do {
        sent = send(socket, message, length, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent >= 0 && sent != (ssize_t)length)
        errno = EPROTO;
    return sent == (ssize_t)length;
}

static bool receive_app_message(int socket, void *message, size_t length)
{
    int fd;
    if (!roce_receive_optional_fd(socket, message, length, &fd))
        return false;
    if (fd < 0)
        return true;
    close(fd);
    errno = EPROTO;
    return false;
}

static bool valid_mapping(int fd, uint8_t kind, size_t length, bool sender)
{
    if (kind == APP_PINNED) {
        struct statfs filesystem;
        struct roce_mr_info info = {0};
        uint32_t access = sender ? 0 : ROCE_MR_ACCESS_LOCAL_WRITE;
        return fstatfs(fd, &filesystem) == 0 && filesystem.f_type == ANON_INODE_FS_MAGIC &&
               ioctl(fd, ROCE_MR_GET_INFO, &info) == 0 && info.abi_version == ROCE_MR_ABI_VERSION &&
               info.length == length && info.access == access;
    }
    if (kind == APP_MEMFD) {
        struct stat status;
        int seals = fcntl(fd, F_GET_SEALS);
        return seals >= 0 && (seals & APP_SEALS) == APP_SEALS && fstat(fd, &status) == 0 &&
               S_ISREG(status.st_mode) && status.st_size >= 0 && (uint64_t)status.st_size == length;
    }
    return false;
}

static int create_shared_file(const char *name, size_t length)
{
    int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)length) == 0 && fcntl(fd, F_ADD_SEALS, APP_SEALS) >= 0)
        return fd;
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}

static struct roce_app_work *offer_work(int socket, const struct roce_lab_options *options)
{
    int fd = create_shared_file("roce-work", sizeof(struct roce_app_work));
    if (fd < 0)
        return NULL;
    struct roce_app_work *work = mmap(NULL, sizeof(*work), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    bool valid = work != MAP_FAILED;
    for (uint32_t i = 0; valid && i < options->workers; ++i)
        valid = roce_doorbell_init(&work->doorbells[i]);
    for (uint32_t i = 0; valid && i < options->qps; ++i) {
        atomic_init(&work->qps[i].admitted, 0);
        atomic_init(&work->qps[i].completed, 0);
        valid = atomic_is_lock_free(&work->qps[i].admitted) &&
                atomic_is_lock_free(&work->qps[i].completed);
    }
    if (work != MAP_FAILED && !valid)
        errno = ENOTSUP;
    bool sent = valid && send_mapping(socket, fd, APP_DOORBELL);
    int error = errno;
    close(fd);
    if (sent)
        return work;
    if (work != MAP_FAILED)
        munmap(work, sizeof(*work));
    errno = error;
    return NULL;
}

static bool receive_local_memory(struct roce_app_memory *memory, size_t length,
                                 bool sender, bool dma_writable)
{
    uint8_t kind;
    int fd = receive_mapping(memory->socket, &kind);
    if (fd < 0)
        return false;
    /* mlx5 verbs registers external memory with local write access. */
    if ((dma_writable && kind != APP_MEMFD) || !valid_mapping(fd, kind, length, sender)) {
        close(fd);
        errno = dma_writable && kind != APP_MEMFD ? EOPNOTSUPP : EINVAL;
        return false;
    }
    int prot = PROT_READ | (!sender || dma_writable ? PROT_WRITE : 0);
    void *buffer = mmap(NULL, length, prot, MAP_SHARED, fd, 0);
    int error = errno;
    close(fd);
    if (buffer == MAP_FAILED) {
        errno = error;
        return false;
    }
    memory->buffer = buffer;
    memory->length = length;
    return true;
}

static bool receive_verbs_memory(struct roce_app_memory *memory, struct roce_mr_server *registrations,
                                 pid_t pid, size_t bytes, bool sender, bool dma_writable)
{
    uint32_t lkey;
    if (!receive_app_message(memory->socket, &lkey, sizeof(lkey)))
        return false;
    uint32_t access = sender ? 0 : ROCE_MR_IPC_WRITE | ROCE_MR_IPC_REMOTE_WRITE;
    if (dma_writable)
        access |= ROCE_MR_IPC_WRITE;
    int error = roce_mr_acquire(registrations, pid, lkey, access, &memory->mr);
    if (error) {
        errno = error;
        return false;
    }
    if (memory->mr.length != bytes) {
        errno = EINVAL;
        return false;
    }
    memory->buffer = memory->mr.buffer;
    memory->length = memory->mr.length;
    return true;
}

static bool receive_auxiliary(struct roce_app_memory *memory, const struct roce_lab_options *options)
{
    uint32_t lkey;
    if (!memory->mr.scope) {
        errno = EACCES;
        return false;
    }
    if (!receive_app_message(memory->socket, &lkey, sizeof(lkey)))
        return false;
    struct roce_mr_lease extra = {0};
    int error = roce_mr_acquire_related(&memory->mr, lkey, ROCE_MR_IPC_REMOTE_WRITE, &extra);
    if (error) {
        errno = error;
        return false;
    }
    size_t expected = (size_t)roce_lab_auxiliary_bytes(options->qps, options->writes, options->size);
    bool valid = extra.rkey != memory->mr.rkey && extra.length == expected;
    if (valid)
        memory->auxiliary = (struct roce_region) { .address = extra.iova, .rkey = extra.rkey, .size = extra.length };
    roce_mr_release(&extra);
    if (!valid)
        errno = EINVAL;
    return valid;
}

bool roce_app_attach(const char *path, const struct roce_lab_options *options,
                     const struct roce_app_config *config, struct roce_app_memory *memory)
{
    uint64_t bytes = (uint64_t)options->size * options->writes * options->qps;
    size_t length = mapping_length(bytes);
    if (!length) {
        errno = EINVAL;
        return false;
    }
    int socket = accept_client(path);
    if (socket < 0)
        return false;
    struct roce_app_memory attached = {
        .socket = socket, .multi_mr_writes = config->multi_mr_writes,
        .native_version = config->two_sge_writes ? ROCE_NATIVE_SGE_QUEUE_VERSION :
                                           ROCE_NATIVE_QUEUE_VERSION,
    };
    struct roce_mr_server *registrations = config->registrations;
    pid_t pid;
    const struct app_workload workload = {
        .bytes = bytes, .version = APP_VERSION, .sender = options->sender,
        .qps = options->qps, .workers = options->workers, .writes = options->writes,
        .flags = (config->in_band_cm ? APP_USE_CM : 0) | (registrations ? APP_USE_VERBS_MR : 0) |
                 (registrations && config->dma_writable ? APP_WRITE_DMA_ALIAS : 0) |
                 (config->scatter_gather ? APP_SCATTER_GATHER : 0) |
                 (config->native_queues ? APP_NATIVE_QUEUES : 0) | (config->rdmacm ? APP_RDMACM : 0) |
                 (config->two_sge_writes ? APP_TWO_SGE_WRITES : 0) |
                 (config->multi_mr_writes ? APP_MULTI_MR_WRITES : 0) |
                 (options->multi_mr_targets ? APP_MULTI_MR_TARGETS : 0),
        .post_batch = config->post_batch,
    };
    if (!configure_peer(socket, &pid) || !send_app_message(socket, &workload, sizeof(workload)))
        goto disconnect;
    if (config->in_band_cm && !config->rdmacm) {
        char command;
        if (!receive_app_message(socket, &command, sizeof(command)))
            goto disconnect;
        if (command != (options->sender ? APP_CONNECT : APP_LISTEN)) {
            errno = EPROTO;
            goto disconnect;
        }
    }

    bool received = registrations ?
        receive_verbs_memory(&attached, registrations, pid, (size_t)bytes, options->sender, config->dma_writable) :
        receive_local_memory(&attached, length, options->sender, config->dma_writable);
    if (!received)
        goto disconnect;
    if (config->multi_mr_writes && !attached.mr.scope) {
        errno = EACCES;
        goto disconnect;
    }
    if (options->multi_mr_targets && !options->sender && !receive_auxiliary(&attached, options))
        goto disconnect;
    if (config->rdmacm) {
        if (!receive_app_message(socket, &attached.qpn, sizeof(attached.qpn)))
            goto disconnect;
        if (attached.qpn == 1 || attached.qpn >= ROCE_PSN_MASK) {
            errno = EPROTO;
            goto disconnect;
        }
    }
    if (options->sender && !config->in_band_cm && !config->native_queues) {
        attached.work = offer_work(socket, options);
        if (!attached.work)
            goto disconnect;
    }
    *memory = attached;
    return true;

disconnect:
    {
        int error = errno;
        roce_app_detach(&attached);
        errno = error;
        return false;
    }
}

bool roce_app_connected(struct roce_app_memory *memory, const struct roce_lab_options *options)
{
    if (!roce_lab_send(memory->socket, APP_ESTABLISHED))
        return false;
    if (!options->sender || memory->native)
        return true;
    memory->work = offer_work(memory->socket, options);
    return memory->work != NULL;
}

bool roce_app_send_auxiliary(const struct roce_app_memory *memory, const struct roce_lab_options *options,
                             const struct roce_lab_offer *peer)
{
    uint8_t data[ROCE_LAB_AUXILIARY_SIZE];
    if (!roce_lab_encode_auxiliary(data, peer, options)) {
        errno = EPROTO;
        return false;
    }
    return send_app_message(memory->socket, data, sizeof(data));
}

bool roce_app_offer_native(struct roce_app_memory *memory, const struct roce_lab_options *options,
                           const struct roce_lab_offer *local, const struct roce_lab_offer *peer,
                           bool announce_established, int *queue_fd)
{
    if (queue_fd)
        *queue_fd = -1;
    const struct roce_native_queue_config config = {
        .version = memory->native_version,
        .qps = options->qps, .workers = options->workers, .writes = options->writes,
        .write_size = options->size,
        .local_qpn = local->qpn, .peer_qpn = peer->qpn,
        .local_psn = local->psn, .peer_psn = peer->psn,
        .pmtu = local->pmtu < peer->pmtu ? local->pmtu : peer->pmtu,
        .local_lkey = memory->mr.lkey, .remote_rkey = peer->region.rkey,
        .local_ipv4 = local->data.ipv4, .peer_ipv4 = peer->data.ipv4,
        .flags = options->sender ? ((memory->multi_mr_writes ? ROCE_NATIVE_QUEUE_MULTI_MR : 0) |
                                    (options->multi_mr_targets ? ROCE_NATIVE_QUEUE_REMOTE_MRS : 0)) :
                 ROCE_NATIVE_QUEUE_RECEIVE_ONLY,
        .local_address = local->region.address, .remote_address = peer->region.address,
    };
    size_t length = roce_native_work_size(config.version);
    int fd = create_shared_file("roce-native-queues", length);
    if (fd < 0)
        return false;
    struct roce_native_work *work = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    bool valid = work != MAP_FAILED;
    if (valid)
        work->config = config;
    for (uint32_t i = 0; valid && i < config.workers; ++i)
        valid = roce_doorbell_init(&work->doorbells[i]);
    for (uint32_t i = 0; valid && i < config.qps; ++i) {
        struct roce_native_sq *sq = &work->qps[i];
        atomic_init(&sq->posted, 0);
        atomic_init(&sq->admitted, 0);
        atomic_init(&sq->acknowledged, 0);
        atomic_init(&sq->retired, 0);
        valid = atomic_is_lock_free(&sq->posted) && atomic_is_lock_free(&sq->retired);
    }
    if (work != MAP_FAILED && !valid)
        errno = ENOTSUP;
    bool sent = valid && (!announce_established || roce_lab_send(memory->socket, APP_ESTABLISHED)) &&
                ((queue_fd && !options->sender) || send_mapping(memory->socket, fd, ROCE_NATIVE_QUEUE_FD));
    int error = errno;
    if (sent && queue_fd)
        *queue_fd = fd;
    else
        close(fd);
    if (sent) {
        memory->native = work;
        return true;
    }
    if (work != MAP_FAILED)
        munmap(work, length);
    errno = error;
    return false;
}

static bool finish_native(struct roce_native_work *work, const struct roce_lab_options *options,
                          bool success)
{
    bool valid = true;
    for (uint32_t i = 0; i < options->qps; ++i) {
        struct roce_native_sq *sq = &work->qps[i];
        uint32_t head = atomic_fetch_or_explicit(&sq->posted, ROCE_NATIVE_SQ_CLOSED, memory_order_acq_rel);
        uint32_t posted = roce_native_posted_count(head);
        uint32_t acknowledged = atomic_load_explicit(&sq->acknowledged, memory_order_relaxed);
        uint32_t retired = atomic_load_explicit(&sq->retired, memory_order_relaxed);
        if (posted > options->writes || posted - retired > ROCE_NATIVE_QUEUE_DEPTH ||
            (success && posted != options->writes)) {
            valid = false;
            continue;
        }
        for (uint32_t seq = retired; seq < posted; ++seq) {
            uint8_t *status = &sq->status[seq % ROCE_NATIVE_QUEUE_DEPTH];
            if (success || seq < acknowledged)
                *status = ROCE_NATIVE_WC_SUCCESS;
            else if (*status == ROCE_NATIVE_WC_SUCCESS)
                *status = ROCE_NATIVE_WC_FLUSHED;
        }
        atomic_store_explicit(&sq->retired, posted, memory_order_release);
    }
    return valid;
}

void roce_app_abort(struct roce_app_memory *memory, const struct roce_lab_options *options)
{
    if (memory->socket < 0)
        return;
    struct roce_mr_server *registrations = memory->mr.server;
    uint32_t lkey = memory->mr.lkey;
    if (registrations) {
        roce_mr_release(&memory->mr);
        memory->buffer = NULL;
    }
    if (options->sender && memory->native)
        finish_native(memory->native, options, false);
    shutdown(memory->socket, SHUT_RDWR);
    if (registrations) {
        int error = roce_mr_wait_deregistered(registrations, lkey, ROCE_LAB_TIMEOUT);
        if (error)
            fprintf(stderr, "application MR deregistration after failed WRITE: %s\n", strerror(error));
    }
}

bool roce_app_complete(struct roce_app_memory *memory, const struct roce_lab_options *options)
{
    if (options->sender && memory->native && !finish_native(memory->native, options, true)) {
        errno = EPROTO;
        return false;
    }
    /* The worker has verified every WRITE and released its PMD references. */
    if (memory->work)
        for (uint32_t i = 0; i < options->qps; ++i)
            atomic_store_explicit(&memory->work->qps[i].completed, options->writes, memory_order_release);
    bool verbs = memory->mr.server != NULL;
    if (verbs) {
        roce_mr_release(&memory->mr);
        memory->buffer = NULL;
    }
    if (!roce_lab_send(memory->socket, 'C'))
        return false;
    if (verbs && roce_lab_receive(memory->socket, APP_DEREGISTERED, false) != 1) {
        errno = EPROTO;
        return false;
    }
    return true;
}

void roce_app_detach(struct roce_app_memory *memory)
{
    if (memory->socket >= 0) {
        if (memory->work)
            munmap(memory->work, sizeof(*memory->work));
        if (memory->native)
            munmap(memory->native, roce_native_work_size(memory->native_version));
        if (memory->mr.server)
            roce_mr_release(&memory->mr);
        else if (memory->buffer)
            munmap(memory->buffer, memory->length);
        close(memory->socket);
        memory->socket = -1;
    }
}

static int pin_mapping(const char *device, void *buffer, size_t length, bool sender)
{
    int registration = open(device, O_RDWR | O_CLOEXEC);
    if (registration < 0)
        return -1;
    struct roce_mr_register request = {
        .address = (uintptr_t)buffer, .length = length,
        .access = sender ? 0 : ROCE_MR_ACCESS_LOCAL_WRITE,
    };
    int fd = ioctl(registration, ROCE_MR_REGISTER, &request);
    int error = errno;
    close(registration);
    errno = error;
    return fd;
}

struct producer {
    struct roce_doorbell *queue;
    const _Atomic uint32_t *completed;
    uint32_t qp;
    uint32_t writes;
    uint32_t post_batch;
    int socket;
    bool posted;
};

static bool wait_to_post(int socket, uint64_t deadline)
{
    if (roce_now_ns() >= deadline)
        return false;
    struct pollfd peer = { .fd = socket, .events = POLLRDHUP };
    int ready = poll(&peer, 1, 1);
    return ready == 0 || (ready < 0 && errno == EINTR);
}

static void *publish_work(void *opaque)
{
    struct producer *producer = opaque;
    uint64_t deadline = roce_now_ns() + ROCE_LAB_TIMEOUT * UINT64_C(1000000000);
    for (uint32_t posted = 0; posted < producer->writes;) {
        uint32_t count = producer->writes - posted;
        if (count > producer->post_batch)
            count = producer->post_batch;
        uint64_t item = (uint64_t)producer->qp << 32 | count;
        while (!roce_doorbell_publish(producer->queue, item))
            if (!wait_to_post(producer->socket, deadline))
                return NULL;
        posted += count;
        while (producer->completed && posted < producer->writes &&
               atomic_load_explicit(producer->completed, memory_order_acquire) < posted)
            if (!wait_to_post(producer->socket, deadline))
                return NULL;
    }
    producer->posted = true;
    return NULL;
}

static bool post_writes(int socket, const struct app_workload *workload)
{
    uint8_t kind;
    int fd = receive_mapping(socket, &kind);
    if (fd < 0)
        return false;
    struct roce_app_work *work = MAP_FAILED;
    if (kind == APP_DOORBELL && valid_mapping(fd, APP_MEMFD, sizeof(*work), false))
        work = mmap(NULL, sizeof(*work), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (work == MAP_FAILED)
        return false;

    struct producer producers[ROCE_LAB_MAX_QPS];
    pthread_t threads[ROCE_LAB_MAX_QPS];
    uint32_t created = 0;
    for (; created < workload->qps; ++created) {
        uint32_t qp = created + 1;
        uint32_t worker = (qp * workload->workers - 1) / workload->qps;
        producers[created] = (struct producer) {
            .queue = &work->doorbells[worker], .qp = qp, .writes = workload->writes,
            .post_batch = workload->post_batch, .socket = socket,
            .completed = workload->flags & APP_SCATTER_GATHER ? NULL : &work->qps[created].completed,
        };
        if (pthread_create(&threads[created], NULL, publish_work, &producers[created]) != 0)
            break;
    }
    bool posted = created == workload->qps;
    for (uint32_t i = 0; i < created; ++i) {
        pthread_join(threads[i], NULL);
        posted = posted && producers[i].posted;
    }
    bool completed = posted && roce_lab_receive(socket, 'C', false) == 1;
    for (uint32_t i = 0; completed && i < workload->qps; ++i)
        completed = atomic_load_explicit(&work->qps[i].completed, memory_order_acquire) == workload->writes;
    munmap(work, sizeof(*work));
    if (completed)
        printf("Application observed %u WRITE completions per QP\n", workload->writes);
    return completed;
}

struct app_verbs {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_mr *second_mr;
};

static bool register_verbs_memory(struct app_verbs *verbs, uint8_t *buffer, size_t length,
                                  bool sender, bool dma_writable)
{
    verbs->context = roce_native_open_device();
    if (!verbs->context)
        return false;
    verbs->pd = ibv_alloc_pd(verbs->context);
    if (!verbs->pd)
        return false;
    int access = sender ? 0 : IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    if (dma_writable)
        access |= IBV_ACCESS_LOCAL_WRITE;
    verbs->mr = ibv_reg_mr(verbs->pd, buffer, length, access);
    return verbs->mr != NULL;
}

static int release_verbs_memory(const struct app_verbs *verbs)
{
    int error = verbs->second_mr ? ibv_dereg_mr(verbs->second_mr) : 0;
    if (!error && verbs->mr)
        error = ibv_dereg_mr(verbs->mr);
    if (!error && verbs->pd)
        error = ibv_dealloc_pd(verbs->pd);
    if (!error && verbs->context)
        error = ibv_close_device(verbs->context);
    return error;
}

struct app_cm_queue {
    struct roce_app_native_qp resources;
    int socket;
    int fd;
    bool receiver;
};

static int receive_cm_queue(struct app_cm_queue *queue)
{
    uint8_t kind;
    queue->fd = receive_mapping(queue->socket, &kind);
    if (queue->fd < 0)
        return errno;
    return kind == ROCE_NATIVE_QUEUE_FD ? 0 : EPROTO;
}

static int prepare_cm_queue(void *opaque)
{
    struct app_cm_queue *queue = opaque;
    int error = receive_cm_queue(queue);
    return error ? error : roce_native_configure_qp(queue->resources.qp, queue->fd, queue->receiver);
}

static bool post_native_writes(const struct app_verbs *verbs, int socket, const struct app_workload *workload,
                                struct roce_app_rdmacm *cma, struct app_cm_queue *queue)
{
    uint8_t kind = ROCE_NATIVE_QUEUE_FD;
    int fd = queue ? queue->fd : receive_mapping(socket, &kind);
    if (fd < 0)
        return false;
    uint8_t target[ROCE_LAB_AUXILIARY_SIZE];
    bool multiple_targets = workload->flags & APP_MULTI_MR_TARGETS;
    bool success = kind == ROCE_NATIVE_QUEUE_FD &&
        (!multiple_targets || receive_app_message(socket, target, sizeof(target))) &&
        roce_app_run_native_verbs(verbs->pd, verbs->second_mr, multiple_targets ? target : NULL,
                                  fd, socket, workload->post_batch,
                                  (workload->flags & APP_SCATTER_GATHER) != 0,
                                  cma ? roce_app_rdmacm_disconnect : NULL, cma,
                                  queue ? &queue->resources : NULL);
    int error = errno;
    if (!queue)
        close(fd);
    errno = error;
    return success;
}

struct app_usage {
    uint64_t wall_ns;
    uint64_t cpu_ns;
};

static struct app_usage app_usage(void)
{
    struct timespec cpu;
    struct app_usage usage = { .wall_ns = roce_now_ns() };
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) == 0)
        usage.cpu_ns = (uint64_t)cpu.tv_sec * UINT64_C(1000000000) + (uint64_t)cpu.tv_nsec;
    return usage;
}

static bool verify_targets(const struct app_workload *workload, const uint8_t *primary, const uint8_t *auxiliary,
                           uint32_t size)
{
    if (!auxiliary)
        return roce_lab_verify(primary, (size_t)workload->bytes);
    size_t logical = 0;
    for (uint32_t qp = 0; qp < workload->qps; ++qp) {
        for (uint32_t write = 0; write < workload->writes; ++write, logical += size) {
            bool alternate = write & 1;
            if (!roce_lab_verify_range(primary + logical, size, logical, !alternate))
                return false;
            if (alternate && !roce_lab_verify_range(auxiliary + roce_lab_aux_offset(qp, write, workload->writes, size),
                                                    size, logical, true))
                return false;
        }
    }
    puts("Application verified both registered destination buffers");
    return true;
}

bool roce_app_write(const char *path, const char *registration_device)
{
    int socket = open_socket(path, false);
    if (socket < 0)
        return false;
    bool success = false;
    int region = -1;
    struct roce_app_rdmacm cma = {0};
    struct app_workload workload;
    if (!configure_peer(socket, NULL) || !receive_app_message(socket, &workload, sizeof(workload)))
        goto disconnect;
    if (workload.version != APP_VERSION || workload.sender > 1 ||
        (workload.flags & ~(APP_USE_CM | APP_USE_VERBS_MR | APP_WRITE_DMA_ALIAS | APP_SCATTER_GATHER |
                             APP_NATIVE_QUEUES | APP_RDMACM | APP_TWO_SGE_WRITES | APP_MULTI_MR_WRITES |
                             APP_MULTI_MR_TARGETS)) ||
        workload.reserved || !workload.post_batch || workload.post_batch > workload.writes ||
        !workload.qps || workload.qps > ROCE_LAB_MAX_QPS || !workload.workers ||
        workload.workers > workload.qps || !workload.writes || workload.writes > ROCE_PSN_MASK) {
        errno = EPROTO;
        goto disconnect;
    }
    bool in_band_cm = (workload.flags & APP_USE_CM) != 0;
    bool verbs_mr = (workload.flags & APP_USE_VERBS_MR) != 0;
    bool native_queues = (workload.flags & APP_NATIVE_QUEUES) != 0;
    bool rdmacm = (workload.flags & APP_RDMACM) != 0;
    bool two_sge_writes = (workload.flags & APP_TWO_SGE_WRITES) != 0;
    bool multi_mr_writes = (workload.flags & APP_MULTI_MR_WRITES) != 0;
    bool multiple_targets = (workload.flags & APP_MULTI_MR_TARGETS) != 0;
    bool auxiliary_receive = multiple_targets && !workload.sender;
    uint32_t send_sges = two_sge_writes ? ROCE_NATIVE_MAX_SEND_SGES : 1;
    uint64_t write_count = (uint64_t)workload.qps * workload.writes;
    uint64_t write_size = workload.bytes / write_count;
    const char *external_qp = secure_getenv("ROCE_DPDK_CM_EXTERNAL_QP");
    bool owned_qp = rdmacm && (!external_qp || strcmp(external_qp, "1"));
    if ((verbs_mr && registration_device) ||
        (multi_mr_writes && !two_sge_writes) ||
        (multiple_targets && (!verbs_mr || workload.writes < 2 || !write_size || write_size > UINT32_MAX ||
                              workload.bytes % write_count || (workload.sender && !native_queues))) ||
        (two_sge_writes && (!native_queues || write_size < 2 || write_size > ROCE_NATIVE_MAX_WRITE ||
                            workload.bytes % write_count)) ||
        (native_queues && (!verbs_mr || !workload.sender ||
                           workload.post_batch > roce_native_queue_capacity(workload.writes))) ||
        (rdmacm && (!in_band_cm || !verbs_mr || workload.qps != 1 ||
                    (workload.sender && !native_queues)))) {
        errno = EINVAL;
        goto disconnect;
    }
    if (in_band_cm && !rdmacm && !roce_lab_send(socket, workload.sender ? APP_CONNECT : APP_LISTEN))
        goto disconnect;
    size_t length = mapping_length(workload.bytes);
    if (!length) {
        errno = EINVAL;
        goto disconnect;
    }
    int flags = MAP_SHARED;
    if (registration_device || verbs_mr) {
        flags |= MAP_ANONYMOUS;
    } else {
        region = create_shared_file("roce-app-mr", length);
        if (region < 0)
            goto disconnect;
    }
    uint8_t *buffer = mmap(NULL, length, PROT_READ | PROT_WRITE, flags, region, 0);
    if (buffer == MAP_FAILED)
        goto disconnect;
    size_t bytes = (size_t)workload.bytes;
    uint8_t *secondary = NULL;
    size_t second_bytes = 0, second_mapping_length = 0;
    if (multi_mr_writes || auxiliary_receive) {
        second_bytes = auxiliary_receive ? roce_lab_auxiliary_bytes(workload.qps, workload.writes, (uint32_t)write_size) :
                       (write_size - roce_app_first_sge_length((uint32_t)write_size)) * write_count;
        second_mapping_length = mapping_length(second_bytes);
        secondary = mmap(NULL, second_mapping_length, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (secondary == MAP_FAILED) {
            int error = errno;
            munmap(buffer, length);
            errno = error;
            goto disconnect;
        }
    }
    if (two_sge_writes) {
        size_t first = roce_app_first_sge_length((uint32_t)write_size);
        size_t second = (size_t)write_size - first;
        /* Reverse the spans in local memory to exercise ordered, noncontiguous gathers. */
        for (size_t offset = 0; offset < bytes; offset += (size_t)write_size) {
            uint8_t *second_span = secondary ? secondary + offset / write_size * second : buffer + offset;
            roce_lab_fill_range(second_span, second, offset + first, true);
            roce_lab_fill_range(buffer + offset + second, first, offset, true);
        }
    } else {
        roce_lab_fill(buffer, bytes, workload.sender);
    }
    struct app_verbs verbs = {0};
    struct app_cm_queue queue = { .socket = socket, .fd = -1, .receiver = !workload.sender };
    bool offered;
    if (verbs_mr) {
        bool dma_writable = (workload.flags & APP_WRITE_DMA_ALIAS) != 0;
        offered = (!(multi_mr_writes || auxiliary_receive) || !setenv("ROCE_DPDK_MR_PD_SCOPE", "1", 1)) &&
                  register_verbs_memory(&verbs, buffer, bytes, workload.sender, dma_writable);
        if (offered && secondary) {
            int access = auxiliary_receive ? IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE :
                         (dma_writable ? IBV_ACCESS_LOCAL_WRITE : 0);
            verbs.second_mr = ibv_reg_mr(verbs.pd, secondary, second_bytes, access);
            offered = verbs.second_mr != NULL;
        }
        if (offered && rdmacm) {
            uint32_t depth = roce_native_queue_capacity(workload.writes);
            if (owned_qp) {
                queue.resources.cq = ibv_create_cq(verbs.context, (int)depth, NULL, NULL, 0);
                offered = queue.resources.cq != NULL;
            } else {
                int error = roce_app_create_native_qp(verbs.pd, depth, send_sges, queue.receiver, &queue.resources);
                if (error)
                    errno = error;
                else
                    printf("Application created verbs QP %06x before CM\n", queue.resources.qp->qp_num);
                offered = !error;
            }
        }
        offered = offered && send_app_message(socket, &verbs.mr->lkey, sizeof(verbs.mr->lkey));
        if (offered && auxiliary_receive)
            offered = send_app_message(socket, &verbs.second_mr->lkey, sizeof(verbs.second_mr->lkey));
        if (offered && rdmacm) {
            uint32_t qpn = owned_qp ? 0 : queue.resources.qp->qp_num;
            offered = send_app_message(socket, &qpn, sizeof(qpn));
        }
    } else {
        if (registration_device)
            region = pin_mapping(registration_device, buffer, length, workload.sender);
        offered = region >= 0 && send_mapping(socket, region, registration_device ? APP_PINNED : APP_MEMFD);
    }
    const char *app_bw = secure_getenv("ROCE_DPDK_APP_BW");
    bool report_bandwidth = app_bw && !strcmp(app_bw, "1");
    struct app_usage start = {0}, end = {0};
    if (offered) {
        bool connected = true;
        if (rdmacm) {
            struct roce_cma_lab_address addresses;
            connected = receive_app_message(socket, &addresses, sizeof(addresses)) &&
                        roce_app_rdmacm_connect(&cma, &addresses, verbs.pd, &queue.resources,
                                                workload.sender, send_sges,
                                                owned_qp ? NULL : prepare_cm_queue, &queue);
            if (connected && owned_qp && workload.sender) {
                int error = receive_cm_queue(&queue);
                if (error)
                    errno = error;
                connected = !error;
            }
        }
        if (connected && (in_band_cm || native_queues))
            connected = roce_lab_receive(socket, APP_ESTABLISHED, false) == 1;
        if (connected && in_band_cm)
            puts(rdmacm ? "Application librdmacm established" : "Application CM established");
        if (connected) {
            if (report_bandwidth)
                start = app_usage();
            if (native_queues)
                success = post_native_writes(&verbs, socket, &workload, rdmacm ? &cma : NULL,
                                             rdmacm ? &queue : NULL);
            else if (workload.sender)
                success = post_writes(socket, &workload);
            else
                success = roce_lab_receive(socket, 'D', false) == 1 &&
                          verify_targets(&workload, buffer, secondary, (uint32_t)write_size) &&
                          roce_lab_send(socket, 'V') && roce_lab_receive(socket, 'C', false) == 1;
            if (report_bandwidth && success)
                end = app_usage();
        }
        if (success && rdmacm) {
            success = roce_app_rdmacm_finish(&cma);
            if (success)
                puts("Application librdmacm disconnected");
        }
        if (!success && !errno)
            errno = EPROTO;
    }
    int saved_errno = errno;
    if (!success)
        shutdown(socket, SHUT_RDWR);
    int error = owned_qp ? roce_app_rdmacm_destroy_qp(&cma) : 0;
    if (owned_qp)
        queue.resources.qp = NULL;
    if (!error)
        error = roce_app_destroy_native_qp(&queue.resources);
    if (queue.fd >= 0)
        close(queue.fd);
    if (!error) {
        roce_app_rdmacm_destroy(&cma);
        error = release_verbs_memory(&verbs);
    }
    if (success && error) {
        saved_errno = error;
        success = false;
    }
    if (success && verbs_mr && !roce_lab_send(socket, APP_DEREGISTERED)) {
        saved_errno = errno;
        success = false;
    }
    if (success) {
        if (start.cpu_ns && end.cpu_ns >= start.cpu_ns && end.wall_ns > start.wall_ns)
            printf("APP_TRANSFER role=%s payload_bytes=%zu start_ns=%" PRIu64
                   " elapsed_ns=%" PRIu64 " process_cpu_ns=%" PRIu64 "\n",
                   workload.sender ? "tx" : "rx", bytes, start.wall_ns,
                   end.wall_ns - start.wall_ns, end.cpu_ns - start.cpu_ns);
        printf("Application %s %zu bytes\n", workload.sender ? "completed" : "verified", bytes);
    }
    if (!error) {
        if (secondary)
            munmap(secondary, second_mapping_length);
        munmap(buffer, length);
    } else if (saved_errno == 0)
        saved_errno = error;
    if (!success && !saved_errno)
        saved_errno = EPROTO;
    errno = saved_errno;

disconnect:
    {
        int error = errno;
        roce_app_rdmacm_destroy(&cma);
        if (region >= 0)
            close(region);
        close(socket);
        errno = error;
        return success;
    }
}
