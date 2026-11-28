#include "roce_cma_ipc.h"
#include "roce_fd.h"

#include <errno.h>
#include <stddef.h>
#include <sys/socket.h>

enum { PRIVATE_OFFSET = offsetof(struct roce_cma_message, private_data) };
enum { LEGACY_RECORD_SIZE = PRIVATE_OFFSET + ROCE_CMA_PRIVATE_MAX + 3 };
_Static_assert(LEGACY_RECORD_SIZE == 56, "Preserve the original CM record layout");

static size_t record_size(const struct roce_cma_message *message)
{
    return roce_cma_is_rejection(message->type) ? PRIVATE_OFFSET + ROCE_CM_REJ_PRIVATE_SIZE :
                                                 LEGACY_RECORD_SIZE;
}

int roce_cma_send(int socket, const struct roce_cma_message *message)
{
    size_t length = record_size(message);
    ssize_t size;
    do {
        size = send(socket, message, length, MSG_NOSIGNAL);
    } while (size < 0 && errno == EINTR);
    if (size < 0)
        return errno;
    return size == (ssize_t)length ? 0 : EPROTO;
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
    return roce_send_fd(socket, message, record_size(message), fd) ? 0 : errno;
}

int roce_cma_receive_fd(int socket, struct roce_cma_message *message, bool nonblocking, int *fd)
{
    *fd = -1;
    memset(message, 0, sizeof(*message));
    ssize_t length = roce_receive_record_fd(socket, message, sizeof(*message), fd,
                                            nonblocking ? MSG_DONTWAIT : 0);
    if (length < 0)
        return nonblocking && (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -errno;
    if (length >= PRIVATE_OFFSET && length == (ssize_t)record_size(message)) {
        uint8_t allowed_flags = message->type == ROCE_CMA_CONNECT || message->type == ROCE_CMA_ACCEPT ?
                                ROCE_CMA_QP_OWNED : 0;
        bool reject = roce_cma_is_rejection(message->type);
        const uint8_t *reserved = message->private_data + ROCE_CMA_PRIVATE_MAX;
        size_t private_max = reject ? ROCE_CM_REJ_PRIVATE_SIZE : ROCE_CMA_PRIVATE_MAX;
        if (message->version == ROCE_CMA_IPC_VERSION && message->status >= 0 &&
            message->private_length <= private_max && !(message->flags & ~allowed_flags) &&
            (reject || (!reserved[0] && !reserved[1] && !reserved[2])) &&
            (*fd < 0 || message->type == ROCE_CMA_CONFIGURE_QP || message->type == ROCE_CMA_CONNECT_RESPONSE))
            return 1;
    }
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
    return -EPROTO;
}
