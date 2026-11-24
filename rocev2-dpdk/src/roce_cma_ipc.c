#include "roce_cma_ipc.h"

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
    int flags = MSG_TRUNC | (nonblocking ? MSG_DONTWAIT : 0);
    ssize_t size;
    do {
        size = recv(socket, message, sizeof(*message), flags);
    } while (size < 0 && errno == EINTR);
    if (size < 0)
        return nonblocking && (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -errno;
    if (!size)
        return -ECONNRESET;
    if (size != sizeof(*message) || message->version != ROCE_CMA_IPC_VERSION ||
        message->status < 0 || message->private_length > ROCE_CMA_PRIVATE_MAX)
        return -EPROTO;
    return 1;
}
