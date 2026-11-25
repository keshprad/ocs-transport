#define _GNU_SOURCE
#include "mrc_oob.h"

#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ms(void)
{
    struct timespec clock;
    clock_gettime(CLOCK_MONOTONIC, &clock);
    return (uint64_t)clock.tv_sec * 1000 + (uint64_t)clock.tv_nsec / 1000000;
}

static int wait_for(int fd, short events, uint64_t deadline)
{
    for (;;) {
        uint64_t now = monotonic_ms();
        if (now >= deadline)
            return -ETIMEDOUT;
        uint64_t remaining = deadline - now;
        int timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
        struct pollfd pollfd = { .fd = fd, .events = events };
        int result = poll(&pollfd, 1, timeout);
        if (result > 0)
            return 0; /* The next socket operation reports error or EOF. */
        if (result == 0)
            return -ETIMEDOUT;
        if (errno != EINTR)
            return -errno;
    }
}

static int open_socket(const struct addrinfo *address)
{
    int fd = socket(address->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    return fd >= 0 ? fd : -errno;
}

static int open_listener(const struct addrinfo *address, uint64_t deadline)
{
    int listener = open_socket(address);
    if (listener < 0)
        return listener;

    int result;
    if (bind(listener, address->ai_addr, address->ai_addrlen) || listen(listener, 1)) {
        result = -errno;
    } else {
        for (;;) {
            result = wait_for(listener, POLLIN, deadline);
            if (result)
                break;
            result = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (result >= 0)
                break;
            if (errno != EINTR && errno != EAGAIN) {
                result = -errno;
                break;
            }
        }
    }
    close(listener);
    return result;
}

static int open_client(const struct addrinfo *address, uint64_t deadline)
{
    for (;;) {
        int fd = open_socket(address);
        if (fd < 0)
            return fd;

        int result = connect(fd, address->ai_addr, address->ai_addrlen);
        if (!result)
            return fd;
        result = -errno;
        if (result == -EINPROGRESS || result == -EINTR) {
            result = wait_for(fd, POLLOUT, deadline);
            if (!result) {
                int error;
                socklen_t length = sizeof(error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length))
                    result = -errno;
                else if (!error)
                    return fd;
                else
                    result = -error;
            }
        }
        close(fd);
        if (result != -ECONNREFUSED)
            return result;
        if (monotonic_ms() >= deadline)
            return -ETIMEDOUT;
        const struct timespec pause = { .tv_nsec = 10000000 };
        nanosleep(&pause, NULL);
    }
}

static int transfer_record(int fd, uint8_t record[MRC_CONNECT_RECORD_SIZE],
                            bool send_mode, uint64_t deadline)
{
    size_t offset = 0;
    while (offset < MRC_CONNECT_RECORD_SIZE) {
        int result = wait_for(fd, send_mode ? POLLOUT : POLLIN, deadline);
        if (result)
            return result;
        size_t remaining = MRC_CONNECT_RECORD_SIZE - offset;
        ssize_t bytes = send_mode ? send(fd, record + offset, remaining, MSG_NOSIGNAL)
                                  : recv(fd, record + offset, remaining, 0);
        if (bytes > 0)
            offset += (size_t)bytes;
        else if (bytes == 0)
            return -ECONNRESET;
        else if (errno != EINTR && errno != EAGAIN)
            return -errno;
    }
    return 0;
}

int mrc_oob_exchange(const char *address, uint16_t port, bool listen_mode,
                     unsigned int timeout_ms, const struct mrc_connect_offer *local,
                     struct mrc_connect_qp *qp)
{
    if (!address || !port || !timeout_ms || !local || !qp)
        return -EINVAL;
    uint8_t record[MRC_CONNECT_RECORD_SIZE];
    int result = mrc_connect_encode(record, sizeof(record), local);
    if (result)
        return result;

    struct addrinfo *resolved;
    const struct addrinfo hints = {
        .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV,
    };
    char service[6];
    snprintf(service, sizeof(service), "%u", port);
    if (getaddrinfo(address, service, &hints, &resolved))
        return -EINVAL;
    uint64_t deadline = monotonic_ms() + timeout_ms;
    int fd = listen_mode ? open_listener(resolved, deadline) : open_client(resolved, deadline);
    freeaddrinfo(resolved);
    if (fd < 0)
        return fd;

    result = transfer_record(fd, record, true, deadline);
    if (!result)
        result = transfer_record(fd, record, false, deadline);
    close(fd);
    if (result)
        return result;

    struct mrc_connect_offer peer;
    result = mrc_connect_decode(record, sizeof(record), &peer);
    return result ? result : mrc_connect_negotiate(local, &peer, qp);
}
