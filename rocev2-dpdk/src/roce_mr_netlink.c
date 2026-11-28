#define _POSIX_C_SOURCE 200809L
#include "roce_mr_netlink.h"

#include <errno.h>
#include <linux/genetlink.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { CTRL_SEQUENCE = 1, CTRL_TIMEOUT_MS = 2000, GENL_HEADERS = NLMSG_HDRLEN + GENL_HDRLEN };

struct netlink_attribute {
    uint16_t type;
    size_t length;
    const uint8_t *data;
};

static bool next_attribute(struct netlink_attribute *attribute, const uint8_t **cursor, size_t *remaining)
{
    struct nlattr header;
    if (*remaining < NLA_HDRLEN)
        return false;
    memcpy(&header, *cursor, sizeof(header));
    size_t aligned = NLA_ALIGN(header.nla_len);
    if (header.nla_len < NLA_HDRLEN || aligned > *remaining)
        return false;

    *attribute = (struct netlink_attribute) {
        .type = header.nla_type,
        .length = header.nla_len - NLA_HDRLEN,
        .data = *cursor + NLA_HDRLEN,
    };
    *cursor += aligned;
    *remaining -= aligned;
    return true;
}

static ssize_t receive_kernel(int fd, uint8_t *data, size_t capacity)
{
    struct sockaddr_nl source = {0};
    struct iovec iov = { .iov_base = data, .iov_len = capacity };
    struct msghdr message = {
        .msg_name = &source, .msg_namelen = sizeof(source), .msg_iov = &iov, .msg_iovlen = 1,
    };
    ssize_t length;
    do {
        length = recvmsg(fd, &message, MSG_DONTWAIT);
    } while (length < 0 && errno == EINTR);
    if (length < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -errno;
    if (message.msg_namelen != sizeof(source) || source.nl_family != AF_NETLINK ||
        source.nl_pid != 0 || source.nl_groups != 0)
        return 0;
    if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))
        return -EMSGSIZE;
    return length ? length : -EPROTO;
}

/* The controller's single-family reply and the driver's unicast ticket each
 * occupy one netlink message in their datagram. */
static bool read_header(struct nlmsghdr *header, const uint8_t *data, size_t length)
{
    if (length < NLMSG_HDRLEN)
        return false;
    memcpy(header, data, sizeof(*header));
    return header->nlmsg_len >= NLMSG_HDRLEN && header->nlmsg_len <= length &&
           length == NLMSG_ALIGN(header->nlmsg_len) &&
           !(header->nlmsg_flags & (NLM_F_REQUEST | NLM_F_MULTI));
}

static int parse_family(uint16_t *family, const uint8_t *cursor, size_t remaining)
{
    const char name[] = ROCE_MR_GENL_NAME;
    const uint32_t expected_version = ROCE_MR_GENL_VERSION;
    const uint32_t no_extra_header = 0;
    uint16_t id = 0;
    bool have_name = false, have_version = false, have_header_size = false;

    while (remaining) {
        struct netlink_attribute attribute;
        if (!next_attribute(&attribute, &cursor, &remaining))
            return EPROTO;
        switch (attribute.type & NLA_TYPE_MASK) {
        case CTRL_ATTR_FAMILY_ID:
            if (id || attribute.type != CTRL_ATTR_FAMILY_ID || attribute.length != sizeof(id))
                return EPROTO;
            memcpy(&id, attribute.data, sizeof(id));
            if (id < GENL_START_ALLOC || id > GENL_MAX_ID)
                return EPROTO;
            break;
        case CTRL_ATTR_FAMILY_NAME:
            if (have_name || attribute.type != CTRL_ATTR_FAMILY_NAME || attribute.length != sizeof(name) ||
                memcmp(attribute.data, name, sizeof(name)))
                return EPROTO;
            have_name = true;
            break;
        case CTRL_ATTR_VERSION:
            if (have_version || attribute.type != CTRL_ATTR_VERSION ||
                attribute.length != sizeof(expected_version))
                return EPROTO;
            if (memcmp(attribute.data, &expected_version, sizeof(expected_version)))
                return EPROTONOSUPPORT;
            have_version = true;
            break;
        case CTRL_ATTR_HDRSIZE:
            if (have_header_size || attribute.type != CTRL_ATTR_HDRSIZE ||
                attribute.length != sizeof(no_extra_header) ||
                memcmp(attribute.data, &no_extra_header, sizeof(no_extra_header)))
                return EPROTO;
            have_header_size = true;
            break;
        }
    }
    if (!id || !have_name || !have_version || !have_header_size)
        return EPROTO;
    *family = id;
    return 0;
}

static int control_error(const uint8_t *data, size_t length)
{
    struct nlmsgerr reply;
    if (length < NLMSG_HDRLEN + sizeof(reply))
        return EPROTO;
    memcpy(&reply, data + NLMSG_HDRLEN, sizeof(reply));
    if (reply.msg.nlmsg_type != GENL_ID_CTRL || reply.msg.nlmsg_seq != CTRL_SEQUENCE ||
        reply.error >= 0 || reply.error < -4095)
        return EPROTO;
    return -reply.error;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -errno;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int receive_family(int fd, uint16_t *family)
{
    int64_t start = monotonic_ms();
    if (start < 0)
        return (int)-start;
    int64_t deadline = start + CTRL_TIMEOUT_MS;

    for (;;) {
        int64_t now = monotonic_ms();
        if (now < 0)
            return (int)-now;
        if (now >= deadline)
            return ETIMEDOUT;
        struct pollfd socket = { .fd = fd, .events = POLLIN };
        int ready = poll(&socket, 1, (int)(deadline - now));
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        if (!ready)
            return ETIMEDOUT;
        if (socket.revents & POLLNVAL)
            return EBADF;
        if (socket.revents & POLLHUP)
            return ECONNRESET;

        uint8_t data[4096];
        ssize_t length = receive_kernel(fd, data, sizeof(data));
        if (length < 0)
            return (int)-length;
        if (!length)
            continue;
        struct nlmsghdr header;
        if (!read_header(&header, data, (size_t)length))
            return EPROTO;
        if (header.nlmsg_type == NLMSG_OVERRUN)
            return ENOBUFS;
        if (header.nlmsg_seq != CTRL_SEQUENCE)
            continue;
        if (header.nlmsg_type == NLMSG_ERROR)
            return control_error(data, header.nlmsg_len);
        if (header.nlmsg_type != GENL_ID_CTRL || header.nlmsg_len < GENL_HEADERS)
            return EPROTO;
        struct genlmsghdr generic;
        memcpy(&generic, data + NLMSG_HDRLEN, sizeof(generic));
        if (generic.cmd != CTRL_CMD_NEWFAMILY || generic.reserved)
            return EPROTO;
        return parse_family(family, data + GENL_HEADERS, header.nlmsg_len - GENL_HEADERS);
    }
}

static int resolve_family(int fd, uint32_t portid, uint16_t *family)
{
    enum { NAME_SIZE = sizeof(ROCE_MR_GENL_NAME), REQUEST_SIZE = GENL_HEADERS + NLA_HDRLEN + NLA_ALIGN(NAME_SIZE) };
    const struct {
        struct nlmsghdr header;
        struct genlmsghdr generic;
        struct nlattr attribute;
        char name[NLA_ALIGN(NAME_SIZE)];
    } request = {
        .header = { .nlmsg_len = REQUEST_SIZE, .nlmsg_type = GENL_ID_CTRL,
                    .nlmsg_flags = NLM_F_REQUEST, .nlmsg_seq = CTRL_SEQUENCE, .nlmsg_pid = portid },
        .generic = { .cmd = CTRL_CMD_GETFAMILY, .version = 1 },
        .attribute = { .nla_len = NLA_HDRLEN + NAME_SIZE, .nla_type = CTRL_ATTR_FAMILY_NAME },
        .name = ROCE_MR_GENL_NAME,
    };
    _Static_assert(sizeof(request) == REQUEST_SIZE, "Unexpected Netlink control request layout");
    const struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
    ssize_t written;
    do {
        written = sendto(fd, &request, sizeof(request), 0, (const struct sockaddr *)&kernel, sizeof(kernel));
    } while (written < 0 && errno == EINTR);
    if (written < 0)
        return errno;
    if (written != sizeof(request))
        return EIO;
    return receive_family(fd, family);
}

int roce_mr_netlink_open(struct roce_mr_netlink *netlink)
{
    if (!netlink)
        return EINVAL;
    *netlink = (struct roce_mr_netlink) { .fd = -1 };

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_GENERIC);
    if (fd < 0)
        return errno;
    struct sockaddr_nl address = { .nl_family = AF_NETLINK };
    socklen_t length = sizeof(address);
    uint16_t family;
    int error;
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0 ||
        getsockname(fd, (struct sockaddr *)&address, &length) < 0) {
        error = errno;
    } else if (length != sizeof(address) || address.nl_family != AF_NETLINK || !address.nl_pid) {
        error = EPROTO;
    } else {
        error = resolve_family(fd, address.nl_pid, &family);
    }
    if (error) {
        close(fd);
        return error;
    }

    *netlink = (struct roce_mr_netlink) { .fd = fd, .portid = address.nl_pid, .family = family };
    return 0;
}

static int parse_ticket(struct roce_mr_ticket_notification *notification, const uint8_t *cursor, size_t remaining)
{
    struct roce_mr_ticket_notification result = {0};
    bool have_ticket = false, have_cookie = false;
    while (remaining) {
        struct netlink_attribute attribute;
        if (!next_attribute(&attribute, &cursor, &remaining))
            return -EPROTO;
        switch (attribute.type & NLA_TYPE_MASK) {
        case ROCE_MR_GENL_A_TICKET:
            if (have_ticket || attribute.type != ROCE_MR_GENL_A_TICKET ||
                attribute.length != sizeof(result.ticket))
                return -EPROTO;
            memcpy(&result.ticket, attribute.data, sizeof(result.ticket));
            have_ticket = true;
            break;
        case ROCE_MR_GENL_A_COOKIE:
            if (have_cookie || attribute.type != ROCE_MR_GENL_A_COOKIE ||
                attribute.length != sizeof(result.cookie))
                return -EPROTO;
            memcpy(&result.cookie, attribute.data, sizeof(result.cookie));
            have_cookie = true;
            break;
        }
    }
    if (!have_ticket || !have_cookie)
        return -EPROTO;
    *notification = result;
    return 1;
}

int roce_mr_netlink_receive(const struct roce_mr_netlink *netlink,
                            struct roce_mr_ticket_notification *notification)
{
    if (!netlink || !notification)
        return -EINVAL;
    if (netlink->fd < 0)
        return -EBADF;

    uint8_t data[256];
    ssize_t length = receive_kernel(netlink->fd, data, sizeof(data));
    if (length <= 0)
        return (int)length;
    struct nlmsghdr header;
    if (!read_header(&header, data, (size_t)length))
        return -EPROTO;
    if (header.nlmsg_type == NLMSG_OVERRUN)
        return -ENOBUFS;
    if (header.nlmsg_type != netlink->family || header.nlmsg_seq != 0 || header.nlmsg_len < GENL_HEADERS)
        return -EPROTO;

    struct genlmsghdr generic;
    memcpy(&generic, data + NLMSG_HDRLEN, sizeof(generic));
    if (generic.cmd != ROCE_MR_GENL_CMD_TICKET || generic.version != ROCE_MR_GENL_VERSION || generic.reserved)
        return -EPROTO;
    return parse_ticket(notification, data + GENL_HEADERS, header.nlmsg_len - GENL_HEADERS);
}
