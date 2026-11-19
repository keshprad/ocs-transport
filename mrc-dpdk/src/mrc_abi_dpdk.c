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
    struct rte_mbuf *packets[RX_PENDING];
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
 * serialized here. Callers already hold their own QP mutex. The dispatcher
 * never takes another QP's mutex. */
static pthread_mutex_t dispatch_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct mrc_abi_route *routes;
static struct mrc_abi_route *routes_by_qpn[ROUTE_BUCKETS];

static uint16_t take_packets(void *context, unsigned int path,
                              struct rte_mbuf **packets, uint16_t limit)
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
        struct rte_mbuf *packets[RX_PENDING];
        uint16_t count = take_packets(direction, path, packets, RX_PENDING);
        rte_pktmbuf_free_bulk(packets, count);
    }
}

static bool queue_packet(struct qp_direction *direction, uint16_t port,
                          const struct mrc_wire_packet *header, struct rte_mbuf *packet)
{
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        if (direction->config.paths[path].port != port)
            continue;
        struct rx_queue *queue = &direction->ingress[path];
        if (queue->count == RX_PENDING ||
            !mrc_endpoint_peer_matches(&direction->config, path, header))
            return false;
        queue->packets[(queue->head + queue->count) % RX_PENDING] = packet;
        queue->count++;
        return true;
    }
    return false;
}

static struct qp_direction *node_direction(struct mrc_abi_route *route)
{
    return route->request ? &route->tx : &route->rx;
}

static bool node_packet(struct mrc_abi_route *current, uint16_t port,
                         const struct mrc_wire_packet *header, struct rte_mbuf *packet)
{
    /* Prefer the QP already progressing. QPs on the same path to this peer
     * share node state, so one copy suffices and requests get one reply. */
    if (queue_packet(node_direction(current), port, header, packet))
        return true;
    for (struct mrc_abi_route *route = routes; route; route = route->next) {
        if (route != current && !route->error &&
            queue_packet(node_direction(route), port, header, packet))
            return true;
    }
    return false;
}

static struct mrc_abi_route *find_route(uint32_t qpn)
{
    struct mrc_abi_route *route = routes_by_qpn[qpn % ROUTE_BUCKETS];
    while (route && route->qpn != qpn)
        route = route->next_qpn;
    return route;
}

static bool dispatch_packet(struct mrc_abi_route *current, uint16_t port, struct rte_mbuf *packet)
{
    uint8_t scratch[MRC_WIRE_IPV6_HEADER_LEN + MRC_BTH_LEN];
    uint32_t length = rte_pktmbuf_pkt_len(packet);
    if (length > sizeof(scratch))
        length = sizeof(scratch);
    const uint8_t *prefix = rte_pktmbuf_read(packet, 0, length, scratch);
    struct mrc_wire_packet header;
    if (!prefix || mrc_wire_dispatch_header(prefix, length, &header))
        return false;

    if (header.bth.dqp == MRC_ENDPOINT_QPN)
        return (header.bth.opcode == MRC_ENDPOINT_REQUEST || header.bth.opcode == MRC_ENDPOINT_RESPONSE)
               && node_packet(current, port, &header, packet);
    struct mrc_abi_route *target = find_route(header.bth.dqp);
    if (!target || target->error)
        return false;
    switch (header.bth.opcode) {
    case MRC_TRANSPORT_ACK:
    case MRC_RELIABILITY_SACK:
    case MRC_RELIABILITY_NACK:
        return target->request && queue_packet(&target->tx, port, &header, packet);
    default:
        return target->response && queue_packet(&target->rx, port, &header, packet);
    }
}

static void receive_ports(struct mrc_abi_route *route)
{
    const struct mrc_endpoint_config *config = &node_direction(route)->config;
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        uint16_t port = config->paths[path].port;
        struct rte_mbuf *packets[RX_BURST];
        uint16_t count = rte_eth_rx_burst(port, 0, packets, RX_BURST);
        for (uint16_t i = 0; i < count; i++) {
            if (!dispatch_packet(route, port, packets[i]))
                rte_pktmbuf_free(packets[i]);
        }
    }
}

static bool same_node_peer(const struct mrc_endpoint_config *a, const struct mrc_endpoint_config *b)
{
    if (a->udp_port != b->udp_port || a->partition_key != b->partition_key ||
        a->peer_partition_key != b->peer_partition_key || a->ip_version != b->ip_version)
        return false;
    if (a->ip_version == MRC_WIRE_IP4) {
        if (a->local_ip != b->local_ip || a->peer_ip != b->peer_ip)
            return false;
    } else if (memcmp(a->local_ip6, b->local_ip6, MRC_IPV6_ADDR_LEN) ||
               memcmp(a->peer_ip6, b->peer_ip6, MRC_IPV6_ADDR_LEN)) {
        return false;
    }
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        if (a->paths[path].port != b->paths[path].port ||
            !rte_is_same_ether_addr(&a->paths[path].local, &b->paths[path].local) ||
            !rte_is_same_ether_addr(&a->paths[path].peer, &b->paths[path].peer))
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

static int configure_addresses(struct mrc_endpoint_config *config, const union ibv_gid *local,
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

static int configure_route(struct mrc_qp *qp, const struct mrc_dpdk_route *route,
                            struct qp_direction *direction, bool tx)
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
        .requestor_qpn = tx ? qp->qpn : verbs->dest_qp_num,
        .responder_qpn = tx ? verbs->dest_qp_num : qp->qpn,
        .initial_psn = tx ? verbs->sq_psn : verbs->rq_psn,
        .partition_key = ntohs(pkey), .peer_partition_key = route->peer_pkey ? route->peer_pkey : ntohs(pkey),
        .mpr_max = mpr, .window = window,
        .max_wimm_inflight = tx ? attrs->wimm.max_wimm : attrs->wimm.max_wimm_dest,
        .rx_ooo_capacity = window, .rx_staging_capacity = window, .rx_work_capacity = window,
        .dynamic_mpr = (attrs->cap & MRC_QP_CAP_DYNAMIC_MPR) != 0,
        .trim_nack_enabled = !(attrs->cap & MRC_QP_CAP_TRIM_NACK_DEST_UNSUPPORTED),
        .timeout_exponent = attrs->timeout, .linear_retries = attrs->retry.linear_cnt,
        .exponential_retries = attrs->retry.exp_cnt,
        .udp_port = route->udp_port ? route->udp_port : MRC_UDP_DEFAULT_PORT,
        .trim_dscp = route->trim_dscp ? route->trim_dscp : 49,
        .drop_after = UINT32_MAX, .restore_after = UINT32_MAX, .trim_after = UINT32_MAX,
        .mpr_change_at = UINT32_MAX, .hz = rte_get_timer_hz(),
        .receive = take_packets, .receive_context = direction,
    };
    if (!config->hz)
        return EIO;
    config->initial_rto = mrc_endpoint_cycles(config, first_timeout);
    /* The API uses the QP's configured retry budget across progress calls. */
    config->idle_timeout = UINT64_MAX;

    error = configure_addresses(config, &qp->source_gid, &verbs->ah_attr.grh.dgid);
    if (error)
        return error;
    if (tx && attrs->profile.ev_profile_id) {
        config->profile_id = attrs->profile.ev_profile_id;
        config->profile_store = mrc_abi_ev_store(qp->context);
        error = -mrc_ctl_store_get(&config->profile_store, config->profile_id, &config->profile);
        if (error)
            return error;
        if (config->profile.state != MRC_CTL_STORE_ONLINE || !mrc_endpoint_ev_profile_valid(config))
            return EINVAL;
    }
    for (unsigned int i = 0; i < PATH_COUNT; i++) {
        struct mrc_endpoint_path *path = &config->paths[i];
        path->port = route->ports[i];
        memcpy(path->peer.addr_bytes, route->peer_macs[i], sizeof(path->peer.addr_bytes));
        error = -rte_eth_macaddr_get(path->port, &path->local);
        if (error)
            return error;
    }
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

MRC_PUBLIC int mrc_dpdk_attach_qp(struct mrc_qp *qp, const struct mrc_dpdk_route *route)
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
        error = configure_route(qp, route, &object->tx, true);
    if (!error && rx)
        error = configure_route(qp, route, &object->rx, false);

    pthread_mutex_lock(&dispatch_mutex);
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
    pthread_mutex_unlock(&dispatch_mutex);

unlock:
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
        pthread_mutex_lock(&dispatch_mutex);
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
        pthread_mutex_unlock(&dispatch_mutex);
    }
    pthread_mutex_unlock(&qp->mutex);
    return error;
}

void mrc_abi_release_route(struct mrc_qp *qp)
{
    struct mrc_abi_route *route = qp->route;
    if (route) {
        pthread_mutex_lock(&dispatch_mutex);
        unregister_route(route);
        free_route(route);
        pthread_mutex_unlock(&dispatch_mutex);
        qp->route = NULL;
    }
}
