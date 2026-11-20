#define _GNU_SOURCE
#include "mrc_port_netlink.h"

#include <errno.h>
#include <limits.h>
#include <linux/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <rdma/rdma_netlink.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

enum {
    NETLINK_SEQUENCE = 1,
    RDMA_PORT_GET = RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, RDMA_NLDEV_CMD_PORT_GET),
    RDMA_GET_REPLY = RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, RDMA_NLDEV_CMD_GET),
    RDMA_GET_CHARDEV = RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, RDMA_NLDEV_CMD_GET_CHARDEV),
};

struct netlink_reply {
    struct nlmsghdr header;
    uint8_t payload[8192];
};

struct netlink_u32_attr {
    struct nlattr header;
    uint32_t value;
};

struct rdma_port_request {
    struct nlmsghdr header;
    struct netlink_u32_attr device;
    struct netlink_u32_attr port;
};

struct rdma_uverbs_request {
    struct nlmsghdr header;
    struct netlink_u32_attr device;
    struct nlattr type;
    char type_name[NLA_ALIGN(sizeof("uverbs"))];
};

struct route_link_request {
    struct nlmsghdr header;
    struct ifinfomsg link;
};

_Static_assert(offsetof(struct netlink_reply, payload) == NLMSG_HDRLEN, "netlink reply layout");
_Static_assert(sizeof(struct rdma_port_request) == NLMSG_HDRLEN + 2 * (NLA_HDRLEN + sizeof(uint32_t)),
               "RDMA port request layout");
_Static_assert(sizeof(struct rdma_uverbs_request) == NLMSG_HDRLEN + NLA_HDRLEN + sizeof(uint32_t) +
                                                   NLA_ALIGN(NLA_HDRLEN + sizeof("uverbs")),
               "RDMA uverbs request layout");
_Static_assert(sizeof(struct route_link_request) == NLMSG_LENGTH(sizeof(struct ifinfomsg)),
               "route link request layout");

static int receive_reply(int fd, const struct nlmsghdr *request, uint16_t response_type,
                          struct netlink_reply *reply)
{
    struct sockaddr_nl sender = {0};
    struct iovec buffer = { .iov_base = reply, .iov_len = sizeof(*reply) };
    struct msghdr message = {
        .msg_name = &sender, .msg_namelen = sizeof(sender),
        .msg_iov = &buffer, .msg_iovlen = 1,
    };
    ssize_t received;
    do {
        received = recvmsg(fd, &message, 0);
    } while (received < 0 && errno == EINTR);
    if (received < 0)
        return -errno;
    if (message.msg_namelen != sizeof(sender) || sender.nl_family != AF_NETLINK ||
        sender.nl_pid || sender.nl_groups || (message.msg_flags & MSG_TRUNC) ||
        received < NLMSG_HDRLEN)
        return -EBADMSG;

    const struct nlmsghdr *header = &reply->header;
    if (header->nlmsg_seq != request->nlmsg_seq || header->nlmsg_len < NLMSG_HDRLEN ||
        header->nlmsg_len > (size_t)received ||
        ((size_t)received != header->nlmsg_len && (size_t)received != NLMSG_ALIGN(header->nlmsg_len)) ||
        (header->nlmsg_flags & NLM_F_MULTI))
        return -EBADMSG;

    if (header->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr error;
        if (header->nlmsg_len < NLMSG_LENGTH(sizeof(error)))
            return -EBADMSG;
        memcpy(&error, reply->payload, sizeof(error));
        if (error.msg.nlmsg_seq != request->nlmsg_seq || error.msg.nlmsg_type != request->nlmsg_type ||
            error.msg.nlmsg_len != request->nlmsg_len || error.error > 0 ||
            (!error.error && response_type != NLMSG_ERROR))
            return -EBADMSG;
        return error.error;
    }
    return header->nlmsg_type == response_type ? 0 : -EBADMSG;
}

static int netlink_exchange(int protocol, const struct nlmsghdr *request, uint16_t response_type,
                             struct netlink_reply *reply)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, protocol);
    if (fd < 0)
        return -errno;

    const struct timeval timeout = { .tv_sec = 1 };
    int result;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        result = -errno;
    } else {
        const struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
        ssize_t sent;
        do {
            sent = sendto(fd, request, request->nlmsg_len, 0,
                          (const struct sockaddr *)&kernel, sizeof(kernel));
        } while (sent < 0 && errno == EINTR);
        if (sent < 0)
            result = -errno;
        else if ((size_t)sent != request->nlmsg_len)
            result = -EIO;
        else
            result = receive_reply(fd, request, response_type, reply);
    }
    close(fd);
    return result;
}

static struct netlink_u32_attr rdma_attribute(uint16_t type, uint32_t value)
{
    return (struct netlink_u32_attr){
        .header = { .nla_len = NLA_HDRLEN + sizeof(value), .nla_type = type },
        .value = value,
    };
}

static int parse_rdma_u32_attributes(const struct netlink_reply *reply, const uint16_t *types,
                                     unsigned int count, uint32_t *values, unsigned int *seen)
{
    *seen = 0;
    for (size_t offset = 0, length = reply->header.nlmsg_len - NLMSG_HDRLEN; offset < length;) {
        struct nlattr attr;
        if (length - offset < NLA_HDRLEN)
            return -EBADMSG;
        memcpy(&attr, reply->payload + offset, sizeof(attr));
        if (attr.nla_len < NLA_HDRLEN || (size_t)NLA_ALIGN(attr.nla_len) > length - offset)
            return -EBADMSG;
        for (unsigned int i = 0; i < count; i++) {
            if ((attr.nla_type & NLA_TYPE_MASK) != types[i])
                continue;
            if ((*seen & (1u << i)) || attr.nla_type != types[i] ||
                attr.nla_len != NLA_HDRLEN + sizeof(values[i]))
                return -EBADMSG;
            memcpy(&values[i], reply->payload + offset + NLA_HDRLEN, sizeof(values[i]));
            *seen |= 1u << i;
            break;
        }
        offset += NLA_ALIGN(attr.nla_len);
    }
    return 0;
}

static int parse_rdma_port(const struct netlink_reply *reply, uint32_t device, uint8_t port,
                           uint32_t *ifindex)
{
    enum { DEVICE, PORT, NETDEV, COUNT };
    const uint16_t types[COUNT] = {
        RDMA_NLDEV_ATTR_DEV_INDEX, RDMA_NLDEV_ATTR_PORT_INDEX, RDMA_NLDEV_ATTR_NDEV_INDEX,
    };
    uint32_t values[COUNT] = {0};
    unsigned int seen;
    int result = parse_rdma_u32_attributes(reply, types, COUNT, values, &seen);
    if (result)
        return result;
    const unsigned int identity = (1u << DEVICE) | (1u << PORT);
    if ((seen & identity) != identity || values[DEVICE] != device || values[PORT] != port)
        return -EBADMSG;
    if (!values[NETDEV] || values[NETDEV] > INT_MAX)
        return -ENODEV;
    *ifindex = values[NETDEV];
    return 0;
}

int mrc_port_netlink_rdma_index(uint32_t dev_index, uint8_t port_num, uint32_t *ifindex)
{
    if (!port_num || !ifindex)
        return -EINVAL;
    struct rdma_port_request request = {
        .header = {
            .nlmsg_len = sizeof(request), .nlmsg_type = RDMA_PORT_GET,
            .nlmsg_flags = NLM_F_REQUEST, .nlmsg_seq = NETLINK_SEQUENCE,
        },
        .device = rdma_attribute(RDMA_NLDEV_ATTR_DEV_INDEX, dev_index),
        .port = rdma_attribute(RDMA_NLDEV_ATTR_PORT_INDEX, port_num),
    };
    struct netlink_reply reply;
    /* The kernel labels direct PORT_GET replies with the generic GET type. */
    int result = netlink_exchange(NETLINK_RDMA, &request.header, RDMA_GET_REPLY, &reply);
    return result ? result : parse_rdma_port(&reply, dev_index, port_num, ifindex);
}

int mrc_port_netlink_uverbs_driver_id(uint32_t dev_index, uint32_t *driver_id)
{
    if (!driver_id)
        return -EINVAL;
    const struct rdma_uverbs_request request = {
        .header = {
            .nlmsg_len = sizeof(request), .nlmsg_type = RDMA_GET_CHARDEV,
            .nlmsg_flags = NLM_F_REQUEST, .nlmsg_seq = NETLINK_SEQUENCE,
        },
        .device = rdma_attribute(RDMA_NLDEV_ATTR_DEV_INDEX, dev_index),
        .type = {
            .nla_len = NLA_HDRLEN + sizeof("uverbs"), .nla_type = RDMA_NLDEV_ATTR_CHARDEV_TYPE,
        },
        .type_name = "uverbs",
    };
    struct netlink_reply reply;
    int result = netlink_exchange(NETLINK_RDMA, &request.header, RDMA_GET_CHARDEV, &reply);
    if (result)
        return result;

    /* ib_uverbs_get_nl_info() sends this attribute as u32. */
    const uint16_t type = RDMA_NLDEV_ATTR_UVERBS_DRIVER_ID;
    unsigned int seen;
    uint32_t id;
    result = parse_rdma_u32_attributes(&reply, &type, 1, &id, &seen);
    if (result)
        return result;
    if (!seen)
        return -EOPNOTSUPP;
    *driver_id = id;
    return 0;
}

static int query_link(uint32_t ifindex, unsigned int required_flags, bool *set)
{
    if (!ifindex || ifindex > INT_MAX || !set)
        return -EINVAL;
    const struct route_link_request request = {
        .header = {
            .nlmsg_len = sizeof(request), .nlmsg_type = RTM_GETLINK,
            .nlmsg_flags = NLM_F_REQUEST, .nlmsg_seq = NETLINK_SEQUENCE,
        },
        .link = { .ifi_family = AF_UNSPEC, .ifi_index = (int)ifindex },
    };
    struct netlink_reply reply;
    int result = netlink_exchange(NETLINK_ROUTE, &request.header, RTM_NEWLINK, &reply);
    if (result)
        return result;
    struct ifinfomsg link;
    if (reply.header.nlmsg_len < NLMSG_LENGTH(sizeof(link)))
        return -EBADMSG;
    memcpy(&link, reply.payload, sizeof(link));
    if (link.ifi_index != (int)ifindex)
        return -EBADMSG;
    *set = (link.ifi_flags & required_flags) == required_flags;
    return 0;
}

int mrc_port_netlink_query(uint32_t ifindex, bool *enabled)
{
    return query_link(ifindex, IFF_UP, enabled);
}

int mrc_port_netlink_link_up(uint32_t ifindex, bool *up)
{
    return query_link(ifindex, IFF_UP | IFF_LOWER_UP, up);
}

int mrc_port_netlink_set(uint32_t ifindex, bool enabled)
{
    if (!ifindex || ifindex > INT_MAX)
        return -EINVAL;
    const struct route_link_request request = {
        .header = {
            .nlmsg_len = sizeof(request), .nlmsg_type = RTM_NEWLINK,
            .nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK, .nlmsg_seq = NETLINK_SEQUENCE,
        },
        .link = {
            .ifi_family = AF_UNSPEC, .ifi_index = (int)ifindex,
            .ifi_flags = enabled ? IFF_UP : 0, .ifi_change = IFF_UP,
        },
    };
    struct netlink_reply reply;
    return netlink_exchange(NETLINK_ROUTE, &request.header, NLMSG_ERROR, &reply);
}
