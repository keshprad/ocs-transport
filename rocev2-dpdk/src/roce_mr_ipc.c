#define _GNU_SOURCE
#include "roce_mr_ipc.h"
#include "roce_fd.h"

#include <stddef.h>
#include <sys/stat.h>
#include <sys/un.h>

static int open_socket(const char *path, bool listener)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    size_t length = strlen(path);
    if (!length || length >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(address.sun_path, path, length + 1);
    socklen_t size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (listener) {
        if (bind(fd, (struct sockaddr *)&address, size) == 0) {
            if (chmod(path, S_IRUSR | S_IWUSR) == 0 && listen(fd, SOMAXCONN) == 0)
                return fd;
            int error = errno;
            unlink(path);
            errno = error;
        }
    } else if (connect(fd, (struct sockaddr *)&address, size) == 0) {
        struct ucred peer;
        socklen_t peer_length = sizeof(peer);
        const struct timeval timeout = { .tv_sec = 10 };
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_length) == 0) {
            if (peer_length != sizeof(peer) || peer.uid != geteuid()) {
                errno = EACCES;
            } else if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
                       setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0) {
                return fd;
            }
        }
    }
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}

int roce_mr_ipc_connect(const char *path)
{
    return open_socket(path, false);
}

int roce_mr_ipc_listen(const char *path)
{
    return open_socket(path, true);
}

static int send_wire(int socket, const void *message, size_t length)
{
    ssize_t bytes;
    do {
        bytes = send(socket, message, length, MSG_NOSIGNAL);
    } while (bytes < 0 && errno == EINTR);
    return bytes == (ssize_t)length ? 0 : bytes < 0 ? errno : EPROTO;
}

static int receive_wire(int socket, void *message, size_t length)
{
    ssize_t bytes;
    do {
        bytes = recv(socket, message, length, MSG_TRUNC);
    } while (bytes < 0 && errno == EINTR);
    if (bytes < 0)
        return errno;
    if (!bytes)
        return ECONNRESET;
    return bytes == (ssize_t)length ? 0 : EPROTO;
}

int roce_mr_ipc_send_registration(int socket, const struct roce_mr_request *request, int mapping_fd)
{
    size_t length = request->scope ? sizeof(*request) : offsetof(struct roce_mr_request, scope);
    if (mapping_fd < 0)
        return send_wire(socket, request, length);
    return roce_send_fd(socket, request, length, mapping_fd) ? 0 : errno;
}

int roce_mr_ipc_read_reply(int socket, struct roce_mr_reply *reply)
{
    int error = receive_wire(socket, reply, sizeof(*reply));
    if (!error && (reply->reserved || reply->error < 0))
        return EPROTO;
    return error;
}

int roce_mr_ipc_receive_registration(int socket, struct roce_mr_request *request, int *mapping_fd)
{
    *request = (struct roce_mr_request){0};
    *mapping_fd = -1;
    ssize_t length = roce_receive_record_fd(socket, request, sizeof(*request), mapping_fd, 0);
    if (length < 0)
        return errno;
    if (length == (ssize_t)offsetof(struct roce_mr_request, scope) &&
        (request->version == ROCE_MR_IPC_VERSION || request->version == ROCE_MR_IPC_TICKET_VERSION))
        return 0;
    if (length == sizeof(*request) && request->scope &&
        (request->version == ROCE_MR_IPC_SCOPED_VERSION || request->version == ROCE_MR_IPC_SCOPED_TICKET_VERSION))
        return 0;
    if (*mapping_fd >= 0)
        close(*mapping_fd);
    *mapping_fd = -1;
    return EPROTO;
}

int roce_mr_ipc_send_reply(int socket, const struct roce_mr_reply *reply)
{
    return send_wire(socket, reply, sizeof(*reply));
}

int roce_mr_ipc_read_ticket_ready(int socket, struct roce_mr_ticket_ready *reply)
{
    int error = receive_wire(socket, reply, sizeof(*reply));
    if (!error && (reply->error < 0 || (!reply->error && (!reply->portid || !reply->cookie))))
        return EPROTO;
    return error;
}

int roce_mr_ipc_send_ticket_ready(int socket, const struct roce_mr_ticket_ready *reply)
{
    return send_wire(socket, reply, sizeof(*reply));
}
