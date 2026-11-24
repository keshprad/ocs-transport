#define _GNU_SOURCE
#include "roce_mr_server.h"
#include "roce_mr_ipc.h"
#include "../kernel/roce_mr_uapi.h"

#include <errno.h>
#include <linux/magic.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/vfs.h>
#include <unistd.h>

struct roce_mr_registration {
    struct roce_mr_request request;
    uint8_t *mapping;
    int mapping_fd;
    int socket;
    pid_t owner;
    unsigned leases;
    uint32_t lkey, rkey;
};

struct roce_mr_server {
    char *path;
    int listener;
    int wakeup;
    pthread_t thread;
    pthread_mutex_t mutex;
    size_t page_size;
    bool stopping;
    struct roce_mr_registration regions[ROCE_MR_IPC_MAX_REGISTRATIONS];
};

static void release_mapping(struct roce_mr_registration *region)
{
    if (region->mapping) {
        munmap(region->mapping, (size_t)region->request.mapped_length);
        close(region->mapping_fd);
        region->mapping = NULL;
    }
}

static void disconnect(struct roce_mr_registration *region)
{
    region->owner = 0;
    region->lkey = 0;
    region->rkey = 0;
    if (!region->leases)
        release_mapping(region);
    if (region->socket >= 0) {
        close(region->socket);
        region->socket = -1;
    }
}

static bool available_key(const struct roce_mr_server *server, uint32_t key)
{
    if (!key)
        return false;
    for (size_t i = 0; i < ROCE_MR_IPC_MAX_REGISTRATIONS; ++i) {
        const struct roce_mr_registration *region = &server->regions[i];
        if (key == region->lkey || key == region->rkey)
            return false;
    }
    return true;
}

static int allocate_keys(const struct roce_mr_server *server, bool remote_write,
                         struct roce_mr_reply *reply)
{
    do {
        if (getentropy(&reply->lkey, sizeof(reply->lkey)) < 0)
            return errno;
    } while (!available_key(server, reply->lkey));
    if (remote_write) {
        do {
            if (getentropy(&reply->rkey, sizeof(reply->rkey)) < 0)
                return errno;
        } while (reply->rkey == reply->lkey || !available_key(server, reply->rkey));
    }
    return 0;
}

static bool valid_request(const struct roce_mr_server *server, const struct roce_mr_request *request)
{
    size_t mask = server->page_size - 1;
    bool writable = request->access & ROCE_MR_IPC_WRITE;
    return request->version == ROCE_MR_IPC_VERSION &&
           !(request->access & ~(ROCE_MR_IPC_WRITE | ROCE_MR_IPC_REMOTE_WRITE)) &&
           (!(request->access & ROCE_MR_IPC_REMOTE_WRITE) || writable) &&
           request->length && request->length - 1 <= UINT64_MAX - request->iova &&
           request->offset == (request->iova & mask) &&
           request->mapped_length <= SIZE_MAX && !(request->mapped_length & mask) &&
           request->mapped_length > request->offset &&
           request->length <= request->mapped_length - request->offset &&
           request->mapped_length - request->offset - request->length <= mask;
}

static int install_mapping(struct roce_mr_server *server, struct roce_mr_registration *region,
                           const struct roce_mr_request *request, int fd, struct roce_mr_reply *reply)
{
    if (!valid_request(server, request))
        return EINVAL;

    bool writable = request->access & ROCE_MR_IPC_WRITE;
    struct roce_mr_info info;
    struct statfs filesystem;
    if (fstatfs(fd, &filesystem) != 0 || filesystem.f_type != ANON_INODE_FS_MAGIC ||
        ioctl(fd, ROCE_MR_GET_INFO, &info) != 0 || info.abi_version != ROCE_MR_ABI_VERSION ||
        info.length != request->mapped_length || info.access != (writable ? ROCE_MR_ACCESS_LOCAL_WRITE : 0))
        return EACCES;
    int error = allocate_keys(server, request->access & ROCE_MR_IPC_REMOTE_WRITE, reply);
    if (error)
        return error;

    void *buffer = mmap(NULL, (size_t)request->mapped_length, PROT_READ | (writable ? PROT_WRITE : 0),
                        MAP_SHARED, fd, 0);
    if (buffer == MAP_FAILED)
        return errno;
    region->mapping = buffer;
    region->mapping_fd = fd;
    region->request = *request;
    region->lkey = reply->lkey;
    region->rkey = reply->rkey;
    return 0;
}

static void register_mapping(struct roce_mr_server *server, struct roce_mr_registration *region)
{
    struct roce_mr_request request;
    int fd = roce_mr_ipc_receive_registration(region->socket, &request);
    if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return;
    struct roce_mr_reply reply = {0};
    if (fd < 0) {
        reply.error = errno;
    } else {
        reply.error = install_mapping(server, region, &request, fd, &reply);
        if (reply.error)
            close(fd);
    }
    if (reply.error)
        reply.lkey = reply.rkey = 0;
    if (roce_mr_ipc_send_reply(region->socket, &reply) || reply.error)
        disconnect(region);
}

static void deregister_mapping(struct roce_mr_registration *region)
{
    char command;
    ssize_t size = recv(region->socket, &command, sizeof(command), MSG_TRUNC);
    if (size < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        return;
    if (size != sizeof(command) || command != ROCE_MR_IPC_DEREGISTER) {
        disconnect(region);
        return;
    }

    const struct roce_mr_reply reply = { .error = region->leases ? EBUSY : 0 };
    if (!reply.error) {
        region->lkey = region->rkey = 0;
        release_mapping(region);
    }
    if (roce_mr_ipc_send_reply(region->socket, &reply) || !reply.error)
        disconnect(region);
}

static void accept_registration(struct roce_mr_server *server)
{
    int fd = accept4(server->listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0)
        return;
    struct ucred peer;
    socklen_t length = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) || length != sizeof(peer) ||
        peer.uid != geteuid() || peer.pid <= 0) {
        close(fd);
        return;
    }
    for (size_t i = 0; i < ROCE_MR_IPC_MAX_REGISTRATIONS; ++i) {
        struct roce_mr_registration *region = &server->regions[i];
        if (region->socket < 0 && !region->mapping) {
            region->socket = fd;
            region->owner = peer.pid;
            return;
        }
    }
    const struct roce_mr_reply reply = { .error = ENOSPC };
    roce_mr_ipc_send_reply(fd, &reply);
    close(fd);
}

static void *server_loop(void *opaque)
{
    struct roce_mr_server *server = opaque;
    struct pollfd pollfds[ROCE_MR_IPC_MAX_REGISTRATIONS + 2] = {
        { .fd = server->wakeup, .events = POLLIN },
        { .fd = server->listener, .events = POLLIN },
    };
    for (;;) {
        for (size_t i = 0; i < ROCE_MR_IPC_MAX_REGISTRATIONS; ++i)
            pollfds[i + 2] = (struct pollfd) { .fd = server->regions[i].socket, .events = POLLIN };
        if (poll(pollfds, ROCE_MR_IPC_MAX_REGISTRATIONS + 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pollfds[0].revents)
            break;

        pthread_mutex_lock(&server->mutex);
        for (size_t i = 0; i < ROCE_MR_IPC_MAX_REGISTRATIONS; ++i) {
            if (!pollfds[i + 2].revents)
                continue;
            struct roce_mr_registration *region = &server->regions[i];
            if (region->mapping)
                deregister_mapping(region);
            else
                register_mapping(server, region);
        }
        if (pollfds[1].revents & POLLIN)
            accept_registration(server);
        pthread_mutex_unlock(&server->mutex);
    }
    pthread_mutex_lock(&server->mutex);
    server->stopping = true;
    for (size_t i = 0; i < ROCE_MR_IPC_MAX_REGISTRATIONS; ++i)
        disconnect(&server->regions[i]);
    pthread_mutex_unlock(&server->mutex);
    return NULL;
}

struct roce_mr_server *roce_mr_server_start(const char *path)
{
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        errno = ENOTSUP;
        return NULL;
    }
    struct roce_mr_server *server = calloc(1, sizeof(*server));
    if (!server)
        return NULL;
    server->path = strdup(path);
    if (!server->path)
        goto free_server;
    server->listener = roce_mr_ipc_listen(path);
    if (server->listener < 0)
        goto free_server;
    server->wakeup = eventfd(0, EFD_CLOEXEC);
    if (server->wakeup < 0)
        goto close_listener;

    for (size_t i = 0; i < ROCE_MR_IPC_MAX_REGISTRATIONS; ++i)
        server->regions[i].socket = -1;
    server->page_size = (size_t)page_size;
    int error = pthread_mutex_init(&server->mutex, NULL);
    if (!error) {
        error = pthread_create(&server->thread, NULL, server_loop, server);
        if (!error)
            return server;
        pthread_mutex_destroy(&server->mutex);
    }
    close(server->wakeup);
    errno = error;

close_listener:
    error = errno;
    close(server->listener);
    unlink(path);
    errno = error;
free_server:
    free(server->path);
    free(server);
    return NULL;
}

int roce_mr_acquire(struct roce_mr_server *server, pid_t owner, uint32_t lkey, uint32_t access,
                   struct roce_mr_lease *lease)
{
    int error = ENOENT;
    pthread_mutex_lock(&server->mutex);
    for (size_t i = 0; !server->stopping && i < ROCE_MR_IPC_MAX_REGISTRATIONS; ++i) {
        struct roce_mr_registration *region = &server->regions[i];
        if (region->socket < 0 || !region->mapping || region->owner != owner || region->lkey != lkey)
            continue;
        if (access & ~region->request.access) {
            error = EACCES;
            break;
        }
        ++region->leases;
        *lease = (struct roce_mr_lease) {
            .server = server, .registration = region,
            .buffer = region->mapping + region->request.offset, .length = (size_t)region->request.length,
            .mapping = region->mapping, .mapped_length = (size_t)region->request.mapped_length,
            .iova = region->request.iova, .rkey = region->rkey,
        };
        error = 0;
        break;
    }
    pthread_mutex_unlock(&server->mutex);
    return error;
}

int roce_mr_export_dmabuf(const struct roce_mr_lease *lease)
{
    if (!lease->registration) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(lease->registration->mapping_fd, ROCE_MR_EXPORT_DMABUF, 0);
}

void roce_mr_release(struct roce_mr_lease *lease)
{
    if (!lease->server)
        return;
    struct roce_mr_server *server = lease->server;
    struct roce_mr_registration *region = lease->registration;
    pthread_mutex_lock(&server->mutex);
    if (!--region->leases && region->socket < 0)
        release_mapping(region);
    pthread_mutex_unlock(&server->mutex);
    *lease = (struct roce_mr_lease) {0};
}

int roce_mr_server_stop(struct roce_mr_server *server)
{
    pthread_mutex_lock(&server->mutex);
    for (size_t i = 0; i < ROCE_MR_IPC_MAX_REGISTRATIONS; ++i) {
        if (server->regions[i].leases) {
            pthread_mutex_unlock(&server->mutex);
            return EBUSY;
        }
    }
    server->stopping = true;
    pthread_mutex_unlock(&server->mutex);
    eventfd_write(server->wakeup, 1);
    pthread_join(server->thread, NULL);
    pthread_mutex_destroy(&server->mutex);
    close(server->listener);
    close(server->wakeup);
    unlink(server->path);
    free(server->path);
    free(server);
    return 0;
}
