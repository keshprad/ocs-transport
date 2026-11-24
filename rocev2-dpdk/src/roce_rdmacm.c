#define _GNU_SOURCE

#include "roce_cma_ipc.h"
#include "roce_mr_ipc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

enum cm_state {
    CM_IDLE,
    CM_BOUND,
    CM_ADDR_PENDING,
    CM_ADDR_READY,
    CM_ROUTE_PENDING,
    CM_ROUTE_READY,
    CM_LISTENING,
    CM_REQUEST,
    CM_CONNECTING,
    CM_RESPONSE,
    CM_ESTABLISHED,
    CM_DISCONNECTING,
    CM_CLOSED,
};

struct cm_id {
    struct rdma_cm_id public;
    enum cm_state state;
    unsigned events;
    bool destroying;
};

struct cm_channel {
    struct rdma_event_channel public;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    struct cm_id *primary;
    struct cm_id *child;
    bool started;
    bool reading;
    bool stopped;
};

struct cm_event {
    struct rdma_cm_event public;
    struct cm_id *id;
    struct cm_id *listener;
    uint8_t private_data[ROCE_CMA_PRIVATE_MAX];
};

static int fail(int error)
{
    errno = error;
    return -1;
}

static struct cm_channel *cm_channel(struct rdma_event_channel *channel)
{
    return (struct cm_channel *)channel;
}

static struct cm_id *cm_id(struct rdma_cm_id *id)
{
    return (struct cm_id *)id;
}

static bool valid_qpn(uint32_t qp_num)
{
    return qp_num > 1 && qp_num < UINT32_C(0xffffff);
}

struct rdma_event_channel *rdma_create_event_channel(void)
{
    const char *path = secure_getenv("ROCE_DPDK_CM_SOCKET");
    if (!path || !*path) {
        errno = ENODEV;
        return NULL;
    }
    struct cm_channel *channel = calloc(1, sizeof(*channel));
    if (!channel)
        return NULL;

    int error = pthread_mutex_init(&channel->mutex, NULL);
    if (error)
        goto free_channel;
    error = pthread_cond_init(&channel->changed, NULL);
    if (error)
        goto destroy_mutex;
    int fd = roce_mr_ipc_connect(path);
    if (fd < 0) {
        error = errno;
        goto destroy_condition;
    }
    /* Keep the public event channel blocking unless the caller requests O_NONBLOCK. */
    const struct timeval no_timeout = {0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout)) < 0) {
        error = errno;
        close(fd);
        goto destroy_condition;
    }
    channel->public.fd = fd;
    return &channel->public;

destroy_condition:
    pthread_cond_destroy(&channel->changed);
destroy_mutex:
    pthread_mutex_destroy(&channel->mutex);
free_channel:
    free(channel);
    errno = error;
    return NULL;
}

static void stop_channel(struct cm_channel *channel)
{
    if (!channel->stopped) {
        channel->stopped = true;
        shutdown(channel->public.fd, SHUT_RDWR);
    }
}

void rdma_destroy_event_channel(struct rdma_event_channel *public)
{
    if (!public)
        return;
    struct cm_channel *channel = cm_channel(public);
    pthread_mutex_lock(&channel->mutex);
    if (channel->primary || channel->child) {
        pthread_mutex_unlock(&channel->mutex);
        errno = EBUSY;
        return;
    }
    stop_channel(channel);
    while (channel->reading)
        pthread_cond_wait(&channel->changed, &channel->mutex);
    pthread_mutex_unlock(&channel->mutex);

    close(public->fd);
    pthread_cond_destroy(&channel->changed);
    pthread_mutex_destroy(&channel->mutex);
    free(channel);
}

static struct cm_id *new_id(struct cm_channel *channel, void *context)
{
    struct cm_id *id = calloc(1, sizeof(*id));
    if (id) {
        id->public = (struct rdma_cm_id) {
            .channel = &channel->public, .context = context,
            .ps = RDMA_PS_TCP, .qp_type = IBV_QPT_RC,
        };
    }
    return id;
}

int rdma_create_id(struct rdma_event_channel *public, struct rdma_cm_id **out, void *context,
                   enum rdma_port_space ps)
{
    if (!out)
        return fail(EINVAL);
    if (!public || ps != RDMA_PS_TCP)
        return fail(EOPNOTSUPP);

    struct cm_channel *channel = cm_channel(public);
    pthread_mutex_lock(&channel->mutex);
    int error = 0;
    struct cm_id *id = NULL;
    if (channel->stopped || channel->primary || channel->child)
        error = EBUSY;
    else if (!(id = new_id(channel, context)))
        error = ENOMEM;
    else
        channel->primary = id;
    pthread_mutex_unlock(&channel->mutex);
    if (error)
        return fail(error);
    *out = &id->public;
    return 0;
}

int rdma_destroy_id(struct rdma_cm_id *public)
{
    if (!public || !public->channel)
        return fail(EINVAL);
    struct cm_id *id = cm_id(public);
    struct cm_channel *channel = cm_channel(public->channel);
    pthread_mutex_lock(&channel->mutex);
    if (id->destroying) {
        pthread_mutex_unlock(&channel->mutex);
        return fail(EBUSY);
    }
    id->destroying = true;
    if (channel->child == id || (!channel->child && (channel->started || channel->reading)))
        stop_channel(channel);
    while (id->events)
        pthread_cond_wait(&channel->changed, &channel->mutex);
    if (channel->child == id)
        channel->child = NULL;
    else
        channel->primary = NULL;
    pthread_mutex_unlock(&channel->mutex);
    free(id);
    return 0;
}

static const struct sockaddr_in *ipv4_address(const struct sockaddr *address)
{
    return address && address->sa_family == AF_INET ? (const struct sockaddr_in *)address : NULL;
}

static struct roce_cma_message command(uint32_t type, const struct sockaddr_in *local,
                                       const struct sockaddr_in *peer)
{
    return (struct roce_cma_message) {
        .version = ROCE_CMA_IPC_VERSION, .type = type,
        .local_ipv4 = ntohl(local->sin_addr.s_addr), .peer_ipv4 = ntohl(peer->sin_addr.s_addr),
        .local_port = ntohs(local->sin_port), .peer_port = ntohs(peer->sin_port),
    };
}

static int send_command(struct cm_channel *channel, struct cm_id *id,
                        const struct roce_cma_message *message, enum cm_state next)
{
    if (id->destroying || channel->stopped)
        return ESHUTDOWN;
    int error = roce_cma_send(channel->public.fd, message);
    if (!error) {
        channel->started = true;
        id->state = next;
    }
    return error;
}

int rdma_bind_addr(struct rdma_cm_id *public, struct sockaddr *address)
{
    if (!public || !public->channel || !address)
        return fail(EINVAL);
    const struct sockaddr_in *ipv4 = ipv4_address(address);
    if (!ipv4)
        return fail(EAFNOSUPPORT);
    struct cm_id *id = cm_id(public);
    struct cm_channel *channel = cm_channel(public->channel);
    pthread_mutex_lock(&channel->mutex);
    int error = id->state != CM_IDLE || id->destroying || channel->stopped ? EINVAL : 0;
    if (!error) {
        id->state = CM_BOUND;
        public->route.addr.src_sin = *ipv4;
    }
    pthread_mutex_unlock(&channel->mutex);
    return error ? fail(error) : 0;
}

int rdma_resolve_addr(struct rdma_cm_id *public, struct sockaddr *src_addr, struct sockaddr *dst_addr,
                      int timeout_ms)
{
    if (!public || !public->channel || !dst_addr || timeout_ms < 0)
        return fail(EINVAL);
    const struct sockaddr_in *local = src_addr ? ipv4_address(src_addr) : &public->route.addr.src_sin;
    const struct sockaddr_in *peer = ipv4_address(dst_addr);
    if (!local || !peer)
        return fail(EAFNOSUPPORT);
    if (!peer->sin_addr.s_addr || !peer->sin_port)
        return fail(EINVAL);

    struct cm_id *id = cm_id(public);
    struct cm_channel *channel = cm_channel(public->channel);
    pthread_mutex_lock(&channel->mutex);
    struct roce_cma_message message = command(ROCE_CMA_RESOLVE_ADDR, local, peer);
    int error = id->state == CM_IDLE || id->state == CM_BOUND ?
        send_command(channel, id, &message, CM_ADDR_PENDING) : EINVAL;
    if (!error) {
        public->route.addr.src_sin = *local;
        public->route.addr.src_sin.sin_family = AF_INET;
        public->route.addr.dst_sin = *peer;
    }
    pthread_mutex_unlock(&channel->mutex);
    return error ? fail(error) : 0;
}

static int simple_command(struct rdma_cm_id *public, uint32_t type, enum cm_state from, enum cm_state to)
{
    if (!public || !public->channel)
        return fail(EINVAL);
    struct cm_id *id = cm_id(public);
    struct cm_channel *channel = cm_channel(public->channel);
    pthread_mutex_lock(&channel->mutex);
    struct roce_cma_message message = command(type, &public->route.addr.src_sin, &public->route.addr.dst_sin);
    int error = id->state == from ? send_command(channel, id, &message, to) : EINVAL;
    pthread_mutex_unlock(&channel->mutex);
    return error ? fail(error) : 0;
}

int rdma_resolve_route(struct rdma_cm_id *id, int timeout_ms)
{
    return timeout_ms < 0 ? fail(EINVAL) :
        simple_command(id, ROCE_CMA_RESOLVE_ROUTE, CM_ADDR_READY, CM_ROUTE_PENDING);
}

int rdma_listen(struct rdma_cm_id *id, int backlog)
{
    return backlog < 0 ? fail(EINVAL) : simple_command(id, ROCE_CMA_LISTEN, CM_BOUND, CM_LISTENING);
}

static int connection_command(struct rdma_cm_id *public, const struct rdma_conn_param *params,
                              uint32_t type, enum cm_state from)
{
    if (!public || !public->channel || !params)
        return fail(EINVAL);
    if (public->qp || params->responder_resources || params->initiator_depth || params->srq)
        return fail(EOPNOTSUPP);
    if (params->private_data_len > ROCE_CMA_PRIVATE_MAX || (params->private_data_len && !params->private_data))
        return fail(EINVAL);

    struct cm_id *id = cm_id(public);
    struct cm_channel *channel = cm_channel(public->channel);
    pthread_mutex_lock(&channel->mutex);
    struct roce_cma_message message = command(type, &public->route.addr.src_sin, &public->route.addr.dst_sin);
    message.qp_num = params->qp_num;
    if (params->private_data_len) {
        message.private_length = params->private_data_len;
        memcpy(message.private_data, params->private_data, message.private_length);
    }
    int error = id->state == from && valid_qpn(message.qp_num) ?
        send_command(channel, id, &message, CM_CONNECTING) : EINVAL;
    pthread_mutex_unlock(&channel->mutex);
    return error ? fail(error) : 0;
}

int rdma_connect(struct rdma_cm_id *id, struct rdma_conn_param *params)
{
    return connection_command(id, params, ROCE_CMA_CONNECT, CM_ROUTE_READY);
}

int rdma_accept(struct rdma_cm_id *id, struct rdma_conn_param *params)
{
    return connection_command(id, params, ROCE_CMA_ACCEPT, CM_REQUEST);
}

int rdma_establish(struct rdma_cm_id *id)
{
    if (!id || id->qp)
        return fail(EINVAL);
    return simple_command(id, ROCE_CMA_ESTABLISH, CM_RESPONSE, CM_ESTABLISHED);
}

int rdma_disconnect(struct rdma_cm_id *public)
{
    if (!public || !public->channel)
        return fail(EINVAL);
    struct cm_id *id = cm_id(public);
    struct cm_channel *channel = cm_channel(public->channel);
    pthread_mutex_lock(&channel->mutex);
    int error = 0;
    if (id->destroying) {
        error = ESHUTDOWN;
    } else if (id->state == CM_ESTABLISHED) {
        struct roce_cma_message message = command(ROCE_CMA_DISCONNECT, &public->route.addr.src_sin,
                                                  &public->route.addr.dst_sin);
        error = send_command(channel, id, &message, CM_DISCONNECTING);
    } else if (id->state != CM_DISCONNECTING && id->state != CM_CLOSED) {
        error = EINVAL;
    }
    pthread_mutex_unlock(&channel->mutex);
    return error ? fail(error) : 0;
}

static void update_address(struct sockaddr_in *address, uint32_t ipv4, uint16_t port)
{
    address->sin_family = AF_INET;
    if (ipv4)
        address->sin_addr.s_addr = htonl(ipv4);
    if (port)
        address->sin_port = htons(port);
}

static void update_route(struct rdma_cm_id *id, const struct roce_cma_message *message)
{
    update_address(&id->route.addr.src_sin, message->local_ipv4, message->local_port);
    update_address(&id->route.addr.dst_sin, message->peer_ipv4, message->peer_port);
    id->port_num = 1;
}

static void prepare_event(struct cm_event *event, struct cm_id *id, struct cm_id *listener,
                          enum rdma_cm_event_type type, const struct roce_cma_message *message)
{
    event->id = id;
    event->listener = listener;
    event->public.id = &id->public;
    event->public.event = type;
    event->public.status = -message->status;
    if (type == RDMA_CM_EVENT_CONNECT_REQUEST || type == RDMA_CM_EVENT_CONNECT_RESPONSE ||
        type == RDMA_CM_EVENT_ESTABLISHED ||
        type == RDMA_CM_EVENT_REJECTED) {
        event->public.param.conn.qp_num = message->qp_num;
        event->public.param.conn.private_data_len = message->private_length;
        if (message->private_length) {
            memcpy(event->private_data, message->private_data, message->private_length);
            event->public.param.conn.private_data = event->private_data;
        }
    }
    ++id->events;
    if (listener) {
        event->public.listen_id = &listener->public;
        ++listener->events;
    }
}

static int connect_request(struct cm_channel *channel, struct cm_event *event,
                           const struct roce_cma_message *message)
{
    struct cm_id *listener = channel->primary;
    if (channel->child || !listener || listener->destroying || listener->state != CM_LISTENING ||
        message->status || !valid_qpn(message->qp_num) || !message->peer_ipv4 || !message->peer_port)
        return EPROTO;
    struct cm_id *child = new_id(channel, listener->public.context);
    if (!child) {
        stop_channel(channel);
        return ENOMEM;
    }
    child->state = CM_REQUEST;
    child->public.route.addr.src_sin = listener->public.route.addr.src_sin;
    update_route(&child->public, message);
    channel->child = child;
    prepare_event(event, child, listener, RDMA_CM_EVENT_CONNECT_REQUEST, message);
    return 0;
}

static int dispatch_event(struct cm_channel *channel, struct cm_event *event,
                          const struct roce_cma_message *message)
{
    if (message->type == ROCE_CMA_CONNECT_REQUEST)
        return connect_request(channel, event, message);
    struct cm_id *id = channel->child ? channel->child : channel->primary;
    if (!id || id->destroying)
        return ECANCELED;

    enum rdma_cm_event_type type;
    enum cm_state next;
    switch (message->type) {
    case ROCE_CMA_ADDR_RESOLVED:
    case ROCE_CMA_ADDR_ERROR:
        if (id->state != CM_ADDR_PENDING || (message->type == ROCE_CMA_ADDR_ERROR) != (message->status != 0))
            return EPROTO;
        type = message->status ? RDMA_CM_EVENT_ADDR_ERROR : RDMA_CM_EVENT_ADDR_RESOLVED;
        next = message->status ? CM_BOUND : CM_ADDR_READY;
        if (!message->status)
            update_route(&id->public, message);
        break;
    case ROCE_CMA_ROUTE_RESOLVED:
    case ROCE_CMA_ROUTE_ERROR:
        if (id->state != CM_ROUTE_PENDING || (message->type == ROCE_CMA_ROUTE_ERROR) != (message->status != 0))
            return EPROTO;
        type = message->status ? RDMA_CM_EVENT_ROUTE_ERROR : RDMA_CM_EVENT_ROUTE_RESOLVED;
        next = message->status ? CM_ADDR_READY : CM_ROUTE_READY;
        if (!message->status)
            update_route(&id->public, message);
        break;
    case ROCE_CMA_CONNECT_RESPONSE:
        if (id == channel->child || id->state != CM_CONNECTING || message->status || !valid_qpn(message->qp_num))
            return EPROTO;
        type = RDMA_CM_EVENT_CONNECT_RESPONSE;
        next = CM_RESPONSE;
        update_route(&id->public, message);
        break;
    case ROCE_CMA_ESTABLISHED:
        if (id != channel->child || id->state != CM_CONNECTING || message->status || !valid_qpn(message->qp_num))
            return EPROTO;
        type = RDMA_CM_EVENT_ESTABLISHED;
        next = CM_ESTABLISHED;
        update_route(&id->public, message);
        break;
    case ROCE_CMA_REJECTED:
    case ROCE_CMA_CONNECT_ERROR:
        if ((id->state != CM_CONNECTING && id->state != CM_RESPONSE && id->state != CM_REQUEST &&
             id->state != CM_LISTENING) || !message->status)
            return EPROTO;
        type = message->type == ROCE_CMA_REJECTED ? RDMA_CM_EVENT_REJECTED : RDMA_CM_EVENT_CONNECT_ERROR;
        next = CM_CLOSED;
        break;
    case ROCE_CMA_DISCONNECTED:
        if (id->state != CM_ESTABLISHED && id->state != CM_DISCONNECTING &&
            id->state != CM_CONNECTING && id->state != CM_REQUEST)
            return EPROTO;
        type = RDMA_CM_EVENT_DISCONNECTED;
        next = CM_CLOSED;
        break;
    default:
        return EPROTO;
    }
    id->state = next;
    prepare_event(event, id, NULL, type, message);
    return 0;
}

int rdma_get_cm_event(struct rdma_event_channel *public, struct rdma_cm_event **out)
{
    if (!public || !out)
        return fail(EINVAL);
    struct cm_event *event = calloc(1, sizeof(*event));
    if (!event)
        return fail(ENOMEM);

    struct cm_channel *channel = cm_channel(public);
    pthread_mutex_lock(&channel->mutex);
    int error = 0;
    if (channel->stopped)
        error = ECANCELED;
    else if (channel->reading)
        error = EBUSY;
    else if (!channel->primary && !channel->child)
        error = EINVAL;
    if (!error) {
        channel->reading = true;
        pthread_mutex_unlock(&channel->mutex);

        struct roce_cma_message message;
        int received = roce_cma_receive(public->fd, &message, false);

        pthread_mutex_lock(&channel->mutex);
        channel->reading = false;
        pthread_cond_broadcast(&channel->changed);
        if (channel->stopped)
            error = ECANCELED;
        else if (received == 1)
            error = dispatch_event(channel, event, &message);
        else
            error = received < 0 ? -received : EAGAIN;
    }
    pthread_mutex_unlock(&channel->mutex);
    if (error) {
        free(event);
        return fail(error);
    }
    *out = &event->public;
    return 0;
}

int rdma_ack_cm_event(struct rdma_cm_event *public)
{
    if (!public)
        return fail(EINVAL);
    struct cm_event *event = (struct cm_event *)public;
    struct cm_channel *channel = cm_channel(event->id->public.channel);
    pthread_mutex_lock(&channel->mutex);
    --event->id->events;
    if (event->listener)
        --event->listener->events;
    pthread_cond_broadcast(&channel->changed);
    pthread_mutex_unlock(&channel->mutex);
    free(event);
    return 0;
}
