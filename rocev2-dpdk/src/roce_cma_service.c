#define _GNU_SOURCE
#include "roce_cma_service.h"
#include "roce_lab.h"
#include "roce_mr_ipc.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

bool roce_cma_service_listen(struct roce_cma_service *service, const char *path)
{
    service->listener = roce_mr_ipc_listen(path);
    if (service->listener < 0)
        return false;
    service->path = path;
    return true;
}

static bool same_peer(int cm_socket, int app_socket)
{
    struct ucred cm, app;
    socklen_t cm_size = sizeof(cm), app_size = sizeof(app);
    if (getsockopt(cm_socket, SOL_SOCKET, SO_PEERCRED, &cm, &cm_size) ||
        getsockopt(app_socket, SOL_SOCKET, SO_PEERCRED, &app, &app_size))
        return false;
    if (cm_size != sizeof(cm) || app_size != sizeof(app) || cm.pid <= 0 ||
        cm.pid != app.pid || cm.uid != app.uid || cm.uid != geteuid()) {
        errno = EACCES;
        return false;
    }
    return true;
}

bool roce_cma_service_attach(struct roce_cma_service *service, int app_socket,
                             const struct roce_cma_lab_address *address, uint32_t local_qpn, bool sender)
{
    if (send(app_socket, address, sizeof(*address), MSG_NOSIGNAL) != sizeof(*address))
        return false;
    struct pollfd wait = { .fd = service->listener, .events = POLLIN };
    int ready;
    do {
        ready = poll(&wait, 1, ROCE_LAB_TIMEOUT * 1000);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0) {
        if (!ready)
            errno = ETIMEDOUT;
        return false;
    }
    service->socket = accept4(service->listener, NULL, NULL, SOCK_CLOEXEC);
    if (service->socket < 0 || !same_peer(service->socket, app_socket))
        return false;
    const struct timeval timeout = { .tv_sec = ROCE_LAB_TIMEOUT };
    if (setsockopt(service->socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(service->socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)))
        return false;

    service->address = *address;
    service->sender = sender;
    service->local_qpn = local_qpn;
    service->local_port = sender ? UINT16_C(0xc000) | (local_qpn & UINT16_C(0x3fff)) : address->service_port;
    service->peer_port = sender ? address->service_port : 0;
    return true;
}

bool roce_cma_service_event(const struct roce_cma_service *service, enum roce_cma_message_type type,
                            int status, const uint8_t *private_data, uint8_t private_length)
{
    if (private_length > ROCE_CMA_PRIVATE_MAX || (private_length && !private_data)) {
        errno = EINVAL;
        return false;
    }
    struct roce_cma_message message = {
        .version = ROCE_CMA_IPC_VERSION, .type = type, .status = status,
        .local_ipv4 = service->address.local_ipv4, .peer_ipv4 = service->address.peer_ipv4,
        .local_port = service->local_port, .peer_port = service->peer_port,
        .qp_num = service->peer_qpn, .private_length = private_length,
    };
    if (private_length)
        memcpy(message.private_data, private_data, private_length);
    int error = roce_cma_send(service->socket, &message);
    if (error)
        errno = error;
    return !error;
}

static bool command(const struct roce_cma_service *service, struct roce_cma_message *message,
                    enum roce_cma_message_type expected)
{
    int received = roce_cma_receive(service->socket, message, false);
    if (received != 1) {
        errno = received < 0 ? -received : ETIMEDOUT;
        return false;
    }
    if (message->type != (uint32_t)expected || message->status) {
        errno = EPROTO;
        return false;
    }
    return true;
}

static bool matches_peer(const struct roce_cma_service *service, const struct roce_cma_message *message)
{
    return message->local_ipv4 == service->address.local_ipv4 && message->local_port == service->local_port &&
           message->peer_ipv4 == service->address.peer_ipv4 && message->peer_port == service->peer_port;
}

bool roce_cma_service_prepare(struct roce_cma_service *service)
{
    struct roce_cma_message message;
    if (!service->sender) {
        if (!command(service, &message, ROCE_CMA_LISTEN))
            return false;
        if (message.local_ipv4 == service->address.local_ipv4 && message.local_port == service->local_port)
            return true;
        errno = EADDRNOTAVAIL;
        return false;
    }

    if (!command(service, &message, ROCE_CMA_RESOLVE_ADDR))
        return false;
    if ((message.local_ipv4 && message.local_ipv4 != service->address.local_ipv4) ||
        message.peer_ipv4 != service->address.peer_ipv4 || message.peer_port != service->peer_port) {
        roce_cma_service_event(service, ROCE_CMA_ADDR_ERROR, EHOSTUNREACH, NULL, 0);
        errno = EHOSTUNREACH;
        return false;
    }
    if (message.local_port)
        service->local_port = message.local_port;
    if (!roce_cma_service_event(service, ROCE_CMA_ADDR_RESOLVED, 0, NULL, 0) ||
        !command(service, &message, ROCE_CMA_RESOLVE_ROUTE))
        return false;
    if (!matches_peer(service, &message)) {
        roce_cma_service_event(service, ROCE_CMA_ROUTE_ERROR, EHOSTUNREACH, NULL, 0);
        errno = EHOSTUNREACH;
        return false;
    }
    if (!roce_cma_service_event(service, ROCE_CMA_ROUTE_RESOLVED, 0, NULL, 0) ||
        !command(service, &message, ROCE_CMA_CONNECT))
        return false;
    if (!matches_peer(service, &message) || message.qp_num != service->local_qpn) {
        errno = EINVAL;
        return false;
    }
    service->app_parameters = message;
    return true;
}

bool roce_cma_service_request(struct roce_cma_service *service, uint16_t peer_port, uint32_t peer_qpn,
                              const uint8_t *private_data, uint8_t private_length)
{
    service->peer_port = peer_port;
    service->peer_qpn = peer_qpn;
    struct roce_cma_message message;
    if (!roce_cma_service_event(service, ROCE_CMA_CONNECT_REQUEST, 0, private_data, private_length) ||
        !command(service, &message, ROCE_CMA_ACCEPT))
        return false;
    if (!matches_peer(service, &message) || message.qp_num != service->local_qpn) {
        errno = EINVAL;
        return false;
    }
    service->app_parameters = message;
    return true;
}

bool roce_cma_service_response(const struct roce_cma_service *service,
                               const uint8_t *private_data, uint8_t private_length)
{
    struct roce_cma_message message;
    if (!roce_cma_service_event(service, ROCE_CMA_CONNECT_RESPONSE, 0, private_data, private_length) ||
        !command(service, &message, ROCE_CMA_ESTABLISH))
        return false;
    if (matches_peer(service, &message))
        return true;
    errno = EINVAL;
    return false;
}

bool roce_cma_service_wait_disconnect(const struct roce_cma_service *service)
{
    if (!service->sender)
        return true;
    struct roce_cma_message message;
    if (!command(service, &message, ROCE_CMA_DISCONNECT))
        return false;
    if (matches_peer(service, &message))
        return true;
    errno = EINVAL;
    return false;
}

void roce_cma_service_close(struct roce_cma_service *service)
{
    if (service->socket >= 0)
        close(service->socket);
    if (service->listener >= 0)
        close(service->listener);
    if (service->path)
        unlink(service->path);
}
