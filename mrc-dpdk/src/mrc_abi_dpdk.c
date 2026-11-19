#include "mrc_abi.h"
#include "mrc_endpoint.h"
#include "mrc_qp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_ethdev.h>

struct mrc_abi_route {
    struct mrc_endpoint_config config;
    struct mrc_endpoint_counters counts;
    struct mrc_requester *request;
    struct mrc_responder *response;
};

/* Until there is shared RX dispatch, no two attached QPs may poll the same
 * ethdev RX queue. An application with separate rails can drive them in parallel. */
static pthread_mutex_t port_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool port_attached[RTE_MAX_ETHPORTS];

static int claim_ports(const struct mrc_dpdk_route *route)
{
    pthread_mutex_lock(&port_mutex);
    int error = 0;
    if (port_attached[route->ports[0]] || port_attached[route->ports[1]]) {
        error = EBUSY;
    } else {
        port_attached[route->ports[0]] = true;
        port_attached[route->ports[1]] = true;
    }
    pthread_mutex_unlock(&port_mutex);
    return error;
}

static void release_ports(const struct mrc_endpoint_config *config)
{
    pthread_mutex_lock(&port_mutex);
    for (unsigned int i = 0; i < PATH_COUNT; i++)
        port_attached[config->paths[i].port] = false;
    pthread_mutex_unlock(&port_mutex);
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
                            struct mrc_endpoint_config *config)
{
    const bool tx = route->requester;
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
    };
    if (!config->hz)
        return EIO;
    config->initial_rto = mrc_endpoint_cycles(config, first_timeout);
    /* The API uses the QP's configured retry budget across progress calls. */
    config->idle_timeout = UINT64_MAX;

    error = configure_addresses(config, &qp->source_gid, &verbs->ah_attr.grh.dgid);
    if (error)
        return error;
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

static int validate_attachment(struct mrc_qp *qp, const struct mrc_dpdk_route *route)
{
    if (qp->route)
        return EBUSY;
    struct mrc_provider_qp_attr attr;
    int error = -mrc_provider_qp_query(qp->provider, &attr);
    if (error)
        return error;
    if (route->requester) {
        if (attr.state != MRC_PROVIDER_QP_RTS || !qp->init.cap.max_send_wr)
            return EINVAL;
        if (attr.recv_pending)
            return EBUSY;
    } else {
        if (attr.state != MRC_PROVIDER_QP_RTR && attr.state != MRC_PROVIDER_QP_RTS)
            return EINVAL;
        if (!(qp->verbs.qp_access_flags & IBV_ACCESS_REMOTE_WRITE))
            return EACCES;
        if (attr.send_pending)
            return EBUSY;
    }
    return 0;
}

MRC_PUBLIC int mrc_dpdk_attach_qp(struct mrc_qp *qp, const struct mrc_dpdk_route *route)
{
    if (!qp || !route || !route->pool || route->ports[0] == route->ports[1] || route->trim_dscp > 63)
        return EINVAL;
    for (unsigned int i = 0; i < PATH_COUNT; i++) {
        if (!rte_eth_dev_is_valid_port(route->ports[i]) || route->ports[i] >= RTE_MAX_ETHPORTS)
            return ENODEV;
    }

    pthread_mutex_lock(&qp->mutex);
    int error = validate_attachment(qp, route);
    if (error)
        goto unlock;
    struct mrc_abi_route *object = calloc(1, sizeof(*object));
    if (!object) {
        error = ENOMEM;
        goto unlock;
    }
    error = configure_route(qp, route, &object->config);
    if (!error)
        error = claim_ports(route);
    if (error)
        goto free_route;

    error = route->requester ?
        -mrc_requester_create(&object->config, qp->provider, &object->counts, &object->request) :
        -mrc_responder_create(&object->config, qp->provider, &object->counts, &object->response);
    if (error) {
        release_ports(&object->config);
        goto free_route;
    }
    qp->requester = route->requester;
    qp->route = object;
    goto unlock;

free_route:
    free(object);
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
    int result = -ENOTCONN;
    if (route) {
        result = route->request ? mrc_requester_progress(route->request)
                               : mrc_responder_progress(route->response);
    }
    pthread_mutex_unlock(&qp->mutex);
    return result < 0 ? -result : 0;
}

void mrc_abi_release_route(struct mrc_qp *qp)
{
    struct mrc_abi_route *route = qp->route;
    if (route) {
        mrc_requester_destroy(route->request);
        mrc_responder_destroy(route->response);
        release_ports(&route->config);
        free(route);
        qp->route = NULL;
    }
}
