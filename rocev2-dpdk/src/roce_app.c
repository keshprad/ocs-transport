#define _GNU_SOURCE
#include "roce_app.h"
#include "roce_doorbell.h"
#include "roce_lab.h"
#include "../kernel/roce_mr_uapi.h"

#include <errno.h>
#include <fcntl.h>
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
    APP_CONNECT = 'A',
    APP_LISTEN = 'L',
    APP_ESTABLISHED = 'E',
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

static bool configure_peer(int fd)
{
    struct ucred credentials;
    socklen_t length = sizeof(credentials);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0)
        return false;
    if (length != sizeof(credentials) || credentials.uid != geteuid()) {
        errno = EACCES;
        return false;
    }
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
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(fd))];
    } control = {0};
    struct iovec data = { .iov_base = &kind, .iov_len = sizeof(kind) };
    struct msghdr message = { .msg_iov = &data, .msg_iovlen = 1,
                              .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    return sendmsg(socket, &message, MSG_NOSIGNAL) == (ssize_t)sizeof(kind);
}

static int receive_mapping(int socket, uint8_t *kind)
{
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int))];
    } control = {0};
    struct iovec data = { .iov_base = kind, .iov_len = sizeof(*kind) };
    struct msghdr message = { .msg_iov = &data, .msg_iovlen = 1,
                              .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
    ssize_t received = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
    if (received < 0)
        return -1;
    int descriptor = -1;
    unsigned int count = 0;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(0))
            continue;
        size_t bytes = header->cmsg_len - CMSG_LEN(0);
        for (size_t offset = 0; offset + sizeof(int) <= bytes; offset += sizeof(int)) {
            int fd;
            memcpy(&fd, CMSG_DATA(header) + offset, sizeof(fd));
            if (++count == 1)
                descriptor = fd;
            else
                close(fd);
        }
    }
    if (received == sizeof(*kind) && count == 1 && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
        return descriptor;
    if (descriptor >= 0)
        close(descriptor);
    errno = EPROTO;
    return -1;
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

bool roce_app_attach(const char *path, const struct roce_lab_options *options,
                     bool dma_writable, bool in_band_cm, struct roce_app_memory *memory)
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
    const struct app_workload workload = {
        .bytes = bytes, .version = APP_VERSION, .sender = options->sender,
        .qps = options->qps, .workers = options->workers, .writes = options->writes,
        .flags = in_band_cm ? APP_USE_CM : 0,
    };
    if (!configure_peer(socket) || send(socket, &workload, sizeof(workload), MSG_NOSIGNAL) != sizeof(workload))
        goto disconnect;
    if (in_band_cm) {
        char command;
        if (recv(socket, &command, sizeof(command), MSG_TRUNC) != sizeof(command) ||
            command != (options->sender ? APP_CONNECT : APP_LISTEN)) {
            errno = EPROTO;
            goto disconnect;
        }
    }

    uint8_t kind;
    int fd = receive_mapping(socket, &kind);
    if (fd < 0)
        goto disconnect;
    /* mlx5 verbs registers external memory with local write access. */
    if (dma_writable && kind != APP_MEMFD) {
        close(fd);
        errno = EOPNOTSUPP;
        goto disconnect;
    }
    if (!valid_mapping(fd, kind, length, options->sender)) {
        close(fd);
        errno = EINVAL;
        goto disconnect;
    }
    int prot = PROT_READ | (!options->sender || dma_writable ? PROT_WRITE : 0);
    void *buffer = mmap(NULL, length, prot, MAP_SHARED, fd, 0);
    close(fd);
    if (buffer == MAP_FAILED)
        goto disconnect;
    struct roce_doorbell *doorbells = NULL;
    if (options->sender && !in_band_cm) {
        doorbells = offer_doorbells(socket, options->workers);
        if (!doorbells) {
            int error = errno;
            munmap(buffer, length);
            errno = error;
            goto disconnect;
        }
    }
    *memory = (struct roce_app_memory) {
        .socket = socket, .buffer = buffer, .length = length, .doorbells = doorbells,
    };
    return true;

disconnect:
    {
        int error = errno;
        close(socket);
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

void roce_app_detach(struct roce_app_memory *memory)
{
    if (memory->socket >= 0) {
        if (memory->doorbells)
            munmap(memory->doorbells, doorbell_bytes);
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

bool roce_app_write(const char *path, const char *registration_device)
{
    int socket = open_socket(path, false);
    if (socket < 0)
        return false;
    bool success = false;
    int region = -1;
    struct app_workload workload;
    if (!configure_peer(socket) ||
        recv(socket, &workload, sizeof(workload), MSG_TRUNC) != sizeof(workload) ||
        workload.version != APP_VERSION || workload.sender > 1 || (workload.flags & ~APP_USE_CM) ||
        !workload.qps || workload.qps > ROCE_LAB_MAX_QPS || !workload.workers ||
        workload.workers > workload.qps || !workload.writes || workload.writes > ROCE_PSN_MASK) {
        errno = EPROTO;
        goto disconnect;
    }
    bool in_band_cm = (workload.flags & APP_USE_CM) != 0;
    if (in_band_cm && !roce_lab_send(socket, workload.sender ? APP_CONNECT : APP_LISTEN))
        goto disconnect;
    size_t length = mapping_length(workload.bytes);
    if (!length) {
        errno = EINVAL;
        goto disconnect;
    }
    int flags = MAP_SHARED;
    if (registration_device) {
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
    if (registration_device)
        region = pin_mapping(registration_device, buffer, length, workload.sender);
    if (region >= 0 && send_mapping(socket, region, registration_device ? APP_PINNED : APP_MEMFD)) {
        bool connected = !in_band_cm || roce_lab_receive(socket, APP_ESTABLISHED, false) == 1;
        if (connected && in_band_cm)
            puts("Application CM established");
        bool done = connected && (workload.sender ? post_writes(socket, &workload) :
            roce_lab_receive(socket, 'D', false) == 1 && roce_lab_verify(buffer, bytes) &&
            roce_lab_send(socket, 'V'));
        if (done) {
            success = roce_lab_receive(socket, 'C', false) == 1;
            if (success)
                printf("Application %s %zu bytes\n", workload.sender ? "completed" : "verified", bytes);
        }
        if (!success)
            errno = EPROTO;
    }
    munmap(buffer, length);

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
