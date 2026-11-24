#define _GNU_SOURCE
#include "roce_lab.h"

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

bool roce_parse_uint(const char *arg, uint32_t max, uint32_t *value)
{
    if (*arg < '0' || *arg > '9')
        return false;
    errno = 0;
    char *end;
    unsigned long result = strtoul(arg, &end, 10);
    if (errno || *end || result > max)
        return false;
    *value = (uint32_t)result;
    return true;
}

bool roce_lab_parse(char **argv, struct roce_lab_options *options)
{
    bool sender = strcmp(argv[1], "tx") == 0;
    if (!sender && strcmp(argv[1], "rx") != 0)
        return false;
    struct in_addr control, data;
    uint32_t size, port, pmtu;
    if (!roce_parse_uint(argv[2], INT32_MAX, &size) || !size ||
        inet_pton(AF_INET, argv[3], &control) != 1 ||
        !roce_parse_uint(argv[4], UINT16_MAX, &port) || !port ||
        inet_pton(AF_INET, argv[5], &data) != 1 || !data.s_addr ||
        !roce_parse_uint(argv[6], UINT16_MAX, &pmtu) || !roce_valid_pmtu((uint16_t)pmtu))
        return false;
    *options = (struct roce_lab_options) {
        .sender = sender, .size = size, .writes = 1, .qps = 1, .workers = 1,
        .control_ip = control.s_addr,
        .control_port = (uint16_t)port, .data_ip = ntohl(data.s_addr), .pmtu = (uint16_t)pmtu,
    };
    return true;
}

bool roce_lab_random(uint32_t *value)
{
    return getentropy(value, sizeof(*value)) == 0;
}

bool roce_lab_wait(int fd, short events)
{
    struct pollfd wait = { .fd = fd, .events = events };
    const uint64_t second = UINT64_C(1000000000);
    uint64_t now = roce_now_ns();
    uint64_t deadline = now + ROCE_LAB_TIMEOUT * second;
    int result = 0;
    while (now < deadline) {
        uint64_t remaining = deadline - now;
        const struct timespec timeout = { .tv_sec = remaining / second, .tv_nsec = remaining % second };
        result = ppoll(&wait, 1, &timeout, NULL);
        if (result >= 0 || errno != EINTR)
            break;
        now = roce_now_ns();
    }
    if (!result || (result < 0 && errno == EINTR))
        errno = ETIMEDOUT;
    return result == 1;
}

int roce_lab_open(const struct roce_lab_options *options)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons(options->control_port),
        .sin_addr.s_addr = options->control_ip,
    };
    if (options->sender) {
        if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
            goto fail;
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            if (errno != EINPROGRESS || !roce_lab_wait(fd, POLLOUT))
                goto fail;
            int error;
            socklen_t length = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
                goto fail;
            if (error) {
                errno = error;
                goto fail;
            }
        }
        if (fcntl(fd, F_SETFL, 0) < 0)
            goto fail;
    } else {
        int yes = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0 ||
            bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
            listen(fd, 1) < 0)
            goto fail;
        puts("control listener ready");
        fflush(stdout);
        if (!roce_lab_wait(fd, POLLIN))
            goto fail;
        int connected = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
        if (connected < 0)
            goto fail;
        close(fd);
        fd = connected;
    }
    struct timeval timeout = { .tv_sec = ROCE_LAB_TIMEOUT };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0)
        return fd;
fail:
    {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
    }
    return -1;
}

static bool transfer(int fd, void *value, size_t length, bool send_value)
{
    uint8_t *bytes = value;
    while (length) {
        ssize_t count = send_value ? send(fd, bytes, length, MSG_NOSIGNAL) :
                                    recv(fd, bytes, length, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        bytes += count;
        length -= (size_t)count;
    }
    return true;
}

static bool exchange_count(int fd, uint32_t count)
{
    uint32_t value = htonl(count);
    return transfer(fd, &value, sizeof(value), true) &&
           transfer(fd, &value, sizeof(value), false) && ntohl(value) == count;
}

bool roce_lab_exchange(int fd, const struct roce_lab_options *options,
                       const struct roce_lab_offer *local, struct roce_lab_offer *peer)
{
    enum { MAGIC = 0x52435731, WORDS = 10 };
    /* Keep the original record unchanged for the single-QP, single-WR peer. */
    const uint32_t more_writes = UINT32_C(0x80000000);
    const uint32_t more_qps = UINT32_C(0x40000000);
    const uint32_t more_workers = UINT32_C(0x20000000);
    uint32_t extension = (options->writes > 1 ? more_writes : 0) |
                         (options->qps > 1 ? more_qps : 0) |
                         (options->workers > 1 ? more_workers : 0);
    uint32_t values[WORDS] = {
        htonl(MAGIC), htonl(options->size), htonl(local->data.ipv4),
        htonl(local->qpn), htonl(local->psn), htonl(local->pmtu),
        htonl(local->region.rkey), htonl((uint32_t)(local->region.address >> 32)),
        htonl((uint32_t)local->region.address), htonl(extension | options->sender),
    };
    uint8_t record[sizeof(values) + ROCE_MAC_LEN];
    memcpy(record, values, sizeof(values));
    memcpy(record + sizeof(values), local->data.mac, ROCE_MAC_LEN);
    if (!transfer(fd, record, sizeof(record), true) ||
        !transfer(fd, record, sizeof(record), false))
        return false;
    memcpy(values, record, sizeof(values));
    for (size_t i = 0; i < WORDS; ++i)
        values[i] = ntohl(values[i]);
    uint64_t address = (uint64_t)values[7] << 32 | values[8];
    uint64_t bytes = (uint64_t)options->size * options->writes * options->qps;
    if (values[0] != MAGIC || values[1] != options->size || !values[2] ||
        values[2] == local->data.ipv4 || values[3] < 2 || values[3] > ROCE_PSN_MASK - options->qps ||
        values[4] > ROCE_PSN_MASK || values[5] > ROCE_MAX_PMTU ||
        !roce_valid_pmtu((uint16_t)values[5]) || bytes - 1 > UINT64_MAX - address ||
        values[9] != (extension | (uint32_t)!options->sender) || (record[sizeof(values)] & 1))
        return false;
    if (((extension & more_writes) && !exchange_count(fd, options->writes)) ||
        ((extension & more_qps) && !exchange_count(fd, options->qps)) ||
        ((extension & more_workers) && !exchange_count(fd, options->workers)))
        return false;
    *peer = (struct roce_lab_offer) {
        .data.ipv4 = values[2], .qpn = values[3], .psn = values[4], .pmtu = (uint16_t)values[5],
        .region = { .address = address, .rkey = values[6], .size = values[1] },
    };
    memcpy(peer->data.mac, record + sizeof(values), ROCE_MAC_LEN);
    return true;
}

bool roce_lab_ready(int fd)
{
    return roce_lab_send(fd, 'R') && roce_lab_receive(fd, 'R', false) == 1;
}

bool roce_lab_wait_start_gate(uint16_t gate_port, uint16_t control_port)
{
    const struct roce_lab_options options = {
        .sender = true, .control_ip = htonl(INADDR_LOOPBACK), .control_port = gate_port,
    };
    int gate = roce_lab_open(&options);
    if (gate < 0)
        return false;
    uint16_t identity = htons(control_port);
    uint64_t release;
    bool received = transfer(gate, &identity, sizeof(identity), true) &&
                    transfer(gate, &release, sizeof(release), false);
    close(gate);
    if (!received)
        return false;

    /* The gate shares the sender's CLOCK_MONOTONIC time domain. */
    release = be64toh(release);
    const uint64_t ns_per_sec = UINT64_C(1000000000);
    uint64_t now = roce_now_ns();
    if (release < now || release - now > ROCE_LAB_TIMEOUT * ns_per_sec)
        return false;
    struct timespec target = { .tv_sec = release / ns_per_sec, .tv_nsec = release % ns_per_sec };
    int error;
    do {
        error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL);
    } while (error == EINTR);
    return !error;
}

bool roce_lab_ready_after_gate(int fd, uint16_t gate_port, uint16_t control_port)
{
    return roce_lab_receive(fd, 'R', false) == 1 && roce_lab_wait_start_gate(gate_port, control_port) &&
           roce_lab_send(fd, 'R');
}

int roce_lab_receive(int fd, char expected, bool nonblocking)
{
    char message;
    ssize_t bytes;
    do {
        bytes = recv(fd, &message, 1, nonblocking ? MSG_DONTWAIT : 0);
    } while (bytes < 0 && errno == EINTR);
    if (bytes < 0 && nonblocking && errno == EAGAIN)
        return 0;
    return bytes == 1 && message == expected ? 1 : -1;
}

bool roce_lab_send(int fd, char message)
{
    return transfer(fd, &message, 1, true);
}

static uint8_t pattern(uint64_t offset)
{
    uint32_t low = (uint32_t)offset * UINT32_C(2654435761);
    uint32_t high = (uint32_t)(offset >> 32);
    return (uint8_t)((low >> 24) ^ (high * 157));
}

void roce_lab_fill(uint8_t *buffer, size_t size, bool sender)
{
    for (size_t i = 0; i < size; ++i)
        buffer[i] = sender ? pattern(i) : (uint8_t)~pattern(i);
}

bool roce_lab_verify(const uint8_t *buffer, size_t size)
{
    enum { VERIFY_BLOCK_BYTES = 4096 };
    for (size_t offset = 0; offset < size;) {
        size_t length = size - offset;
        if (length > VERIFY_BLOCK_BYTES)
            length = VERIFY_BLOCK_BYTES;

        /* Reduce every byte so the compiler can vectorize the successful path. */
        uint8_t difference = 0;
        for (size_t i = 0; i < length; ++i)
            difference |= buffer[offset + i] ^ pattern(offset + i);
        if (difference) {
            for (size_t i = 0; i < length; ++i) {
                if (buffer[offset + i] != pattern(offset + i)) {
                    fprintf(stderr, "payload mismatch at byte %zu\n", offset + i);
                    break;
                }
            }
            return false;
        }
        offset += length;
    }
    return true;
}

uint64_t roce_now_ns(void)
{
    struct timespec timestamp;
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    return (uint64_t)timestamp.tv_sec * UINT64_C(1000000000) + (uint64_t)timestamp.tv_nsec;
}
