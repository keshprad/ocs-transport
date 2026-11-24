#define _GNU_SOURCE
#include "roce_app.h"
#include "roce_doorbell.h"
#include "roce_fd.h"
#include "roce_lab.h"
#include "roce_mr_ipc.h"
#include "../kernel/roce_mr_uapi.h"

#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <linux/magic.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <unistd.h>

enum {
    APP_VERSION = 2,
    APP_MEMFD = 1,
    APP_PINNED = 2,
    APP_DOORBELL = 3,
    APP_USE_CM = 1u << 0,
    APP_USE_VERBS_MR = 1u << 1,
    APP_WRITE_DMA_ALIAS = 1u << 2,
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
};

static const size_t doorbell_bytes = ROCE_LAB_MAX_QPS * sizeof(struct roce_doorbell);

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
    struct pollfd wait = { .fd = listener, .events = POLLIN };
    int ready;
    do {
        ready = poll(&wait, 1, ROCE_LAB_TIMEOUT * 1000);
    } while (ready < 0 && errno == EINTR);
    int fd = -1;
    if (ready > 0)
        fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    else if (!ready)
        errno = ETIMEDOUT;
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

static struct roce_doorbell *offer_doorbells(int socket, uint32_t workers)
{
    int fd = create_shared_file("roce-doorbells", doorbell_bytes);
    if (fd < 0)
        return NULL;
    struct roce_doorbell *queues = mmap(NULL, doorbell_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    bool valid = queues != MAP_FAILED;
    for (uint32_t i = 0; valid && i < workers; ++i)
        valid = roce_doorbell_init(&queues[i]);
    if (queues != MAP_FAILED && !valid)
        errno = ENOTSUP;
    bool sent = valid && send_mapping(socket, fd, APP_DOORBELL);
    int error = errno;
    close(fd);
    if (sent && roce_lab_receive(socket, 'P', false) == 1)
        return queues;
    if (queues != MAP_FAILED)
        munmap(queues, doorbell_bytes);
    errno = sent ? EPROTO : error;
    return NULL;
}

bool roce_app_take_writes(struct roce_doorbell *doorbell, uint32_t first, uint32_t count, uint32_t writes)
{
    uint32_t cursor = 0, submitted = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t item = roce_doorbell_take(doorbell, &cursor);
        uint32_t qp = (uint32_t)(item >> 32);
        if (qp <= first || qp - first > count || (uint32_t)item != writes)
            return false;
        uint32_t bit = UINT32_C(1) << (qp - first - 1);
        if (submitted & bit)
            return false;
        submitted |= bit;
    }
    return roce_doorbell_take(doorbell, &cursor) == 0;
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
    if (recv(memory->socket, &lkey, sizeof(lkey), MSG_TRUNC) != sizeof(lkey)) {
        errno = EPROTO;
        return false;
    }
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

bool roce_app_attach(const char *path, const struct roce_lab_options *options,
                     bool dma_writable, bool in_band_cm, struct roce_mr_server *registrations,
                     struct roce_app_memory *memory)
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
    struct roce_app_memory attached = { .socket = socket };
    pid_t pid;
    const struct app_workload workload = {
        .bytes = bytes, .version = APP_VERSION, .sender = options->sender,
        .qps = options->qps, .workers = options->workers, .writes = options->writes,
        .flags = (in_band_cm ? APP_USE_CM : 0) | (registrations ? APP_USE_VERBS_MR : 0) |
                 (registrations && dma_writable ? APP_WRITE_DMA_ALIAS : 0),
    };
    if (!configure_peer(socket, &pid) ||
        send(socket, &workload, sizeof(workload), MSG_NOSIGNAL) != sizeof(workload))
        goto disconnect;
    if (in_band_cm) {
        char command;
        if (recv(socket, &command, sizeof(command), MSG_TRUNC) != sizeof(command) ||
            command != (options->sender ? APP_CONNECT : APP_LISTEN)) {
            errno = EPROTO;
            goto disconnect;
        }
    }

    bool received = registrations ?
        receive_verbs_memory(&attached, registrations, pid, (size_t)bytes, options->sender, dma_writable) :
        receive_local_memory(&attached, length, options->sender, dma_writable);
    if (!received)
        goto disconnect;
    if (options->sender && !in_band_cm) {
        attached.doorbells = offer_doorbells(socket, options->workers);
        if (!attached.doorbells)
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
    if (!options->sender)
        return true;
    memory->doorbells = offer_doorbells(memory->socket, options->workers);
    return memory->doorbells != NULL;
}

bool roce_app_complete(struct roce_app_memory *memory)
{
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
        if (memory->doorbells)
            munmap(memory->doorbells, doorbell_bytes);
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
    uint64_t item;
    bool posted;
};

static void *publish_work(void *opaque)
{
    struct producer *producer = opaque;
    producer->posted = roce_doorbell_publish(producer->queue, producer->item);
    return NULL;
}

static bool post_writes(int socket, const struct app_workload *workload)
{
    uint8_t kind;
    int fd = receive_mapping(socket, &kind);
    if (fd < 0)
        return false;
    struct roce_doorbell *queues = MAP_FAILED;
    if (kind == APP_DOORBELL && valid_mapping(fd, APP_MEMFD, doorbell_bytes, false))
        queues = mmap(NULL, doorbell_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (queues == MAP_FAILED)
        return false;

    struct producer producers[ROCE_LAB_MAX_QPS];
    pthread_t threads[ROCE_LAB_MAX_QPS];
    uint32_t created = 0;
    for (; created < workload->qps; ++created) {
        uint32_t qp = created + 1;
        uint32_t worker = (qp * workload->workers - 1) / workload->qps;
        producers[created] = (struct producer) {
            .queue = &queues[worker], .item = (uint64_t)qp << 32 | workload->writes,
        };
        if (pthread_create(&threads[created], NULL, publish_work, &producers[created]) != 0)
            break;
    }
    bool posted = created == workload->qps;
    for (uint32_t i = 0; i < created; ++i) {
        pthread_join(threads[i], NULL);
        posted = posted && producers[i].posted;
    }
    munmap(queues, doorbell_bytes);
    return posted && roce_lab_send(socket, 'P');
}

struct app_verbs {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
};

static bool register_verbs_memory(struct app_verbs *verbs, uint8_t *buffer, size_t length,
                                  bool sender, bool dma_writable)
{
    struct ibv_device **devices = ibv_get_device_list(NULL);
    if (!devices)
        return false;
    errno = ENODEV;
    for (size_t i = 0; devices[i]; ++i) {
        if (strcmp(ibv_get_device_name(devices[i]), "roce_dpdk0") == 0) {
            verbs->context = ibv_open_device(devices[i]);
            break;
        }
    }
    int error = errno;
    ibv_free_device_list(devices);
    if (!verbs->context) {
        errno = error;
        return false;
    }
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
    int error = verbs->mr ? ibv_dereg_mr(verbs->mr) : 0;
    if (!error && verbs->pd)
        error = ibv_dealloc_pd(verbs->pd);
    if (!error && verbs->context)
        error = ibv_close_device(verbs->context);
    return error;
}

bool roce_app_write(const char *path, const char *registration_device)
{
    int socket = open_socket(path, false);
    if (socket < 0)
        return false;
    bool success = false;
    int region = -1;
    struct app_workload workload;
    if (!configure_peer(socket, NULL) ||
        recv(socket, &workload, sizeof(workload), MSG_TRUNC) != sizeof(workload) ||
        workload.version != APP_VERSION || workload.sender > 1 ||
        (workload.flags & ~(APP_USE_CM | APP_USE_VERBS_MR | APP_WRITE_DMA_ALIAS)) ||
        !workload.qps || workload.qps > ROCE_LAB_MAX_QPS || !workload.workers ||
        workload.workers > workload.qps || !workload.writes || workload.writes > ROCE_PSN_MASK) {
        errno = EPROTO;
        goto disconnect;
    }
    bool in_band_cm = (workload.flags & APP_USE_CM) != 0;
    bool verbs_mr = (workload.flags & APP_USE_VERBS_MR) != 0;
    if (verbs_mr && registration_device) {
        errno = EINVAL;
        goto disconnect;
    }
    if (in_band_cm && !roce_lab_send(socket, workload.sender ? APP_CONNECT : APP_LISTEN))
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
    roce_lab_fill(buffer, bytes, workload.sender);
    struct app_verbs verbs = {0};
    bool offered;
    if (verbs_mr) {
        offered = register_verbs_memory(&verbs, buffer, bytes, workload.sender,
                                        (workload.flags & APP_WRITE_DMA_ALIAS) != 0) &&
                  send(socket, &verbs.mr->lkey, sizeof(verbs.mr->lkey), MSG_NOSIGNAL) == sizeof(verbs.mr->lkey);
    } else {
        if (registration_device)
            region = pin_mapping(registration_device, buffer, length, workload.sender);
        offered = region >= 0 && send_mapping(socket, region, registration_device ? APP_PINNED : APP_MEMFD);
    }
    if (offered) {
        bool connected = !in_band_cm || roce_lab_receive(socket, APP_ESTABLISHED, false) == 1;
        if (connected && in_band_cm)
            puts("Application CM established");
        bool done = connected && (workload.sender ? post_writes(socket, &workload) :
            roce_lab_receive(socket, 'D', false) == 1 && roce_lab_verify(buffer, bytes) &&
            roce_lab_send(socket, 'V'));
        success = done && roce_lab_receive(socket, 'C', false) == 1;
        if (!success)
            errno = EPROTO;
    }
    int saved_errno = errno;
    int error = release_verbs_memory(&verbs);
    if (success && error) {
        saved_errno = error;
        success = false;
    }
    if (success && verbs_mr && !roce_lab_send(socket, APP_DEREGISTERED)) {
        saved_errno = errno;
        success = false;
    }
    if (success)
        printf("Application %s %zu bytes\n", workload.sender ? "completed" : "verified", bytes);
    if (!error)
        munmap(buffer, length);
    else if (saved_errno == 0)
        saved_errno = error;
    if (!success && !saved_errno)
        saved_errno = EPROTO;
    errno = saved_errno;

disconnect:
    {
        int error = errno;
        if (region >= 0)
            close(region);
        close(socket);
        errno = error;
        return success;
    }
}
