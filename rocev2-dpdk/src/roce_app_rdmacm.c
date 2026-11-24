#include "roce_app_rdmacm.h"
#include "roce_lab.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <rdma/rdma_cma.h>
#include <string.h>

enum { CM_TIMEOUT_MS = ROCE_LAB_TIMEOUT * 1000 };

static const char request_private[] = "roce-dpdk-client\0v1";
static const char reply_private[] = "roce-dpdk-server\0v1";
_Static_assert(sizeof(request_private) <= ROCE_CMA_PRIVATE_MAX, "CM request private data fits");
_Static_assert(sizeof(reply_private) <= ROCE_CMA_PRIVATE_MAX, "CM reply private data fits");

static struct sockaddr_in ipv4_address(uint32_t ip, uint16_t port)
{
    return (struct sockaddr_in) {
        .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(ip),
    };
}

static bool match_addresses(struct rdma_cm_id *id, const struct roce_cma_lab_address *expected,
                            bool sender)
{
    const struct sockaddr_in *local = (const struct sockaddr_in *)rdma_get_local_addr(id);
    const struct sockaddr_in *peer = (const struct sockaddr_in *)rdma_get_peer_addr(id);
    return local->sin_family == AF_INET && peer->sin_family == AF_INET &&
           local->sin_addr.s_addr == htonl(expected->local_ipv4) &&
           peer->sin_addr.s_addr == htonl(expected->peer_ipv4) &&
           (sender ? peer : local)->sin_port == htons(expected->service_port);
}

static bool next_event(struct rdma_event_channel *channel, uint64_t deadline, struct rdma_cm_event **event)
{
    const uint64_t ns_per_ms = UINT64_C(1000000);
    for (;;) {
        const uint64_t now = roce_now_ns();
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return false;
        }
        int ms = (int)((deadline - now + ns_per_ms - 1) / ns_per_ms);
        struct pollfd ready = { .fd = channel->fd, .events = POLLIN };
        int result = poll(&ready, 1, ms);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (!result)
            continue;
        if (!(ready.revents & POLLIN)) {
            errno = ready.revents & POLLNVAL ? EBADF : ECONNRESET;
            return false;
        }
        if (!rdma_get_cm_event(channel, event))
            return true;
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return false;
    }
}

static bool match_private(const struct rdma_cm_event *event, const char *data, size_t length)
{
    return event->param.conn.private_data_len == length && event->param.conn.private_data &&
           !memcmp(event->param.conn.private_data, data, length);
}

static bool expect_event(struct roce_app_rdmacm *cm, enum rdma_cm_event_type expected,
                         const struct roce_cma_lab_address *address, bool sender, uint64_t deadline)
{
    struct rdma_cm_event *event;
    if (!next_event(cm->channel, deadline, &event))
        return false;

    struct rdma_cm_id *unexpected_child = NULL;
    if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST && cm->listener &&
        event->listen_id == cm->listener && event->id && event->id != cm->listener) {
        if (!cm->id)
            cm->id = event->id;
        else if (cm->id != event->id)
            unexpected_child = event->id;
    }
    int error = EPROTO;
    if (event->status >= -4095 && event->status <= 4095 && event->status) {
        error = event->status < 0 ? -event->status : event->status;
    } else if (!event->status && event->event == expected && cm->id && event->id == cm->id &&
               (!address || match_addresses(event->id, address, sender))) {
        if (expected == RDMA_CM_EVENT_CONNECT_REQUEST) {
            if (event->listen_id == cm->listener && match_private(event, request_private, sizeof(request_private)))
                error = 0;
        } else if (expected != RDMA_CM_EVENT_CONNECT_RESPONSE ||
                   match_private(event, reply_private, sizeof(reply_private))) {
            error = 0;
        }
    }
    if (rdma_ack_cm_event(event) && !error)
        error = errno ? errno : EIO;
    if (unexpected_child)
        rdma_destroy_id(unexpected_child);
    if (error)
        errno = error;
    return !error;
}

static bool connect_active(struct roce_app_rdmacm *cm, const struct roce_cma_lab_address *address,
                           uint64_t deadline)
{
    struct sockaddr_in local = ipv4_address(address->local_ipv4, 0);
    struct sockaddr_in peer = ipv4_address(address->peer_ipv4, address->service_port);
    struct rdma_conn_param parameters = {
        .private_data = request_private, .private_data_len = sizeof(request_private),
        .qp_num = address->local_qpn, .retry_count = ROCE_LAB_RETRIES,
    };
    return !rdma_create_id(cm->channel, &cm->id, NULL, RDMA_PS_TCP) &&
           !rdma_resolve_addr(cm->id, (struct sockaddr *)&local, (struct sockaddr *)&peer, CM_TIMEOUT_MS) &&
           expect_event(cm, RDMA_CM_EVENT_ADDR_RESOLVED, address, true, deadline) &&
           !rdma_resolve_route(cm->id, CM_TIMEOUT_MS) &&
           expect_event(cm, RDMA_CM_EVENT_ROUTE_RESOLVED, address, true, deadline) &&
           !rdma_connect(cm->id, &parameters) &&
           expect_event(cm, RDMA_CM_EVENT_CONNECT_RESPONSE, address, true, deadline) &&
           !rdma_establish(cm->id);
}

static bool connect_passive(struct roce_app_rdmacm *cm, const struct roce_cma_lab_address *address,
                            uint64_t deadline)
{
    struct sockaddr_in local = ipv4_address(address->local_ipv4, address->service_port);
    struct rdma_conn_param parameters = {
        .private_data = reply_private, .private_data_len = sizeof(reply_private), .qp_num = address->local_qpn,
    };
    return !rdma_create_id(cm->channel, &cm->listener, NULL, RDMA_PS_TCP) &&
           !rdma_bind_addr(cm->listener, (struct sockaddr *)&local) && !rdma_listen(cm->listener, 1) &&
           expect_event(cm, RDMA_CM_EVENT_CONNECT_REQUEST, address, false, deadline) &&
           !rdma_accept(cm->id, &parameters) &&
           expect_event(cm, RDMA_CM_EVENT_ESTABLISHED, address, false, deadline);
}

static uint64_t event_deadline(void)
{
    return roce_now_ns() + ROCE_LAB_TIMEOUT * UINT64_C(1000000000);
}

bool roce_app_rdmacm_connect(struct roce_app_rdmacm *cm, const struct roce_cma_lab_address *address,
                             bool sender)
{
    if (!cm || !address || !address->local_ipv4 || !address->peer_ipv4 || !address->service_port ||
        address->reserved || address->local_qpn <= 1 || address->local_qpn >= ROCE_PSN_MASK) {
        errno = EINVAL;
        return false;
    }
    if (cm->channel || cm->id || cm->listener) {
        errno = EALREADY;
        return false;
    }
    cm->channel = rdma_create_event_channel();
    if (!cm->channel)
        return false;

    int flags = fcntl(cm->channel->fd, F_GETFL);
    uint64_t deadline = event_deadline();
    if (flags >= 0 && !fcntl(cm->channel->fd, F_SETFL, flags | O_NONBLOCK) &&
        (sender ? connect_active(cm, address, deadline) : connect_passive(cm, address, deadline)))
        return true;
    int error = errno ? errno : EIO;
    roce_app_rdmacm_destroy(cm);
    errno = error;
    return false;
}

int roce_app_rdmacm_disconnect(void *opaque)
{
    struct roce_app_rdmacm *cm = opaque;
    if (!cm || !cm->id)
        return EINVAL;
    if (cm->disconnect_started || cm->disconnected)
        return 0;
    if (rdma_disconnect(cm->id))
        return errno ? errno : EIO;
    cm->disconnect_started = true;
    return 0;
}

bool roce_app_rdmacm_finish(struct roce_app_rdmacm *cm)
{
    if (!cm || !cm->id) {
        errno = EINVAL;
        return false;
    }
    if (!cm->disconnected)
        cm->disconnected = expect_event(cm, RDMA_CM_EVENT_DISCONNECTED, NULL, false, event_deadline());
    return cm->disconnected;
}

void roce_app_rdmacm_destroy(struct roce_app_rdmacm *cm)
{
    if (!cm)
        return;
    if (cm->id)
        rdma_destroy_id(cm->id);
    if (cm->listener)
        rdma_destroy_id(cm->listener);
    if (cm->channel)
        rdma_destroy_event_channel(cm->channel);
    *cm = (struct roce_app_rdmacm) {0};
}
