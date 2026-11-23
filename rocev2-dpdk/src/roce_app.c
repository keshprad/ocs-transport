#define _GNU_SOURCE
#include "roce_app.h"
#include "roce_lab.h"
#include "../kernel/roce_mr_uapi.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <poll.h>
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
    APP_VERSION = 1,
    APP_MEMFD = 1,
    APP_PINNED = 2,
    APP_SEALS = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL,
};

struct app_workload {
    uint64_t bytes;
    uint32_t version;
    uint32_t sender;
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

bool roce_app_attach(const char *path, size_t bytes, bool sender, struct roce_app_memory *memory)
{
    size_t length = mapping_length(bytes);
    if (!length) {
        errno = EINVAL;
        return false;
    }
    int socket = accept_client(path);
    if (socket < 0)
        return false;
    const struct app_workload workload = { .bytes = bytes, .version = APP_VERSION, .sender = sender };
    if (!configure_peer(socket) || send(socket, &workload, sizeof(workload), MSG_NOSIGNAL) != sizeof(workload))
        goto disconnect;

    uint8_t kind;
    int fd = receive_mapping(socket, &kind);
    if (fd < 0)
        goto disconnect;
    if (!valid_mapping(fd, kind, length, sender)) {
        close(fd);
        errno = EINVAL;
        goto disconnect;
    }
    void *buffer = mmap(NULL, length, PROT_READ | (sender ? 0 : PROT_WRITE), MAP_SHARED, fd, 0);
    close(fd);
    if (buffer == MAP_FAILED)
        goto disconnect;
    *memory = (struct roce_app_memory) { .socket = socket, .buffer = buffer, .length = length };
    return true;

disconnect:
    {
        int error = errno;
        close(socket);
        errno = error;
        return false;
    }
}

void roce_app_detach(struct roce_app_memory *memory)
{
    if (memory->socket >= 0) {
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
        workload.version != APP_VERSION || workload.sender > 1) {
        errno = EPROTO;
        goto disconnect;
    }
    size_t length = mapping_length(workload.bytes);
    if (!length) {
        errno = EINVAL;
        goto disconnect;
    }
    int flags = MAP_SHARED;
    if (registration_device) {
        flags |= MAP_ANONYMOUS;
    } else {
        region = memfd_create("roce-app-mr", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (region < 0 || ftruncate(region, (off_t)length) != 0 ||
            fcntl(region, F_ADD_SEALS, APP_SEALS) < 0)
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
        if (workload.sender ||
            (roce_lab_receive(socket, 'D', false) == 1 && roce_lab_verify(buffer, bytes) &&
             roce_lab_send(socket, 'V'))) {
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
