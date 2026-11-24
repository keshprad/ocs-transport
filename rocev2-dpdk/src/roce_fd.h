#ifndef ROCE_FD_H
#define ROCE_FD_H

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static inline bool roce_send_fd(int socket, void *bytes, size_t length, int fd)
{
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(fd))];
    } control = {0};
    struct iovec data = { .iov_base = bytes, .iov_len = length };
    struct msghdr message = {
        .msg_iov = &data, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes),
    };
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(header), &fd, sizeof(fd));

    ssize_t sent;
    do {
        sent = sendmsg(socket, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent >= 0 && sent != (ssize_t)length)
        errno = EPROTO;
    return sent == (ssize_t)length;
}

static inline bool roce_receive_optional_fd(int socket, void *bytes, size_t length, int *fd_out)
{
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int))];
    } control = {0};
    struct iovec data = { .iov_base = bytes, .iov_len = length };
    struct msghdr message = {
        .msg_iov = &data, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes),
    };
    ssize_t received;
    do {
        received = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);
    if (received < 0)
        return false;
    int descriptor = -1;
    unsigned int count = 0;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(0))
            continue;
        size_t size = header->cmsg_len - CMSG_LEN(0);
        for (size_t offset = 0; offset + sizeof(int) <= size; offset += sizeof(int)) {
            int fd;
            memcpy(&fd, CMSG_DATA(header) + offset, sizeof(fd));
            if (++count == 1)
                descriptor = fd;
            else
                close(fd);
        }
    }
    if (received == (ssize_t)length && count <= 1 && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
        *fd_out = descriptor;
        return true;
    }
    if (descriptor >= 0)
        close(descriptor);
    errno = received == 0 ? ECONNRESET : EPROTO;
    return false;
}

static inline int roce_receive_fd(int socket, void *bytes, size_t length)
{
    int fd;
    if (!roce_receive_optional_fd(socket, bytes, length, &fd))
        return -1;
    if (fd >= 0)
        return fd;
    errno = EPROTO;
    return -1;
}

#endif
