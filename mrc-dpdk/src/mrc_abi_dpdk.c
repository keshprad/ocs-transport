#include "mrc_abi.h"
#include "mrc_endpoint.h"
#include "mrc_qp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

enum {
    RX_BURST = 32,
    RX_PENDING = 128,
    ROUTE_BUCKETS = 256,
};

struct rx_queue {
    struct mrc_endpoint_rx_packet packets[RX_PENDING];
    uint16_t head;
    uint16_t count;
};

struct qp_direction {
    struct mrc_endpoint_config config;
    struct rx_queue ingress[PATH_COUNT];
};

struct peer_node {
    struct mrc_endpoint_node state;
    size_t users;
};

struct mrc_abi_route {
    struct mrc_abi_route *next;
    struct mrc_abi_route *next_qpn;
    struct qp_direction tx;
    struct qp_direction rx;
    struct peer_node *node;
    struct mrc_endpoint_counters counts;
    struct mrc_requester *request;
    struct mrc_responder *response;
    uint32_t qpn;
    int error;
};

/* Shared ethdev RX and TX queue I/O, route lifetime, and node state are
 * serialized here. QP callers hold their own QP mutex. The dispatcher
 * does not acquire QP or context mutexes. */
pthread_mutex_t mrc_abi_dispatch_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct mrc_abi_route *routes;
static struct mrc_abi_route *routes_by_qpn[ROUTE_BUCKETS];

static uint16_t take_packets(void *context, unsigned int path,
                              struct mrc_endpoint_rx_packet *packets, uint16_t limit)
{
    struct qp_direction *direction = context;
    struct rx_queue *queue = &direction->ingress[path];
    uint16_t count = queue->count < limit ? queue->count : limit;
    for (uint16_t i = 0; i < count; i++)
        packets[i] = queue->packets[(queue->head + i) % RX_PENDING];
    queue->head = (queue->head + count) % RX_PENDING;
    queue->count -= count;
    return count;
}

static void discard_packets(struct qp_direction *direction)
{
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        struct mrc_endpoint_rx_packet packets[RX_PENDING];
        uint16_t count = take_packets(direction, path, packets, RX_PENDING);
        for (uint16_t i = 0; i < count; i++)
            rte_pktmbuf_free(packets[i].mbuf);
    }
}

static int matched_path(const struct mrc_endpoint_config *config, uint16_t port,
                         const struct mrc_wire_packet *header, const struct mrc_wire_srv6 *routing)
{
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        if (config->paths[path].port != port)
            continue;
        return mrc_endpoint_peer_matches(config, path, header) &&
               mrc_endpoint_route_matches(config, path, routing) ? (int)path : -1;
    }
    return -1;
}

static bool queue_packet(struct qp_direction *direction, unsigned int path,
                          struct mrc_endpoint_rx_packet packet)
{
    struct rx_queue *queue = &direction->ingress[path];
    if (queue->count == RX_PENDING)
        return false;
    queue->packets[(queue->head + queue->count) % RX_PENDING] = packet;
    queue->count++;
    return true;
}

static struct qp_direction *node_direction(struct mrc_abi_route *route)
{
    return route->request ? &route->tx : &route->rx;
}

static bool route_node_packet(struct mrc_abi_route *route, uint16_t port,
                               const struct mrc_wire_packet *packet, const struct mrc_wire_srv6 *routing,
                               struct mrc_endpoint_rx_packet received, bool srv6, bool *matched)
{
    struct qp_direction *direction = node_direction(route);
    if (direction->config.srv6 != srv6)
        return false;
    int path = matched_path(&direction->config, port, packet, routing);
    if (path < 0)
        return false;
    if (matched)
        *matched = true;
    if (packet->bth.opcode == MRC_ENDPOINT_REQUEST &&
        packet->body.endpoint_request.op == MRC_ENDPOINT_PORT_STATUS) {
        /* Reachability belongs to the shared node. Reply even when this
         * QP is idle and an endpoint worker is driving the ports. */
        route->counts.rx[path].frames++;
        route->counts.rx[path].matched++;
        mrc_endpoint_receive_op(&direction->config, &route->node->state, &route->counts,
                                (unsigned int)path, packet, received.arrival_cycles);
        rte_pktmbuf_free(received.mbuf);
        return true;
    }
    return queue_packet(direction, (unsigned int)path, received);
}

static bool queue_node_packet(struct mrc_abi_route *current, uint16_t port,
                               const struct mrc_wire_packet *header, const struct mrc_wire_srv6 *routing,
                               struct mrc_endpoint_rx_packet packet, bool srv6, bool *matched)
{
    /* Prefer the QP already progressing. QPs on the same path to this peer
     * share node state, so one copy suffices and requests get one reply. */
    if (current && route_node_packet(current, port, header, routing, packet, srv6, matched))
        return true;
    for (struct mrc_abi_route *route = routes; route; route = route->next) {
        if (route == current || route->error)
            continue;
        if (route_node_packet(route, port, header, routing, packet, srv6, matched))
            return true;
    }
    return false;
}

static bool node_packet(struct mrc_abi_route *current, uint16_t port,
                         const struct mrc_wire_packet *header, const struct mrc_wire_srv6 *routing,
                         struct mrc_endpoint_rx_packet packet)
{
    bool matched = false;
    if (queue_node_packet(current, port, header, routing, packet, routing->encapsulated, &matched))
        return true;

    /* Bare traffic can come from a fabric that decapsulated SRv6. A full
     * native queue must still own its matching traffic when both coexist. */
    return !routing->encapsulated && !matched &&
           queue_node_packet(current, port, header, routing, packet, true, NULL);
}

static struct mrc_abi_route *find_route(uint32_t qpn)
{
    struct mrc_abi_route *route = routes_by_qpn[qpn % ROUTE_BUCKETS];
    while (route && route->qpn != qpn)
        route = route->next_qpn;
    return route;
}

bool mrc_abi_qp_port_token_in_use(uint16_t token)
{
    for (struct mrc_abi_route *route = routes; route; route = route->next) {
        if (route->error)
            continue;
        const struct mrc_endpoint_node *node = &route->node->state;
        for (unsigned int i = 0; i < PATH_COUNT; i++) {
            if (node->awaiting[i] && node->pending[i] == token)
                return true;
        }
    }
    return false;
}

static bool dispatch_node_packet(struct mrc_abi_route *current, uint16_t port,
                                  struct mrc_endpoint_rx_packet received,
                                  const uint8_t *frame, size_t length)
{
    struct mrc_wire_packet packet;
    struct mrc_wire_srv6 routing;
    if (mrc_wire_decode_srv6(frame, length, &packet, &routing))
        return false;

    bool status_request = packet.bth.opcode == MRC_ENDPOINT_REQUEST &&
                          packet.body.endpoint_request.op == MRC_ENDPOINT_PORT_STATUS;
    /* An existing QP needs the peer's reports for its rail reachability. */
    if (status_request && node_packet(current, port, &packet, &routing, received))
        return true;
    if (mrc_abi_dispatch_endpoint(port, &packet, &routing, received.arrival_cycles)) {
        rte_pktmbuf_free(received.mbuf);
        return true;
    }
    return !status_request && node_packet(current, port, &packet, &routing, received);
}

static bool dispatch_packet(struct mrc_abi_route *current, uint16_t port,
                             struct mrc_endpoint_rx_packet packet)
{
    uint8_t scratch[MRC_ENDPOINT_FRAME_CAPACITY];
    uint32_t length = rte_pktmbuf_pkt_len(packet.mbuf);
    if (length > sizeof(scratch))
        return false;
    const uint8_t *frame = rte_pktmbuf_read(packet.mbuf, 0, length, scratch);
    struct mrc_wire_packet header;
    struct mrc_wire_srv6 routing;
    if (!frame || mrc_wire_dispatch_srv6_header(frame, length, &header, &routing))
        return false;

    if (header.bth.dqp == MRC_ENDPOINT_QPN)
        return (header.bth.opcode == MRC_ENDPOINT_REQUEST || header.bth.opcode == MRC_ENDPOINT_RESPONSE)
               && dispatch_node_packet(current, port, packet, frame, length);
    struct mrc_abi_route *target = find_route(header.bth.dqp);
    if (!target || target->error)
        return false;
    struct qp_direction *direction;
    switch (header.bth.opcode) {
    case MRC_TRANSPORT_ACK:
    case MRC_RELIABILITY_SACK:
    case MRC_RELIABILITY_NACK:
        direction = target->request ? &target->tx : NULL;
        break;
    default:
        direction = target->response ? &target->rx : NULL;
        break;
    }
    if (!direction)
        return false;
    int path = matched_path(&direction->config, port, &header, &routing);
    return path >= 0 && queue_packet(direction, (unsigned int)path, packet);
}

static void poll_ports(struct mrc_abi_route *route, const uint16_t ports[PATH_COUNT])
{
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        uint16_t port = ports[path];
        struct rte_mbuf *packets[RX_BURST];
        uint16_t count = rte_eth_rx_burst(port, 0, packets, RX_BURST);
        uint64_t arrival_cycles = count ? rte_get_timer_cycles() : 0;
        for (uint16_t i = 0; i < count; i++) {
            struct mrc_endpoint_rx_packet packet = { .mbuf = packets[i], .arrival_cycles = arrival_cycles };
            if (!dispatch_packet(route, port, packet))
                rte_pktmbuf_free(packets[i]);
        }
    }
}

static void receive_ports(struct mrc_abi_route *route)
{
    const struct mrc_endpoint_config *config = &node_direction(route)->config;
    const uint16_t ports[PATH_COUNT] = { config->paths[0].port, config->paths[1].port };
    poll_ports(route, ports);
}

void mrc_abi_poll_endpoint_ports(const uint16_t ports[2])
{
    poll_ports(NULL, ports);
}

static bool same_node_peer(const struct mrc_endpoint_config *a, const struct mrc_endpoint_config *b)
{
    if (a->udp_port != b->udp_port || a->partition_key != b->partition_key ||
        a->peer_partition_key != b->peer_partition_key || a->ip_version != b->ip_version ||
        a->srv6 != b->srv6)
        return false;
    if (a->ip_version == MRC_WIRE_IP4) {
        if (a->local_ip != b->local_ip || a->peer_ip != b->peer_ip)
            return false;
    } else if (memcmp(a->local_ip6, b->local_ip6, MRC_IPV6_ADDR_LEN) ||
               memcmp(a->peer_ip6, b->peer_ip6, MRC_IPV6_ADDR_LEN)) {
        return false;
    }
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        const struct mrc_endpoint_path *first = &a->paths[path], *second = &b->paths[path];
        if (first->port != second->port || !rte_is_same_ether_addr(&first->local, &second->local) ||
            !rte_is_same_ether_addr(&first->peer, &second->peer))
            return false;
        if (a->srv6 && (memcmp(first->srv6.local_sid, second->srv6.local_sid, MRC_IPV6_ADDR_LEN) ||
                        memcmp(first->srv6.peer_sid, second->srv6.peer_sid, MRC_IPV6_ADDR_LEN)))
            return false;
    }
    return true;
}

static struct peer_node *join_peer_node(const struct mrc_endpoint_config *config)
{
    for (struct mrc_abi_route *route = routes; route; route = route->next) {
        if (same_node_peer(config, &node_direction(route)->config)) {
            route->node->users++;
            return route->node;
        }
    }
    struct peer_node *node = calloc(1, sizeof(*node));
    if (node)
        node->users = 1;
    return node;
}

static void release_peer_node(struct peer_node *node)
{
    if (node && --node->users == 0)
        free(node);
}

static void register_route(struct mrc_abi_route *route)
{
    struct mrc_abi_route **bucket = &routes_by_qpn[route->qpn % ROUTE_BUCKETS];
    route->next = routes;
    route->next_qpn = *bucket;
    routes = *bucket = route;
}

static void unregister_route(const struct mrc_abi_route *route)
{
    struct mrc_abi_route **bucket = &routes_by_qpn[route->qpn % ROUTE_BUCKETS];
    while (*bucket != route)
        bucket = &(*bucket)->next_qpn;
    *bucket = route->next_qpn;

    struct mrc_abi_route **entry = &routes;
    while (*entry != route)
        entry = &(*entry)->next;
    *entry = route->next;
}

int mrc_abi_configure_addresses(struct mrc_endpoint_config *config, const union ibv_gid *local,
                                const union ibv_gid *peer)
{
    struct in6_addr source, dest;
    memcpy(&source, local->raw, sizeof(source));
    memcpy(&dest, peer->raw, sizeof(dest));
    if (IN6_IS_ADDR_UNSPECIFIED(&source) || IN6_IS_ADDR_UNSPECIFIED(&dest))
        return EADDRNOTAVAIL;
    bool ipv4 = IN6_IS_ADDR_V4MAPPED(&source);
    if (ipv4 != !!IN6_IS_ADDR_V4MAPPED(&dest))
        return EAFNOSUPPORT;
    if (ipv4) {
        uint32_t source4, dest4;
        memcpy(&source4, &local->raw[12], sizeof(source4));
        memcpy(&dest4, &peer->raw[12], sizeof(dest4));
        config->ip_version = MRC_WIRE_IP4;
        config->local_ip = ntohl(source4);
        config->peer_ip = ntohl(dest4);
    } else {
        config->ip_version = MRC_WIRE_IP6;
        memcpy(config->local_ip6, local->raw, sizeof(config->local_ip6));
        memcpy(config->peer_ip6, peer->raw, sizeof(config->peer_ip6));
    }
    return 0;
}

static bool sid_is_zero(const uint8_t sid[MRC_IPV6_ADDR_LEN])
{
    const uint8_t zero[MRC_IPV6_ADDR_LEN] = {0};
    return memcmp(sid, zero, sizeof(zero)) == 0;
}

int mrc_abi_prepare_srv6_paths(const struct mrc_dpdk_srv6_path *paths,
                              struct mrc_endpoint_srv6_path *prepared)
{
    if (!paths)
        return EINVAL;
    for (unsigned int i = 0; i < PATH_COUNT; i++) {
        const struct mrc_dpdk_srv6_path *path = &paths[i];
        struct mrc_endpoint_srv6_path *output = &prepared[i];
        bool segment = !sid_is_zero(path->srh_segment);
        if (sid_is_zero(path->local_sid) || sid_is_zero(path->peer_sid) ||
            path->local_sid[0] == 0xff || path->peer_sid[0] == 0xff ||
            path->initial_dst[0] == 0xff || path->srh_segment[0] == 0xff ||
            path->segments_left > (segment ? 1 : 0))
            return EINVAL;

        const uint8_t *initial_dst = sid_is_zero(path->initial_dst) ? path->peer_sid : path->initial_dst;
        memcpy(output->local_sid, path->local_sid, sizeof(output->local_sid));
        memcpy(output->peer_sid, path->peer_sid, sizeof(output->peer_sid));
        memcpy(output->initial_dst, initial_dst, sizeof(output->initial_dst));
        memcpy(output->segments[0], path->srh_segment, sizeof(output->segments[0]));
        output->segment_count = segment ? 1 : 0;
        output->segments_left = path->segments_left;
    }
    return 0;
}

static int configure_route(struct mrc_qp *qp, const struct mrc_dpdk_route *route,
                            const struct mrc_endpoint_srv6_path *srv6, struct qp_direction *direction,
                            bool tx)
{
    struct mrc_endpoint_config *config = &direction->config;
    const struct ibv_qp_attr *verbs = &qp->verbs;
    const struct mrc_qp_attr *attrs = &qp->attrs;
    const uint8_t mpr = tx ? attrs->mpr.mpr : attrs->mpr.mpr_dest;
    const uint32_t window = mpr * MRC_MPR_UNIT;
    uint64_t first_timeout;
    if (!mrc_local_ack_timeout_ns(attrs->timeout, attrs->retry.linear_cnt,
                                  attrs->retry.exp_cnt, 0, &first_timeout))
        return EINVAL;

    __be16 pkey;
    int error = ibv_query_pkey(qp->context->verbs, verbs->ah_attr.port_num, verbs->pkey_index, &pkey);
    if (error)
        return error > 0 ? error : (errno ? errno : -error);
    *config = (struct mrc_endpoint_config){
        .sender = tx, .continuous_send = tx, .pool = route->pool, .pd = qp->pd->provider,
        .srv6 = srv6 != NULL,
        .requestor_qpn = tx ? qp->qpn : verbs->dest_qp_num,
        .responder_qpn = tx ? verbs->dest_qp_num : qp->qpn,
        .initial_psn = tx ? verbs->sq_psn : verbs->rq_psn,
        .partition_key = ntohs(pkey), .peer_partition_key = route->peer_pkey ? route->peer_pkey : ntohs(pkey),
        .mpr_max = mpr, .window = window,
        .max_wimm_inflight = tx ? attrs->wimm.max_wimm : attrs->wimm.max_wimm_dest,
        .rx_ooo_capacity = window, .rx_staging_capacity = window, .rx_work_capacity = window,
        .dynamic_mpr = (attrs->cap & MRC_QP_CAP_DYNAMIC_MPR) != 0,
        /* The local device advertises generation independently of the peer. */
        .trim_nack_enabled = !tx,
        .report_service_time = true,
        .peer_service_time = tx && !(attrs->cap & MRC_QP_CAP_SVC_TIME_DEST_UNSUPPORTED),
        .timeout_exponent = attrs->timeout, .linear_retries = attrs->retry.linear_cnt,
        .exponential_retries = attrs->retry.exp_cnt,
        .udp_port = route->udp_port ? route->udp_port : MRC_UDP_DEFAULT_PORT,
        .trim_dscp = route->trim_dscp ? route->trim_dscp : MRC_ENDPOINT_TRIM_DSCP,
        .drop_after = UINT32_MAX, .restore_after = UINT32_MAX, .trim_after = UINT32_MAX,
        .mpr_change_at = UINT32_MAX, .hz = rte_get_timer_hz(),
        .receive = take_packets, .receive_context = direction,
        .port_status_token_in_use = mrc_abi_endpoint_port_token_in_use,
    };
    if (!config->hz)
        return EIO;
    config->initial_rto = mrc_endpoint_cycles(config, first_timeout);
    /* The API uses the QP's configured retry budget across progress calls. */
    config->idle_timeout = UINT64_MAX;

    error = mrc_abi_configure_addresses(config, &qp->source_gid, &verbs->ah_attr.grh.dgid);
    if (error)
        return error;
    if (tx) {
        config->cc_profile = qp->cc_profile;
        pthread_mutex_lock(&qp->context->mutex);
        struct mrc_abi_sl_dscp dscp = qp->context->sl_dscp[verbs->ah_attr.sl];
        pthread_mutex_unlock(&qp->context->mutex);
        config->data_dscp = dscp.data;
        config->retry_dscp = dscp.retry;
    }
    if (tx && attrs->profile.ev_profile_id) {
        config->profile_id = attrs->profile.ev_profile_id;
        config->profile_store = mrc_abi_profile_store(qp->context);
        error = -mrc_ctl_store_get(&config->profile_store, config->profile_id, &config->profile);
        if (error)
            return error;
        if (config->profile.state != MRC_CTL_STORE_ONLINE)
            return EINVAL;
    }
    for (unsigned int i = 0; i < PATH_COUNT; i++) {
        struct mrc_endpoint_path *path = &config->paths[i];
        path->port = route->ports[i];
        if (srv6)
            path->srv6 = srv6[i];
        memcpy(path->peer.addr_bytes, route->peer_macs[i], sizeof(path->peer.addr_bytes));
        error = -rte_eth_macaddr_get(path->port, &path->local);
        if (error)
            return error;
    }
    if (config->profile_id && !mrc_endpoint_apply_ev_profile(config))
        return EINVAL;
    return 0;
}

static int validate_attachment(struct mrc_qp *qp, bool tx, bool rx)
{
    if (qp->route)
        return EBUSY;
    struct mrc_provider_qp_attr attr;
    int error = -mrc_provider_qp_query(qp->provider, &attr);
    if (error)
        return error;
    if (tx && (attr.state != MRC_PROVIDER_QP_RTS || !qp->init.cap.max_send_wr))
        return EINVAL;
    if (rx) {
        if (attr.state != MRC_PROVIDER_QP_RTR && attr.state != MRC_PROVIDER_QP_RTS)
            return EINVAL;
        if (!(qp->verbs.qp_access_flags & IBV_ACCESS_REMOTE_WRITE))
            return EACCES;
    }
    if ((!tx && attr.send_pending) || (!rx && attr.recv_pending))
        return EBUSY;
    return 0;
}

static void free_route(struct mrc_abi_route *route)
{
    discard_packets(&route->tx);
    discard_packets(&route->rx);
    mrc_requester_destroy(route->request);
    mrc_responder_destroy(route->response);
    release_peer_node(route->node);
    free(route);
}

static int attach_qp(struct mrc_qp *qp, const struct mrc_dpdk_route *route,
                     const struct mrc_endpoint_srv6_path *srv6)
{
    if (!qp || !route || !route->pool || route->ports[0] == route->ports[1] || route->trim_dscp > 63)
        return EINVAL;
    for (unsigned int i = 0; i < PATH_COUNT; i++) {
        if (!rte_eth_dev_is_valid_port(route->ports[i]) || route->ports[i] >= RTE_MAX_ETHPORTS)
            return ENODEV;
    }

    bool tx = route->requester || route->bidirectional;
    bool rx = !route->requester || route->bidirectional;
    pthread_mutex_lock(&qp->mutex);
    int error = validate_attachment(qp, tx, rx);
    if (error)
        goto unlock;
    struct mrc_abi_route *object = calloc(1, sizeof(*object));
    if (!object) {
        error = ENOMEM;
        goto unlock;
    }
    object->qpn = qp->qpn;
    if (tx)
        error = configure_route(qp, route, srv6, &object->tx, true);
    if (!error && rx)
        error = configure_route(qp, route, srv6, &object->rx, false);

    pthread_mutex_lock(&mrc_abi_dispatch_mutex);
    if (!error) {
        object->node = join_peer_node(tx ? &object->tx.config : &object->rx.config);
        if (!object->node)
            error = ENOMEM;
    }
    if (!error && tx) {
        object->tx.config.node = &object->node->state;
        error = -mrc_requester_create(&object->tx.config, qp->provider, &object->counts, &object->request);
    }
    if (!error && rx) {
        object->rx.config.node = &object->node->state;
        error = -mrc_responder_create(&object->rx.config, qp->provider, &object->counts, &object->response);
    }
    if (error) {
        free_route(object);
    } else {
        register_route(object);
        qp->requester = tx;
        qp->responder = rx;
        qp->route = object;
    }
    pthread_mutex_unlock(&mrc_abi_dispatch_mutex);

unlock:
    pthread_mutex_unlock(&qp->mutex);
    return error;
}

MRC_PUBLIC int mrc_dpdk_set_sl_dscp(struct mrc_context *context, unsigned int sl,
                                  unsigned int data_dscp, unsigned int retry_dscp)
{
    if (!context || sl >= MRC_ABI_SL_COUNT || data_dscp > 63 || retry_dscp > 63 ||
        data_dscp == MRC_ENDPOINT_CONTROL_DSCP || retry_dscp == MRC_ENDPOINT_CONTROL_DSCP)
        return EINVAL;
    pthread_mutex_lock(&context->mutex);
    context->sl_dscp[sl] = (struct mrc_abi_sl_dscp){ .data = (uint8_t)data_dscp, .retry = (uint8_t)retry_dscp };
    pthread_mutex_unlock(&context->mutex);
    return 0;
}

MRC_PUBLIC int mrc_dpdk_attach_qp(struct mrc_qp *qp, const struct mrc_dpdk_route *route)
{
    return attach_qp(qp, route, NULL);
}

MRC_PUBLIC int mrc_dpdk_attach_srv6_qp(struct mrc_qp *qp, const struct mrc_dpdk_route *route,
                                      const struct mrc_dpdk_srv6_path paths[2])
{
    struct mrc_endpoint_srv6_path prepared[PATH_COUNT];
    int error = mrc_abi_prepare_srv6_paths(paths, prepared);
    return error ? error : attach_qp(qp, route, prepared);
}

MRC_PUBLIC int mrc_dpdk_set_rx_flow_control(struct mrc_qp *qp, uint8_t penalty, bool restore)
{
    if (!qp || penalty > 127)
        return EINVAL;
    pthread_mutex_lock(&qp->mutex);
    int error = ENOTCONN;
    if (qp->route && qp->route->response)
        error = -mrc_responder_set_flow_control(qp->route->response, penalty, restore);
    pthread_mutex_unlock(&qp->mutex);
    return error;
}

MRC_PUBLIC int mrc_dpdk_set_trim_last_hop_dscp(struct mrc_qp *qp, unsigned int dscp)
{
    if (!qp || dscp > 63 || dscp == MRC_ENDPOINT_CONTROL_DSCP)
        return EINVAL;
    pthread_mutex_lock(&qp->mutex);
    int error = ENOTCONN;
    if (qp->route && qp->route->response) {
        struct mrc_endpoint_config *config = &qp->route->rx.config;
        error = dscp == config->trim_dscp ? EINVAL : 0;
        if (!error)
            config->trim_last_hop_dscp = (uint8_t)dscp;
    }
    pthread_mutex_unlock(&qp->mutex);
    return error;
}

MRC_PUBLIC int mrc_dpdk_progress_qp(struct mrc_qp *qp)
{
    if (!qp)
        return EINVAL;
    pthread_mutex_lock(&qp->mutex);
    struct mrc_abi_route *route = qp->route;
    int error = ENOTCONN;
    if (route) {
        pthread_mutex_lock(&mrc_abi_dispatch_mutex);
        if (!route->error) {
            int result = mrc_provider_qp_check_cqs(qp->provider);
            if (!result) {
                receive_ports(route);
                if (route->response)
                    result = mrc_responder_progress(route->response);
                if (result >= 0 && route->request)
                    result = mrc_requester_progress(route->request);
            }
            if (result < 0) {
                route->error = -result;
                discard_packets(&route->rx);
                discard_packets(&route->tx);
            }
        }
        error = route->error;
        pthread_mutex_unlock(&mrc_abi_dispatch_mutex);
    }
    pthread_mutex_unlock(&qp->mutex);
    return error;
}

void mrc_abi_release_route(struct mrc_qp *qp)
{
    struct mrc_abi_route *route = qp->route;
    if (route) {
        pthread_mutex_lock(&mrc_abi_dispatch_mutex);
        unregister_route(route);
        free_route(route);
        pthread_mutex_unlock(&mrc_abi_dispatch_mutex);
        qp->route = NULL;
    }
}
