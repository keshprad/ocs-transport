#include "mrc_abi.h"
#include "mrc_endpoint.h"

#include <errno.h>
#include <limits.h>
#include <mrc_ctl.h>
#include <stdlib.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_pause.h>

enum {
    CONTROLLER_PORT_MASK = ((1 << PATH_COUNT) - 1) << 1,
    ENDPOINT_TX_BURST = 32,
    NANOSECONDS_PER_SECOND = 1000000000,
};

struct pending_request {
    uint64_t sent_at;
    uint16_t id;
    bool sent;
    bool answered;
};

struct endpoint_batch {
    struct pending_request *pending;
    struct mrc_ctl_ep_rsp *responses;
    uint64_t wait_started;
    uint64_t wait_cycles;
    int count;
    int received;
    uint8_t op;
    bool waiting;
};

struct mrc_abi_endpoint {
    struct mrc_abi_endpoint *next;
    struct mrc_endpoint_config config;
    union ibv_gid local_gid;
    union ibv_gid peer_gid;
    uint16_t ports[PATH_COUNT];
    struct endpoint_batch *batch;
};

/* Endpoint lists and batches are protected by the DPDK dispatch mutex. */
static struct mrc_abi_endpoint *endpoints;

static int configure_endpoint(struct mrc_abi_endpoint *endpoint, const struct mrc_dpdk_endpoint_route *route,
                               const struct mrc_dpdk_srv6_path *paths)
{
    struct mrc_endpoint_config *config = &endpoint->config;
    config->pool = route->pool;
    config->partition_key = route->pkey ? route->pkey : UINT16_MAX;
    config->peer_partition_key = route->peer_pkey ? route->peer_pkey : config->partition_key;
    config->udp_port = route->udp_port ? route->udp_port : MRC_UDP_DEFAULT_PORT;
    config->report_service_time = true;
    config->hz = rte_get_timer_hz();
    if (!config->hz)
        return EIO;
    if (!(config->partition_key & 0x7fff) || !(config->peer_partition_key & 0x7fff))
        return EINVAL;

    int result = mrc_abi_configure_addresses(config, &route->local_gid, &route->peer_gid);
    if (result)
        return result;

    struct mrc_endpoint_srv6_path srv6[PATH_COUNT] = {0};
    if (paths) {
        result = mrc_abi_prepare_srv6_paths(paths, srv6);
        if (result)
            return result;
        config->srv6 = true;
    }
    for (unsigned int i = 0; i < PATH_COUNT; i++) {
        struct mrc_endpoint_path *path = &config->paths[i];
        if (route->ports[i] >= RTE_MAX_ETHPORTS || !rte_eth_dev_is_valid_port(route->ports[i]))
            return ENODEV;
        path->port = route->ports[i];
        path->srv6 = srv6[i];
        memcpy(path->peer.addr_bytes, route->peer_macs[i], sizeof(path->peer.addr_bytes));
        if (!rte_is_valid_assigned_ether_addr(&path->peer))
            return EINVAL;
        result = -rte_eth_macaddr_get(path->port, &path->local);
        if (result)
            return result;
    }
    endpoint->local_gid = route->local_gid;
    endpoint->peer_gid = route->peer_gid;
    memcpy(endpoint->ports, route->ports, sizeof(endpoint->ports));
    return 0;
}

static bool gids_match(const union ibv_gid *a, const union ibv_gid *b)
{
    return !memcmp(a->raw, b->raw, sizeof(a->raw));
}

static bool endpoint_overlaps(const struct mrc_abi_endpoint *a, const struct mrc_abi_endpoint *b)
{
    if (a->config.udp_port != b->config.udp_port ||
        a->config.peer_partition_key != b->config.peer_partition_key ||
        !gids_match(&a->local_gid, &b->local_gid) || !gids_match(&a->peer_gid, &b->peer_gid))
        return false;
    /* The inner tuple must identify a single route even after SRv6 decapsulation. */
    for (unsigned int i = 0; i < PATH_COUNT; i++) {
        const struct mrc_endpoint_path *first = &a->config.paths[i];
        for (unsigned int j = 0; j < PATH_COUNT; j++) {
            const struct mrc_endpoint_path *second = &b->config.paths[j];
            if (first->port == second->port && rte_is_same_ether_addr(&first->peer, &second->peer) &&
                rte_is_same_ether_addr(&first->local, &second->local))
                return true;
        }
    }
    return false;
}

MRC_PUBLIC int mrc_dpdk_attach_endpoint(struct mrc_context *context, const struct mrc_dpdk_endpoint_route *route,
                                        const struct mrc_dpdk_srv6_path paths[2])
{
    if (!context || !route || !route->pool || route->ports[0] == route->ports[1])
        return EINVAL;
    struct mrc_abi_endpoint *endpoint = calloc(1, sizeof(*endpoint));
    if (!endpoint)
        return ENOMEM;
    int result = mrc_abi_retain_context(context);
    if (result) {
        free(endpoint);
        return result;
    }

    pthread_mutex_lock(&mrc_abi_dispatch_mutex);
    result = context->endpoint ? EBUSY : configure_endpoint(endpoint, route, paths);
    for (struct mrc_abi_endpoint *other = endpoints; !result && other; other = other->next) {
        if (endpoint_overlaps(endpoint, other))
            result = EADDRINUSE;
    }
    if (!result) {
        endpoint->next = endpoints;
        endpoints = endpoint;
        context->endpoint = endpoint;
    }
    pthread_mutex_unlock(&mrc_abi_dispatch_mutex);
    if (result) {
        mrc_abi_release_context(context);
        free(endpoint);
    }
    return result;
}

MRC_PUBLIC int mrc_dpdk_progress_endpoint(struct mrc_context *context)
{
    if (!context)
        return EINVAL;
    pthread_mutex_lock(&mrc_abi_dispatch_mutex);
    struct mrc_abi_endpoint *endpoint = context->endpoint;
    bool attached = endpoint != NULL;
    if (endpoint)
        mrc_abi_poll_endpoint_ports(endpoint->ports);
    pthread_mutex_unlock(&mrc_abi_dispatch_mutex);
    return attached ? 0 : ENODEV;
}

MRC_PUBLIC int mrc_dpdk_detach_endpoint(struct mrc_context *context)
{
    if (!context)
        return EINVAL;
    pthread_mutex_lock(&mrc_abi_dispatch_mutex);
    struct mrc_abi_endpoint *endpoint = context->endpoint;
    int result = 0;
    if (!endpoint) {
        result = ENODEV;
    } else if (endpoint->batch) {
        result = EBUSY;
    } else {
        struct mrc_abi_endpoint **entry = &endpoints;
        while (*entry != endpoint)
            entry = &(*entry)->next;
        *entry = endpoint->next;
        context->endpoint = NULL;
    }
    pthread_mutex_unlock(&mrc_abi_dispatch_mutex);
    if (!result) {
        free(endpoint);
        mrc_abi_release_context(context);
    }
    return result;
}

static uint32_t numeric_ev(const uint8_t value[MRC_CTL_EV_MAX_BYTES])
{
    return (uint32_t)value[0] << 24 | (uint32_t)value[1] << 16 | (uint32_t)value[2] << 8 | value[3];
}

static bool zero_octets(const uint8_t *value, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (value[i])
            return false;
    }
    return true;
}

static bool has_srh(const uint8_t value[MRC_CTL_EV_MAX_BYTES])
{
    return !zero_octets(value + MRC_IPV6_ADDR_LEN, MRC_IPV6_ADDR_LEN);
}

static int validate_request(const struct mrc_abi_endpoint *endpoint, const struct mrc_ctl_ep_req *request,
                            uint8_t op)
{
    if (request->req_ev.port < 1 || request->req_ev.port > PATH_COUNT ||
        !gids_match(&request->sgid, &endpoint->local_gid) || !gids_match(&request->dgid, &endpoint->peer_gid))
        return EINVAL;
    if (op == MRC_ENDPOINT_PORT_STATUS &&
        (request->op.port_status_update.port_status & ~(uint32_t)CONTROLLER_PORT_MASK))
        return EINVAL;

    const uint8_t *value = request->req_ev.val;
    if (request->ev_fmt_mode == MRC_CTL_EV_FMT_MODE_SRV6) {
        enum mrc_ctl_store_format format = has_srh(value) ? MRC_CTL_STORE_FORMAT_SRV6_SRH :
                                                          MRC_CTL_STORE_FORMAT_SRV6;
        return endpoint->config.srv6 && mrc_ctl_store_srv6_ev_valid(format, value) ? 0 : EINVAL;
    }
    if (request->ev_fmt_mode == MRC_CTL_EV_FMT_MODE_STEV) {
        if (endpoint->config.ip_version != MRC_WIRE_IP6)
            return EINVAL;
    } else if (request->ev_fmt_mode != MRC_CTL_EV_FMT_MODE_UDP || value[0] || value[1]) {
        return EINVAL;
    }
    return (value[2] || value[3]) && zero_octets(value + 4, MRC_CTL_EV_MAX_BYTES - 4) ? 0 : EINVAL;
}

static int compare_requests(const void *lhs, const void *rhs)
{
    const struct pending_request *a = lhs, *b = rhs;
    return (int)a->id - (int)b->id;
}

static struct pending_request *find_request(const struct endpoint_batch *batch, uint16_t id)
{
    const struct pending_request key = { .id = id };
    return bsearch(&key, batch->pending, (size_t)batch->count, sizeof(key), compare_requests);
}

bool mrc_abi_endpoint_port_token_in_use(uint16_t token)
{
    for (const struct mrc_abi_endpoint *endpoint = endpoints; endpoint; endpoint = endpoint->next) {
        const struct endpoint_batch *batch = endpoint->batch;
        if (batch && batch->op == MRC_ENDPOINT_PORT_STATUS && find_request(batch, token))
            return true;
    }
    return false;
}

static bool send_request(const struct mrc_abi_endpoint *endpoint, const struct mrc_ctl_ep_req *request,
                           struct pending_request *pending, uint8_t op, uint8_t dscp)
{
    unsigned int path = request->req_ev.port - 1;
    struct mrc_endpoint_config config = endpoint->config;
    const uint8_t *value = request->req_ev.val;
    config.srv6 = request->ev_fmt_mode == MRC_CTL_EV_FMT_MODE_SRV6;
    if (config.srv6) {
        struct mrc_endpoint_srv6_path *route = &config.paths[path].srv6;
        const uint8_t *segment = value + MRC_IPV6_ADDR_LEN;
        memcpy(route->initial_dst, value, sizeof(route->initial_dst));
        memcpy(route->segments[0], segment, sizeof(route->segments[0]));
        route->segment_count = has_srh(value);
        route->segments_left = route->segment_count && memcmp(value, segment, MRC_IPV6_ADDR_LEN) != 0;
    }

    struct mrc_wire_packet packet = mrc_endpoint_packet_for(&config, path, MRC_ENDPOINT_REQUEST, request->req_id);
    packet.body.endpoint_request.op = op;
    /* Controller port bits start at 1. ERTH starts with the first rail at bit 0. */
    if (op == MRC_ENDPOINT_PORT_STATUS)
        packet.body.endpoint_request.port_status_mask = request->op.port_status_update.port_status >> 1;
    else if (config.ip_version == MRC_WIRE_IP6)
        packet.ipv6.dscp = dscp;
    else
        packet.ipv4.dscp = dscp;

    if (!config.srv6) {
        uint32_t ev = numeric_ev(value);
        if (config.ip_version == MRC_WIRE_IP6) {
            packet.ipv6.src_port = (uint16_t)ev;
            packet.ipv6.flow_label = (packet.ipv6.flow_label & ~UINT32_C(0xffff)) | (ev >> 16);
        } else {
            packet.ipv4.src_port = (uint16_t)ev;
        }
    }

    pending->sent_at = rte_get_timer_cycles();
    pending->sent = mrc_endpoint_transmit(&config, path, &packet);
    return pending->sent;
}

static bool receive_response(struct mrc_abi_endpoint *endpoint, unsigned int path,
                             const struct mrc_wire_packet *packet, uint64_t arrival_cycles)
{
    struct endpoint_batch *batch = endpoint->batch;
    if (!batch || batch->op != packet->body.endpoint_response.op)
        return false;
    uint16_t id = (uint16_t)packet->bth.psn;
    struct pending_request *pending = find_request(batch, id);
    if (!pending)
        return false;
    if (!pending->sent || pending->answered ||
        (batch->waiting && arrival_cycles - batch->wait_started > batch->wait_cycles))
        return true;

    __uint128_t rtt = (__uint128_t)(arrival_cycles - pending->sent_at) * NANOSECONDS_PER_SECOND /
                      endpoint->config.hz;
    uint64_t service = packet->body.endpoint_response.tx_timestamp * MRC_CC_TIMESTAMP_UNIT_NS;
    bool adjusted = service && service < rtt;
    if (adjusted)
        rtt -= service;
    batch->responses[batch->received++] = (struct mrc_ctl_ep_rsp){
        .req_id = id,
        .port = (uint8_t)(path + 1),
        .rtt = rtt > UINT_MAX ? UINT_MAX : (unsigned int)rtt,
        .adj_svc_time = adjusted,
    };
    pending->answered = true;
    return true;
}

bool mrc_abi_dispatch_endpoint(uint16_t port, const struct mrc_wire_packet *packet,
                               const struct mrc_wire_srv6 *routing, uint64_t arrival_cycles)
{
    if (packet->bth.opcode != MRC_ENDPOINT_REQUEST && packet->bth.opcode != MRC_ENDPOINT_RESPONSE)
        return false;
    for (struct mrc_abi_endpoint *endpoint = endpoints; endpoint; endpoint = endpoint->next) {
        const struct mrc_endpoint_config *config = &endpoint->config;
        for (unsigned int path = 0; path < PATH_COUNT; path++) {
            if (config->paths[path].port != port || !mrc_endpoint_peer_matches(config, path, packet) ||
                !mrc_endpoint_route_matches(config, path, routing))
                continue;
            if (packet->bth.opcode == MRC_ENDPOINT_RESPONSE)
                return receive_response(endpoint, path, packet, arrival_cycles);

            const struct mrc_wire_endpoint_request *request = &packet->body.endpoint_request;
            struct mrc_wire_packet response = mrc_endpoint_packet_for(config, path, MRC_ENDPOINT_RESPONSE,
                                                                      (uint16_t)packet->bth.psn);
            response.body.endpoint_response.op = request->op;
            response.body.endpoint_response.tx_timestamp = mrc_endpoint_feedback_timestamp(
                config, arrival_cycles, request->timestamp, request->tsr, request->tx_timestamp);
            mrc_endpoint_transmit(config, path, &response);
            return true;
        }
    }
    return false;
}

static int run_batch(struct mrc_abi_endpoint *endpoint, struct endpoint_batch *batch,
                     const struct mrc_ctl_ep_req *requests, uint8_t dscp, uint32_t timeout)
{
    for (int i = 0; i < batch->count; i++) {
        int error = validate_request(endpoint, &requests[i], batch->op);
        if (error)
            return error;
        uint16_t id = requests[i].req_id;
        if (batch->op == MRC_ENDPOINT_PORT_STATUS &&
            (mrc_abi_qp_port_token_in_use(id) || mrc_abi_endpoint_port_token_in_use(id)))
            return EAGAIN;
    }

    endpoint->batch = batch;
    int sent = 0;
    for (int i = 0; i < batch->count; i++) {
        struct pending_request *pending = find_request(batch, requests[i].req_id);
        sent += send_request(endpoint, &requests[i], pending, batch->op, dscp);
        if ((i + 1) % ENDPOINT_TX_BURST == 0 && i + 1 < batch->count) {
            mrc_abi_poll_endpoint_ports(endpoint->ports);
            pthread_mutex_unlock(&mrc_abi_dispatch_mutex);
            pthread_mutex_lock(&mrc_abi_dispatch_mutex);
        }
    }

    batch->wait_started = rte_get_timer_cycles();
    batch->wait_cycles = mrc_endpoint_cycles(&endpoint->config, timeout);
    if (timeout && !batch->wait_cycles)
        batch->wait_cycles = 1;
    batch->waiting = true;
    while (batch->received < sent && rte_get_timer_cycles() - batch->wait_started < batch->wait_cycles) {
        mrc_abi_poll_endpoint_ports(endpoint->ports);
        if (batch->received == sent)
            break;
        pthread_mutex_unlock(&mrc_abi_dispatch_mutex);
        rte_pause();
        pthread_mutex_lock(&mrc_abi_dispatch_mutex);
    }
    endpoint->batch = NULL;
    if (sent < batch->count)
        return EAGAIN;
    return batch->received < sent ? ETIMEDOUT : 0;
}

static int endpoint_error(int error)
{
    errno = error;
    return -1;
}

MRC_PUBLIC int mrc_ctl_ep_batch_send_wait(struct mrc_context *context, uint8_t req_tc,
                                           enum mrc_ctl_ep_op_type op_type, struct mrc_ctl_ep_req *req,
                                           int num_req, uint32_t rsp_timeout, struct mrc_ctl_ep_rsp *rsp,
                                           int *num_rsp)
{
    if (!num_rsp)
        return endpoint_error(EINVAL);
    *num_rsp = 0;
    if (!context || req_tc > 63 || num_req < 0 || num_req > (int)UINT16_MAX + 1 ||
        (num_req && (!req || !rsp)))
        return endpoint_error(EINVAL);
    if (op_type != MRC_CTL_EP_OP_EV_PROBE && op_type != MRC_CTL_EP_OP_PORT_STATUS_UPDATE)
        return endpoint_error(ENOTSUP);
    if (!mrc_abi_controller_capable())
        return endpoint_error(EPERM);
    if (!num_req)
        return 0;

    struct endpoint_batch batch = {
        .pending = calloc((size_t)num_req, sizeof(*batch.pending)),
        .responses = rsp,
        .count = num_req,
        .op = op_type == MRC_CTL_EP_OP_EV_PROBE ? MRC_ENDPOINT_EV_PROBE : MRC_ENDPOINT_PORT_STATUS,
    };
    if (!batch.pending)
        return endpoint_error(ENOMEM);
    for (int i = 0; i < num_req; i++)
        batch.pending[i].id = req[i].req_id;
    qsort(batch.pending, (size_t)num_req, sizeof(*batch.pending), compare_requests);
    for (int i = 1; i < num_req; i++) {
        if (batch.pending[i - 1].id == batch.pending[i].id) {
            free(batch.pending);
            return endpoint_error(EINVAL);
        }
    }

    pthread_mutex_lock(&mrc_abi_dispatch_mutex);
    struct mrc_abi_endpoint *endpoint = context->endpoint;
    int result = endpoint ? EAGAIN : ENODEV;
    if (endpoint && !endpoint->batch)
        result = run_batch(endpoint, &batch, req, req_tc, rsp_timeout);
    pthread_mutex_unlock(&mrc_abi_dispatch_mutex);
    free(batch.pending);
    *num_rsp = batch.received;
    return result ? endpoint_error(result) : 0;
}
