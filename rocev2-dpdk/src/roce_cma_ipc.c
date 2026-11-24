#include "roce_cma_ipc.h"
#include "roce_fd.h"

#include <errno.h>
#include <sys/socket.h>

int roce_cma_send(int socket, const struct roce_cma_message *message)
{
    ssize_t size;
    do {
        size = send(socket, message, sizeof(*message), MSG_NOSIGNAL);
    } while (size < 0 && errno == EINTR);
    if (size < 0)
        return errno;
    return size == sizeof(*message) ? 0 : EPROTO;
}

int roce_cma_receive(int socket, struct roce_cma_message *message, bool nonblocking)
{
    int fd = -1;
    int result = roce_cma_receive_fd(socket, message, nonblocking, &fd);
    if (fd < 0)
        return result;
    close(fd);
    return -EPROTO;
}

int roce_cma_send_fd(int socket, const struct roce_cma_message *message, int fd)
{
    return roce_send_fd(socket, message, sizeof(*message), fd) ? 0 : errno;
}

int roce_cma_receive_fd(int socket, struct roce_cma_message *message, bool nonblocking, int *fd)
{
    *fd = -1;
    if (!roce_receive_optional_fd_flags(socket, message, sizeof(*message), fd,
                                       nonblocking ? MSG_DONTWAIT : 0))
        return nonblocking && (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -errno;
    uint8_t allowed_flags = message->type == ROCE_CMA_CONNECT || message->type == ROCE_CMA_ACCEPT ?
                            ROCE_CMA_QP_OWNED : 0;
    if (message->version == ROCE_CMA_IPC_VERSION && message->status >= 0 &&
        message->private_length <= ROCE_CMA_PRIVATE_MAX && !(message->flags & ~allowed_flags) &&
        !message->reserved[0] && !message->reserved[1] && !message->reserved[2] &&
        (*fd < 0 || message->type == ROCE_CMA_CONFIGURE_QP || message->type == ROCE_CMA_CONNECT_RESPONSE))
        return 1;
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
    return -EPROTO;
}
