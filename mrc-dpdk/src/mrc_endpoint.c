#include "mrc_endpoint.h"

#include "dpdk_io.h"
#include "mrc_cc.h"
#include "mrc_ev.h"
#include "mrc_ev_event_io.h"
#include "mrc_mpr.h"
#include "mrc_qp.h"
#include "mrc_wr.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

enum {
    BURST = 32,
    SACK_THRESHOLD = 4096,
    UDP_HEADER_LEN = 8,
    REQUESTOR_EV_PORT = 0xc000,
    RESPONDER_EV_PORT = 0xd000,
    EV_FLOW_LABEL = 0xa0100,
    SINGLE_SEGMENT_SRH_LEN = 24,
    INITIAL_CWND = 128 * MRC_NOMINAL_MTU,
    TARGET_QUEUE_NS = 20000,
    FAST_LOSS_GAP = 16,
    ALL_PATHS = (1 << PATH_COUNT) - 1,
    DATA_DSCP = 0,
    CONTROL_DSCP = 48,
    ACK_SYNDROME = 0x1f,
    INVALID_REQUEST_NAK = 0x61,
    REMOTE_ACCESS_NAK = 0x62,
    REMOTE_OPERATION_NAK = 0x63,
};

struct send_record {
    unsigned int path;
    bool fast_retransmitted;
    bool nacked;
    bool counted_inflight;
    bool redundant_attempt;
};

struct outstanding_probe {
    uint64_t sent_at;
    uint32_t entropy;
    uint16_t id;
    bool pending;
};

struct mrc_requester {
    struct mrc_endpoint_config transfer;
    struct mrc_endpoint_counters *counts;
    struct mrc_provider_qp *provider_qp;
    uint64_t first_sequence;
    uint64_t scheduled_packets;
    uint32_t msn_base;
    uint16_t rqmsn_base;
    size_t work_capacity;
    uint64_t last_progress;
    uint64_t profile_period;
    uint64_t next_profile_check;
    uint64_t next_retry_scan;
    uint64_t next_probe;
    uint32_t denied;
    int event_fd;
    bool event_dropped;
    int result;
    uint32_t initial_probe_paths;
    struct mrc_cc cc;
    struct mrc_endpoint_node local_node;
    struct mrc_endpoint_node *node;
    struct mrc_tx_qp qp;
    struct mrc_tx_packet slots[WINDOW];
    struct send_record metadata[WINDOW];
    struct mrc_ev_profile ev;
    struct mrc_mpr mpr;
    uint64_t blocked_since[PATH_COUNT];
    uint32_t next;
    int32_t highest_sacked;
    uint16_t probe_id;
    struct outstanding_probe probes[PATH_COUNT];
};

struct mrc_responder {
    const struct mrc_endpoint_config *transfer;
    struct mrc_endpoint_counters *counts;
    struct mrc_provider_qp *provider_qp;
    uint8_t *staged_payload;
    uint64_t last_progress;
    uint64_t last_frame;
    int result;
    struct mrc_rx_qp qp;
    struct mrc_wr_responder wr;
    struct mrc_mpr mpr;
    struct mrc_endpoint_node local_node;
    struct mrc_endpoint_node *node;
    struct mrc_wr_slot work[WINDOW];
    struct mrc_wr_staged staged[WINDOW];
    uint8_t slots[WINDOW];
    uint32_t unique_packets;
    struct mrc_cc_rx cc;
    struct mrc_cc_receiver_flow flow;
};

static uint32_t request_index(const struct mrc_endpoint_config *transfer, uint32_t packet)
{
    if (packet >= transfer->packets)
        return transfer->work_requests;
    if (transfer->uniform_wr_packets)
        return packet / transfer->uniform_wr_packets;

    uint32_t first = 0, last = transfer->work_requests;
    while (last - first > 1) {
        uint32_t middle = first + (last - first) / 2;
        if (packet < transfer->work[middle].first_packet)
            last = middle;
        else
            first = middle;
    }
    return first;
}

static uint32_t length_at(const struct mrc_endpoint_write *work, uint32_t index)
{
    uint32_t remaining = work->length - (index - work->first_packet) * PMTU;
    return remaining < PMTU ? remaining : PMTU;
}

static uint8_t expected_opcode(const struct mrc_endpoint_write *work, uint32_t index)
{
    if (work->packets == 1)
        return work->with_immediate ? MRC_WRITE_ONLY_IMM : MRC_WRITE_ONLY;
    uint32_t fragment = index - work->first_packet;
    if (fragment == 0)
        return MRC_WRITE_FIRST;
    if (fragment + 1 == work->packets)
        return work->with_immediate ? MRC_WRITE_LAST_IMM : MRC_WRITE_LAST;
    return MRC_WRITE_MIDDLE;
}

static uint32_t nominal_size(uint8_t opcode, size_t data_length, bool timestamp)
{
    size_t body = MRC_BTH_LEN + MRC_METH_LEN + MRC_RETH_LEN + data_length;
    if (timestamp)
        body += MRC_TSETH_LEN;
    if (opcode == MRC_WRITE_LAST_IMM || opcode == MRC_WRITE_ONLY_IMM)
        body += MRC_IMMDT_LEN;
    return mrc_cc_nominal_size((uint16_t)(UDP_HEADER_LEN + ((body + 3) & ~(size_t)3) +
                                          MRC_ICRC_LEN));
}

static uint32_t nominal_at(const struct mrc_endpoint_config *transfer, uint32_t index)
{
    const struct mrc_endpoint_write *work = &transfer->work[request_index(transfer, index)];
    return nominal_size(expected_opcode(work, index), length_at(work, index), false);
}

static uint64_t cycles_to_ns(const struct mrc_endpoint_config *transfer, uint64_t cycles)
{
    return (cycles / transfer->hz) * 1000000000 +
           (cycles % transfer->hz) * 1000000000 / transfer->hz;
}

uint16_t mrc_endpoint_feedback_timestamp(const struct mrc_endpoint_config *transfer,
                                         uint64_t arrival_cycles, bool timestamp_present,
                                         bool tsr, uint16_t timestamp)
{
    uint64_t elapsed = transfer->report_service_time ?
        cycles_to_ns(transfer, rte_get_timer_cycles() - arrival_cycles) : 0;
    return mrc_cc_service_timestamp(elapsed, transfer->report_service_time, timestamp_present, tsr, timestamp);
}

static uint16_t write_feedback_timestamp(const struct mrc_endpoint_config *transfer,
                                          const struct mrc_wire_packet *request, uint64_t arrival_cycles)
{
    const struct mrc_wire_write *write = &request->body.write;
    return mrc_endpoint_feedback_timestamp(transfer, arrival_cycles, request->bth.timestamp,
                                            write->tsr, write->tx_timestamp);
}

/* The requester keeps a local send time and sends neither TSETH nor a
 * timestamp in PETH. A negotiated report is a standalone duration. */
static uint64_t peer_service_ns(const struct mrc_endpoint_config *transfer, uint16_t timestamp)
{
    return transfer->peer_service_time ? (uint64_t)timestamp * MRC_CC_TIMESTAMP_UNIT_NS : 0;
}

uint32_t mrc_endpoint_default_entropy(bool structured, unsigned int path)
{
    uint32_t port = REQUESTOR_EV_PORT + path;
    return structured ? port | ((EV_FLOW_LABEL + path) & UINT16_MAX) << 16 : port;
}

bool mrc_endpoint_apply_ev_profile(struct mrc_endpoint_config *config)
{
    const struct mrc_ctl_store_config *profile = &config->profile.config;
    if (profile->count != PATH_COUNT ||
        (profile->format == MRC_CTL_STORE_FORMAT_STEV && config->ip_version != MRC_WIRE_IP6))
        return false;
    if (profile->mode == MRC_CTL_STORE_AUTO)
        return !mrc_ctl_store_format_is_srv6(profile->format);
    if (mrc_ctl_store_format_is_srv6(profile->format)) {
        if (!config->sender || !config->srv6)
            return false;
        for (unsigned int path = 0; path < PATH_COUNT; path++) {
            if (!mrc_ctl_store_srv6_ev_valid(profile->format, profile->srv6[path]))
                return false;
        }
        for (unsigned int path = 0; path < PATH_COUNT; path++) {
            struct mrc_endpoint_srv6_path *route = &config->paths[path].srv6;
            const uint8_t *value = profile->srv6[path];
            const uint8_t *segment = value + MRC_CTL_STORE_SRV6_SID_BYTES;
            memcpy(route->initial_dst, value, sizeof(route->initial_dst));
            memcpy(route->segments[0], segment, sizeof(route->segments[0]));
            route->segment_count = profile->format == MRC_CTL_STORE_FORMAT_SRV6_SRH;
            route->segments_left = route->segment_count &&
                memcmp(value, segment, MRC_CTL_STORE_SRV6_SID_BYTES) != 0;
        }
        return true;
    }
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        uint32_t entropy = profile->entropy[path];
        if (!(uint16_t)entropy ||
            ((config->ip_version != MRC_WIRE_IP6 || profile->format == MRC_CTL_STORE_FORMAT_UDP) &&
             entropy > UINT16_MAX))
            return false;
    }
    return profile->entropy[0] != profile->entropy[1];
}

static uint32_t path_entropy(const struct mrc_endpoint_config *transfer, unsigned int index,
                             bool requestor)
{
    bool structured = transfer->ip_version == MRC_WIRE_IP6;
    if (requestor) {
        const struct mrc_ctl_store_config *profile = &transfer->profile.config;
        /* An SRv6 profile selects the route; the inner entropy still
         * carries a unique per-rail identifier for SACK/NACK reflection. */
        if (transfer->profile_id && profile->mode != MRC_CTL_STORE_AUTO &&
            !mrc_ctl_store_format_is_srv6(profile->format))
            return profile->entropy[index];
        return mrc_endpoint_default_entropy(
            structured && profile->format != MRC_CTL_STORE_FORMAT_UDP, index);
    }
    uint32_t port = RESPONDER_EV_PORT + index;
    return structured ? port | ((EV_FLOW_LABEL + index) & UINT16_MAX) << 16 : port;
}

static void publish_ev_transition(void *context, unsigned int path, enum mrc_ev_state state)
{
    struct mrc_requester *sender = context;
    const struct mrc_endpoint_config *transfer = &sender->transfer;
    const struct mrc_ctl_store_config *profile = &transfer->profile.config;
    uint8_t mask;
    uint8_t event_state;
    if (state == MRC_EV_GOOD) {
        mask = MRC_CTL_STORE_EV_EVENT_GOOD;
        event_state = MRC_EV_EVENT_GOOD;
    } else if (state == MRC_EV_ASSUMED_BAD) {
        mask = MRC_CTL_STORE_EV_EVENT_ASSUMED_BAD;
        event_state = MRC_EV_EVENT_ASSUMED_BAD;
    } else {
        return;
    }
    if (!(profile->event_mask & mask))
        return;

    struct mrc_ev_event_record event = {
        .profile_id = transfer->profile_id, .port = (uint8_t)(path + 1),
        .state = event_state, .dropped = sender->event_dropped,
    };
    if (mrc_ctl_store_format_is_srv6(profile->format)) {
        memcpy(event.value, profile->srv6[path], sizeof(event.value));
    } else {
        uint32_t value = path_entropy(transfer, path, true);
        for (unsigned int i = 0; i < sizeof(value); i++)
            event.value[i] = (uint8_t)(value >> (8 * (sizeof(value) - 1 - i)));
    }

    int result = mrc_ev_event_send(sender->event_fd, transfer->profile_store.fd, &event);
    if (!result)
        sender->event_dropped = false;
    else if (result == -EAGAIN || result == -ENOBUFS)
        sender->event_dropped = true;
}

static uint32_t reflect_entropy(const struct mrc_wire_packet *packet)
{
    if (packet->ip_version == MRC_WIRE_IP6)
        return ((packet->ipv6.flow_label & UINT16_MAX) << 16) | packet->ipv6.src_port;
    return packet->ipv4.src_port;
}

static uint8_t packet_ecn(const struct mrc_wire_packet *packet)
{
    return packet->ip_version == MRC_WIRE_IP6 ? packet->ipv6.ecn : packet->ipv4.ecn;
}

struct mrc_wire_packet mrc_endpoint_packet_for(const struct mrc_endpoint_config *transfer,
                                               unsigned int index, uint8_t opcode, uint32_t psn)
{
    const struct mrc_endpoint_path *path = &transfer->paths[index];
    uint32_t dqp = transfer->sender ? transfer->responder_qpn : transfer->requestor_qpn;
    if (opcode == MRC_ENDPOINT_REQUEST || opcode == MRC_ENDPOINT_RESPONSE)
        dqp = MRC_ENDPOINT_QPN;
    struct mrc_wire_packet packet = {
        .ip_version = transfer->ip_version,
        .bth = {
            .opcode = opcode, .psn = psn, .p_key = transfer->partition_key,
            .dqp = dqp,
        },
    };

    bool data = mrc_wire_is_write(opcode);
    uint32_t entropy = path_entropy(transfer, index, transfer->sender);
    uint8_t dscp = data || opcode == MRC_RELIABILITY_PROBE ? DATA_DSCP : CONTROL_DSCP;
    uint8_t ecn = data ? 2 : 0;
    if (transfer->ip_version == MRC_WIRE_IP6) {
        packet.ipv6 = (struct mrc_wire_ipv6){
            .src_port = (uint16_t)entropy, .dst_port = transfer->udp_port,
            .dscp = dscp, .ecn = ecn,
            .flow_label = (EV_FLOW_LABEL & ~UINT32_C(0xffff)) | (entropy >> 16),
        };
        memcpy(packet.ipv6.src_mac, path->local.addr_bytes, MRC_MAC_LEN);
        memcpy(packet.ipv6.dst_mac, path->peer.addr_bytes, MRC_MAC_LEN);
        memcpy(packet.ipv6.src_ip, transfer->local_ip6, MRC_IPV6_ADDR_LEN);
        memcpy(packet.ipv6.dst_ip, transfer->peer_ip6, MRC_IPV6_ADDR_LEN);
    } else {
        packet.ipv4 = (struct mrc_wire_ipv4){
            .src_port = (uint16_t)entropy, .dst_port = transfer->udp_port, .dscp = dscp, .ecn = ecn,
            .src_ip = transfer->local_ip, .dst_ip = transfer->peer_ip,
        };
        memcpy(packet.ipv4.src_mac, path->local.addr_bytes, MRC_MAC_LEN);
        memcpy(packet.ipv4.dst_mac, path->peer.addr_bytes, MRC_MAC_LEN);
    }
    return packet;
}

static size_t simulate_trim(const struct mrc_endpoint_config *transfer, unsigned int path, uint8_t *frame)
{
    /* Preserve the nominal UDP length but truncate the body before the RETH. */
    uint8_t *ip = frame + RTE_ETHER_HDR_LEN;
    size_t trim_headers = MRC_BTH_LEN + MRC_METH_LEN;
    if (transfer->srv6 || transfer->ip_version == MRC_WIRE_IP6) {
        struct rte_ipv6_hdr header;
        memcpy(&header, ip, sizeof(header));
        uint32_t vtc_flow = rte_be_to_cpu_32(header.vtc_flow);
        vtc_flow = (vtc_flow & ~(UINT32_C(63) << 22)) | (uint32_t)transfer->trim_dscp << 22;
        header.vtc_flow = rte_cpu_to_be_32(vtc_flow);
        size_t outer_payload = UDP_HEADER_LEN + trim_headers;
        if (transfer->srv6) {
            outer_payload += transfer->ip_version == MRC_WIRE_IP6 ?
                sizeof(struct rte_ipv6_hdr) : sizeof(struct rte_ipv4_hdr);
            if (transfer->paths[path].srv6.segment_count)
                outer_payload += SINGLE_SEGMENT_SRH_LEN;
        }
        header.payload_len = rte_cpu_to_be_16((uint16_t)outer_payload);
        memcpy(ip, &header, sizeof(header));
        return RTE_ETHER_HDR_LEN + sizeof(header) + outer_payload;
    }

    struct rte_ipv4_hdr header;
    memcpy(&header, ip, sizeof(header));
    size_t length = MRC_WIRE_IPV4_HEADER_LEN + trim_headers;
    header.type_of_service = (uint8_t)((transfer->trim_dscp << 2) | (header.type_of_service & 3));
    header.total_length = rte_cpu_to_be_16((uint16_t)(length - RTE_ETHER_HDR_LEN));
    header.hdr_checksum = 0;
    header.hdr_checksum = rte_ipv4_cksum(&header);
    memcpy(ip, &header, sizeof(header));
    return length;
}

static bool transmit_frame(const struct mrc_endpoint_config *transfer, unsigned int path,
                           const struct mrc_wire_packet *packet, bool inject_trim)
{
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(transfer->pool);
    if (!mbuf)
        return false;
    uint8_t *frame = rte_pktmbuf_mtod(mbuf, uint8_t *);
    size_t length;
    size_t capacity = rte_pktmbuf_tailroom(mbuf);
    int result = mrc_wire_encode(frame, capacity, packet, &length);
    if (!result && transfer->srv6) {
        const struct mrc_endpoint_srv6_path *selected = &transfer->paths[path].srv6;
        bool ipv6 = packet->ip_version == MRC_WIRE_IP6;
        struct mrc_wire_srv6_tx route = {
            .flow_label = ipv6 ? packet->ipv6.flow_label : 0,
            .dscp = ipv6 ? packet->ipv6.dscp : packet->ipv4.dscp,
            .ecn = packet_ecn(packet),
            .segment_list = selected->segment_count ? selected->segments : NULL,
            .segment_count = selected->segment_count, .segments_left = selected->segments_left,
        };
        memcpy(route.outer_src_ip, selected->local_sid, MRC_IPV6_ADDR_LEN);
        memcpy(route.outer_dst_ip, selected->initial_dst, MRC_IPV6_ADDR_LEN);
        result = mrc_wire_wrap_srv6(frame, capacity, length, &route, &length);
    }
    if (result) {
        rte_pktmbuf_free(mbuf);
        return false;
    }
    if (inject_trim)
        length = simulate_trim(transfer, path, frame);
    if (!rte_pktmbuf_append(mbuf, (uint16_t)length)) {
        rte_pktmbuf_free(mbuf);
        return false;
    }
    return mrc_dpdk_transmit(transfer->paths[path].port, mbuf);
}

bool mrc_endpoint_transmit(const struct mrc_endpoint_config *transfer, unsigned int path,
                           const struct mrc_wire_packet *packet)
{
    return transmit_frame(transfer, path, packet, false);
}

static const uint8_t *read_frame(const struct rte_mbuf *mbuf,
                                  uint8_t scratch[MRC_ENDPOINT_FRAME_CAPACITY], size_t *length)
{
    *length = rte_pktmbuf_pkt_len(mbuf);
    if (*length > MRC_ENDPOINT_FRAME_CAPACITY)
        return NULL;
    return rte_pktmbuf_read(mbuf, 0, (uint32_t)*length, scratch);
}

bool mrc_endpoint_peer_matches(const struct mrc_endpoint_config *transfer, unsigned int index,
                         const struct mrc_wire_packet *packet)
{
    uint32_t expected_qpn = transfer->sender ? transfer->requestor_qpn : transfer->responder_qpn;
    if (packet->bth.opcode == MRC_ENDPOINT_REQUEST || packet->bth.opcode == MRC_ENDPOINT_RESPONSE)
        expected_qpn = MRC_ENDPOINT_QPN;
    if (packet->bth.p_key != transfer->peer_partition_key || packet->bth.dqp != expected_qpn ||
        packet->ip_version != transfer->ip_version)
        return false;

    const struct mrc_endpoint_path *path = &transfer->paths[index];
    if (packet->ip_version == MRC_WIRE_IP6) {
        const struct mrc_wire_ipv6 *ip = &packet->ipv6;
        return ip->dst_port == transfer->udp_port &&
               memcmp(ip->src_ip, transfer->peer_ip6, MRC_IPV6_ADDR_LEN) == 0 &&
               memcmp(ip->dst_ip, transfer->local_ip6, MRC_IPV6_ADDR_LEN) == 0 &&
               memcmp(ip->src_mac, path->peer.addr_bytes, MRC_MAC_LEN) == 0 &&
               memcmp(ip->dst_mac, path->local.addr_bytes, MRC_MAC_LEN) == 0;
    }

    const struct mrc_wire_ipv4 *ip = &packet->ipv4;
    return ip->dst_port == transfer->udp_port &&
           ip->src_ip == transfer->peer_ip && ip->dst_ip == transfer->local_ip &&
           memcmp(ip->src_mac, path->peer.addr_bytes, MRC_MAC_LEN) == 0 &&
           memcmp(ip->dst_mac, path->local.addr_bytes, MRC_MAC_LEN) == 0;
}

bool mrc_endpoint_route_matches(const struct mrc_endpoint_config *transfer, unsigned int path,
                                const struct mrc_wire_srv6 *route)
{
    const struct mrc_endpoint_srv6_path *selected = &transfer->paths[path].srv6;
    return !route->encapsulated ||
           (transfer->srv6 &&
            memcmp(route->outer_src_ip, selected->peer_sid, MRC_IPV6_ADDR_LEN) == 0 &&
            memcmp(route->outer_dst_ip, selected->local_sid, MRC_IPV6_ADDR_LEN) == 0);
}

static int decode_routed(const struct mrc_endpoint_config *transfer, unsigned int path,
                         const uint8_t *frame, size_t length, struct mrc_wire_packet *packet)
{
    struct mrc_wire_srv6 route;
    int result = mrc_wire_decode_srv6(frame, length, packet, &route);
    if (result)
        return result;
    if (!mrc_endpoint_route_matches(transfer, path, &route))
        return -EHOSTUNREACH;
    /* Propagate a fabric CE mark before building reliability feedback. */
    if (route.encapsulated && route.outer_ecn == 3) {
        if (packet->ip_version == MRC_WIRE_IP6)
            packet->ipv6.ecn = 3;
        else
            packet->ipv4.ecn = 3;
    }
    return 0;
}

static bool decode(const struct mrc_endpoint_config *transfer, unsigned int index,
                   const struct rte_mbuf *mbuf, uint8_t scratch[MRC_ENDPOINT_FRAME_CAPACITY],
                   struct mrc_wire_packet *packet, struct mrc_endpoint_rx_counters *counts)
{
    size_t length;
    counts->frames++;
    const uint8_t *frame = read_frame(mbuf, scratch, &length);
    if (!frame || decode_routed(transfer, index, frame, length, packet)) {
        counts->rejected++;
        return false;
    }
    if (!mrc_endpoint_peer_matches(transfer, index, packet)) {
        counts->other_peer++;
        return false;
    }
    counts->matched++;
    return true;
}

static bool next_port_status_token(const struct mrc_endpoint_config *transfer, struct mrc_endpoint_node *status,
                                   uint16_t *token)
{
    for (uint32_t attempts = 0; attempts <= UINT16_MAX; attempts++) {
        *token = ++status->token;
        bool pending = false;
        for (unsigned int path = 0; path < PATH_COUNT; path++)
            pending |= status->awaiting[path] && status->pending[path] == *token;
        if (!pending && (!transfer->port_status_token_in_use || !transfer->port_status_token_in_use(*token)))
            return true;
    }
    return false;
}

static void poll_port_status(const struct mrc_endpoint_config *transfer, struct mrc_endpoint_node *status,
                             uint64_t now)
{
    if (now < status->next_poll)
        return;
    if (!status->fast_poll_until)
        status->fast_poll_until = now + transfer->hz / 50;
    bool connecting = now < status->fast_poll_until || !status->local_ports;
    status->next_poll = now + transfer->hz / (connecting ? 1000 : 20);

    uint32_t active = 0, down = 0;
    for (unsigned int i = 0; i < PATH_COUNT; i++) {
        struct rte_eth_link link;
        if (rte_eth_link_get_nowait(transfer->paths[i].port, &link))
            continue;
        if (link.link_status)
            active |= 1u << i;
        else
            down |= 1u << i;
    }
    status->local_down = down;
    if (active == status->local_ports && now < status->next_announce)
        return;

    /* Publish fresh snapshots even when nothing changes. Neither endpoint
     * requests nor their acknowledgements have delivery guarantees. */
    status->local_ports = active;
    status->next_announce = now + transfer->hz / 4;
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        status->awaiting[path] = false;
        if (!(active & (1u << path)))
            continue;
        uint16_t token;
        if (!next_port_status_token(transfer, status, &token))
            continue;
        struct mrc_wire_packet request = mrc_endpoint_packet_for(transfer, path, MRC_ENDPOINT_REQUEST, token);
        request.body.endpoint_request = (struct mrc_wire_endpoint_request){
            .op = MRC_ENDPOINT_PORT_STATUS, .port_status_mask = active,
        };
        if (mrc_endpoint_transmit(transfer, path, &request)) {
            status->pending[path] = token;
            status->awaiting[path] = true;
        }
    }
}

bool mrc_endpoint_receive_op(const struct mrc_endpoint_config *transfer, struct mrc_endpoint_node *status,
                             struct mrc_endpoint_counters *counts, unsigned int path,
                             const struct mrc_wire_packet *packet, uint64_t arrival_cycles)
{
    if (packet->bth.opcode == MRC_ENDPOINT_RESPONSE) {
        if (packet->body.endpoint_response.op == MRC_ENDPOINT_PORT_STATUS) {
            for (unsigned int i = 0; i < PATH_COUNT; i++) {
                if (status->awaiting[i] && status->pending[i] == (uint16_t)packet->bth.psn) {
                    status->awaiting[i] = false;
                    counts->port_status_replies++;
                    break;
                }
            }
        }
        return true;
    }
    if (packet->bth.opcode != MRC_ENDPOINT_REQUEST)
        return false;

    const struct mrc_wire_endpoint_request *request = &packet->body.endpoint_request;
    if (request->op == MRC_ENDPOINT_PORT_STATUS) {
        /* The identifier is private to the peer; its value gives no
         * ordering guarantee. Refresh soft state on each valid report. */
        status->peer_ports = request->port_status_mask & ALL_PATHS;
        status->peer_valid_until = rte_get_timer_cycles() + transfer->hz;
        counts->port_status_updates++;
    }
    struct mrc_wire_packet response = mrc_endpoint_packet_for(transfer, path, MRC_ENDPOINT_RESPONSE,
                                                              (uint16_t)packet->bth.psn);
    response.body.endpoint_response = (struct mrc_wire_endpoint_response){
        .op = request->op,
        .tx_timestamp = mrc_endpoint_feedback_timestamp(transfer, arrival_cycles, request->timestamp,
                                                        request->tsr, request->tx_timestamp),
    };
    mrc_endpoint_transmit(transfer, path, &response);
    return true;
}

static uint32_t available_paths(const struct mrc_requester *sender, uint64_t now)
{
    const struct mrc_endpoint_node *node = sender->node;
    uint32_t local = ALL_PATHS & ~node->local_down;
    if (now < node->peer_valid_until)
        return local & node->peer_ports;
    return local;
}

static bool blackholed(const struct mrc_endpoint_config *transfer, const struct mrc_requester *sender,
                        unsigned int path)
{
    return path == 1 && transfer->drop_after != UINT32_MAX &&
           sender->scheduled_packets >= transfer->drop_after &&
           (transfer->restore_after == UINT32_MAX ||
            sender->scheduled_packets < transfer->restore_after);
}

static int reflected_path(const struct mrc_endpoint_config *transfer, uint32_t entropy)
{
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        if (entropy == path_entropy(transfer, path, true))
            return (int)path;
    }
    return -1;
}

static enum mrc_ev_feedback ev_mark(uint8_t mark)
{
    switch (mark) {
    case MRC_SACK_SKIP:
        return MRC_EV_SKIP_ONCE;
    case MRC_SACK_ALWAYS_SKIP:
        return MRC_EV_ALWAYS_SKIP;
    default:
        return MRC_EV_UNMARKED;
    }
}

static int send_write(const struct mrc_endpoint_config *transfer, struct mrc_requester *sender,
                       struct mrc_endpoint_counters *counts, unsigned int path, uint32_t index,
                       bool retry)
{
    uint32_t wr = request_index(transfer, index);
    const struct mrc_endpoint_write *work = &transfer->work[wr];
    struct mrc_provider_write_view write;
    int result = mrc_provider_qp_get_write(sender->provider_qp, sender->first_sequence + wr, &write);
    if (result)
        return result;
    if (write.length != work->length || write.with_immediate != work->with_immediate)
        return -EPROTO;

    uint32_t psn = mrc_psn_add(transfer->initial_psn, (int32_t)index);
    struct mrc_wire_packet packet = mrc_endpoint_packet_for(transfer, path, expected_opcode(work, index), psn);
    uint32_t length = length_at(work, index);
    uint32_t nominal_bytes = nominal_size(packet.bth.opcode, length, false);
    if (!mrc_cc_can_send(&sender->cc, nominal_bytes))
        return -EPROTO;
    packet.bth.retransmit = retry;
    packet.bth.solicited = write.solicited &&
        (packet.bth.opcode == MRC_WRITE_LAST_IMM || packet.bth.opcode == MRC_WRITE_ONLY_IMM);
    packet.bth.ack_request = retry || (index + 1) % BURST == 0 ||
                             index + 1 == transfer->packets ||
                             mrc_cc_ack_request(&sender->cc, nominal_bytes, SACK_THRESHOLD);
    uint32_t offset = (index - work->first_packet) * PMTU;
    packet.body.write = (struct mrc_wire_write){
        .rqmsn = write.with_immediate ?
            (uint16_t)(sender->rqmsn_base + work->preceding_immediates + 1) : 0,
        .msn = (uint16_t)mrc_psn_add(sender->msn_base, (int32_t)wr + 1),
        .va = write.remote_address + offset,
        .r_key = write.rkey,
        .dma_len = write.length,
        .imm_data = write.immediate,
        .data_len = length,
    };

    if (blackholed(transfer, sender, path)) {
        counts->dropped++;
    } else {
        uint8_t payload[PMTU];
        packet.body.write.data = mrc_provider_write_slice(&write, offset, length, payload);
        if (length && !packet.body.write.data)
            return -EPROTO;
        bool inject_trim = path == 1 && transfer->trim_after != UINT32_MAX &&
                           sender->scheduled_packets >= transfer->trim_after;
        if (!transmit_frame(transfer, path, &packet, inject_trim))
            return -EAGAIN;
        counts->trimmed += inject_trim;
    }

    /* A rejected TX burst leaves the congestion window accounting untouched. */
    if (!mrc_cc_on_send(&sender->cc, nominal_bytes))
        return -EPROTO;
    if (retry)
        counts->retries[path]++;
    else
        counts->sent[path]++;
    return 0;
}

static bool retire_redundant_attempt(const struct mrc_endpoint_config *transfer, struct mrc_requester *sender,
                                      uint32_t index)
{
    struct send_record *record = &sender->metadata[index % WINDOW];
    if (!record->counted_inflight || !record->redundant_attempt)
        return true;

    /* A retry of already delivered data never increments the responder's byte counter. */
    if (!mrc_cc_on_nack(&sender->cc, nominal_at(transfer, index), false, false, 0, NULL))
        return false;
    record->counted_inflight = false;
    return true;
}

static int fail_send(const struct mrc_endpoint_config *transfer, struct mrc_requester *sender, uint32_t packet_index,
                      enum mrc_provider_wc_status status, int error)
{
    uint64_t sequence = sender->first_sequence + request_index(transfer, packet_index);
    mrc_provider_qp_fail(sender->provider_qp, sequence, status);
    return error;
}

static enum mrc_provider_wc_status aeth_error_status(uint8_t syndrome)
{
    switch (syndrome) {
    case INVALID_REQUEST_NAK:
        return MRC_PROVIDER_WC_REMOTE_INVALID_REQUEST;
    case REMOTE_ACCESS_NAK:
        return MRC_PROVIDER_WC_REMOTE_ACCESS;
    case REMOTE_OPERATION_NAK:
        return MRC_PROVIDER_WC_REMOTE_OPERATION;
    default:
        return MRC_PROVIDER_WC_BAD_RESPONSE;
    }
}

static int match_probe(const struct mrc_requester *sender, const struct mrc_wire_sack *sack)
{
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        const struct outstanding_probe *probe = &sender->probes[path];
        if (probe->pending && probe->id == (uint16_t)sack->ack_psn_offset &&
            probe->entropy == sack->entropy)
            return (int)path;
    }
    return -1;
}

static int apply_control(const struct mrc_endpoint_config *transfer, struct mrc_requester *sender,
                         struct mrc_endpoint_counters *counts, const struct mrc_wire_packet *packet,
                         uint64_t arrival_cycles)
{
    uint64_t now_cycles = rte_get_timer_cycles();
    uint64_t now = cycles_to_ns(transfer, now_cycles);
    switch (packet->bth.opcode) {
    case MRC_TRANSPORT_ACK: {
        const struct mrc_wire_aeth *ack = &packet->body.aeth;
        int32_t last = mrc_psn_delta(packet->bth.psn, transfer->initial_psn);
        int32_t first = mrc_psn_delta(sender->qp.first_psn, transfer->initial_psn);
        if (last < first || last >= (int32_t)sender->next)
            return 0;
        int32_t msn = mrc_psn_delta(ack->msn, sender->msn_base);
        if (ack->syndrome != ACK_SYNDROME) {
            fprintf(stderr, "Transport rejected WRITE, syndrome %#x\n", ack->syndrome);
            uint32_t failed = (uint32_t)last;
            if (ack->syndrome == REMOTE_OPERATION_NAK &&
                msn >= (int32_t)request_index(transfer, (uint32_t)first) &&
                msn <= (int32_t)request_index(transfer, (uint32_t)last)) {
                uint32_t start = transfer->work[msn].first_packet;
                failed = start < (uint32_t)first ? (uint32_t)first : start;
            }
            return fail_send(transfer, sender, failed, aeth_error_status(ack->syndrome), -EPROTO);
        }
        uint32_t completed = request_index(transfer, (uint32_t)last + 1);
        if (msn != (int32_t)completed)
            return fail_send(transfer, sender, (uint32_t)last, MRC_PROVIDER_WC_BAD_RESPONSE, -EPROTO);
        for (int32_t index = first; index <= last; index++) {
            if (!retire_redundant_attempt(transfer, sender, (uint32_t)index))
                return -EPROTO;
        }
        if (!mrc_tx_apply_transport_ack(&sender->qp, packet->bth.psn))
            return 0;
        counts->transport_acks++;
        return completed > request_index(transfer, (uint32_t)first)
               ? mrc_provider_qp_complete_send(sender->provider_qp, sender->first_sequence + completed - 1) : 0;
    }
    case MRC_RELIABILITY_SACK: {
        const struct mrc_wire_sack *wire = &packet->body.sack;
        if (wire->spdcid != (uint16_t)transfer->requestor_qpn || wire->dpdcid != (uint16_t)transfer->responder_qpn ||
            (!wire->probe_response && packet->bth.psn != wire->cack_psn))
            return 0;
        int probe_path = -1;
        if (wire->probe_response) {
            probe_path = match_probe(sender, wire);
            if (probe_path < 0)
                return 0;
        }
        struct mrc_sack sack = {
            .cack_psn = wire->cack_psn, .ack_psn_offset = wire->ack_psn_offset,
            .sack_offset = wire->sack_offset, .bitmap = wire->bitmap, .ooo_count = wire->ooo_count,
        };
        uint32_t previous_cack = sender->qp.delivery_cack_psn;
        if (!mrc_tx_apply_sack(&sender->qp, &sack, wire->probe_response))
            return 0;
        enum mrc_mpr_update_result update = mrc_mpr_update(
            &sender->mpr, previous_cack, sack.cack_psn, wire->mpr);
        if (update == MRC_MPR_CHANGED)
            counts->mpr_updates++;
        else if (update == MRC_MPR_ZERO || update == MRC_MPR_EXCEEDS_MAX)
            fprintf(stderr, "Ignoring invalid dynamic MPR %u at cumulative PSN %u\n",
                    wire->mpr, sack.cack_psn);
        counts->sacks++;
        struct mrc_cc_tx_sample sample;
        struct mrc_cc_tx_sample *rtt_sample = NULL;
        if (probe_path >= 0) {
            struct outstanding_probe *probe = &sender->probes[probe_path];
            sample = (struct mrc_cc_tx_sample){
                .sent_at_ns = cycles_to_ns(transfer, probe->sent_at),
                .arrived_at_ns = cycles_to_ns(transfer, arrival_cycles),
                .service_time_ns = peer_service_ns(transfer, wire->tx_timestamp),
            };
            rtt_sample = &sample;
            mrc_ev_feedback(&sender->ev, (unsigned int)probe_path, ev_mark(wire->mark),
                            true, probe->sent_at, now_cycles);
            probe->pending = false;
            sender->initial_probe_paths &= ~(UINT32_C(1) << probe_path);
            counts->probe_replies++;
        } else {
            int ev = reflected_path(transfer, wire->entropy);
            if (ev >= 0)
                mrc_ev_feedback(&sender->ev, (unsigned int)ev, ev_mark(wire->mark),
                                false, 0, now_cycles);
            uint32_t ack_psn = mrc_psn_add(wire->cack_psn, wire->ack_psn_offset);
            const struct mrc_tx_packet *sent = mrc_tx_packet(&sender->qp, ack_psn);
            if (sent) {
                uint32_t index = (uint32_t)mrc_psn_delta(ack_psn, transfer->initial_psn);
                if (packet->bth.retransmit &&
                    !retire_redundant_attempt(transfer, sender, index))
                    return -EPROTO;
                sample = (struct mrc_cc_tx_sample){
                    .sent_at_ns = cycles_to_ns(transfer, sent->sent_at),
                    .arrived_at_ns = cycles_to_ns(transfer, arrival_cycles),
                    .service_time_ns = peer_service_ns(transfer, wire->tx_timestamp),
                    .retransmissions = sent->retransmissions,
                };
                rtt_sample = &sample;
            }
        }
        const struct mrc_cc_receiver_flow flow = {
            .penalty = wire->rcv_cwnd_pen, .restore = wire->restore_cwnd,
        };
        if (!mrc_cc_on_sack(&sender->cc, wire->rcvd_bytes, wire->mark == MRC_SACK_SKIP,
                            packet->bth.retransmit, now, rtt_sample, &flow, NULL))
            return -EPROTO;

        int32_t highest = mrc_psn_delta(sack.cack_psn, transfer->initial_psn);
        int32_t cack = highest;
        if (sack.bitmap) {
            int32_t bitmap_end = cack + sack.sack_offset + 63 -
                                 (int32_t)__builtin_clzll(sack.bitmap);
            if (bitmap_end > highest && bitmap_end < (int32_t)sender->next)
                highest = bitmap_end;
        }
        int32_t trigger = cack + sack.ack_psn_offset;
        if (!wire->probe_response && trigger > highest && trigger < (int32_t)sender->next)
            highest = trigger;
        if (highest > sender->highest_sacked && highest < (int32_t)sender->next)
            sender->highest_sacked = highest;
        return 0;
    }
    case MRC_RELIABILITY_NACK: {
        const struct mrc_wire_nack *nack = &packet->body.nack;
        struct mrc_tx_packet *state = mrc_tx_packet(&sender->qp, nack->nack_psn);
        if (nack->spdcid != (uint16_t)transfer->requestor_qpn || nack->dpdcid != (uint16_t)transfer->responder_qpn ||
            packet->bth.psn != nack->nack_psn || !state)
            return 0;
        if (nack->reason == MRC_NACK_UNEXP_EVENT) {
            counts->nack_received_mask |= UINT32_C(1) << nack->reason;
            fprintf(stderr, "Unexpected remote event at PSN %" PRIu32 "\n", nack->nack_psn);
            uint32_t index = (uint32_t)mrc_psn_delta(nack->nack_psn, transfer->initial_psn);
            return fail_send(transfer, sender, index, MRC_PROVIDER_WC_REMOTE_OPERATION, -EREMOTEIO);
        }
        if (state->received)
            return 0;
        switch (nack->reason) {
        case MRC_NACK_TRIMMED:
        case MRC_NACK_TRIMMED_LASTHOP:
        case MRC_NACK_NO_BITMAP:
        case MRC_NACK_NO_PKT_BUFFER:
        case MRC_NACK_NO_RESOURCE:
        case MRC_NACK_PSN_OOR_WINDOW: {
            uint32_t index = (uint32_t)mrc_psn_delta(nack->nack_psn, transfer->initial_psn);
            struct send_record *record = &sender->metadata[index % WINDOW];
            if (record->counted_inflight &&
                packet->bth.retransmit == (state->retransmissions != 0)) {
                struct mrc_cc_tx_sample sample = {
                    .sent_at_ns = cycles_to_ns(transfer, state->sent_at),
                    .arrived_at_ns = cycles_to_ns(transfer, arrival_cycles),
                    .service_time_ns = peer_service_ns(transfer, nack->tx_timestamp),
                    .retransmissions = state->retransmissions,
                };
                bool trimmed = nack->reason == MRC_NACK_TRIMMED ||
                               nack->reason == MRC_NACK_TRIMMED_LASTHOP;
                int ev = reflected_path(transfer, nack->entropy);
                if (ev >= 0 && nack->reason == MRC_NACK_TRIMMED)
                    mrc_ev_feedback(&sender->ev, (unsigned int)ev, MRC_EV_SKIP_ONCE,
                                    false, 0, now_cycles);
                if (!mrc_cc_on_nack(&sender->cc, nominal_at(transfer, index), trimmed,
                                    packet->bth.retransmit, now, &sample))
                    return -EPROTO;
                record->counted_inflight = false;
                record->nacked = true;
                counts->nack_received_mask |= UINT32_C(1) << nack->reason;
                counts->trim_nacks += trimmed;
            }
            return 0;
        }
        default:
            return 0;
        }
    }
    default:
        return 0;
    }
}

static uint16_t receive_burst(const struct mrc_endpoint_config *config, unsigned int path,
                               struct mrc_endpoint_rx_packet *packets)
{
    if (config->receive)
        return config->receive(config->receive_context, path, packets, BURST);
    struct rte_mbuf *mbufs[BURST];
    uint16_t count = rte_eth_rx_burst(config->paths[path].port, 0, mbufs, BURST);
    uint64_t arrival_cycles = count ? rte_get_timer_cycles() : 0;
    for (uint16_t i = 0; i < count; i++)
        packets[i] = (struct mrc_endpoint_rx_packet){ .mbuf = mbufs[i], .arrival_cycles = arrival_cycles };
    return count;
}

static int receive_control(const struct mrc_endpoint_config *transfer, struct mrc_requester *sender,
                           struct mrc_endpoint_counters *counts)
{
    int result = 0;
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        struct mrc_endpoint_rx_packet packets[BURST];
        uint16_t n = receive_burst(transfer, path, packets);
        for (uint16_t i = 0; i < n; i++) {
            const struct mrc_endpoint_rx_packet *received = &packets[i];
            uint8_t scratch[MRC_ENDPOINT_FRAME_CAPACITY];
            struct mrc_wire_packet packet;
            if (!result && decode(transfer, path, received->mbuf, scratch, &packet, &counts->rx[path]) &&
                !mrc_endpoint_receive_op(transfer, sender->node, counts, path, &packet, received->arrival_cycles))
                result = apply_control(transfer, sender, counts, &packet, received->arrival_cycles);
            rte_pktmbuf_free(received->mbuf);
        }
    }
    return result;
}

uint64_t mrc_endpoint_cycles(const struct mrc_endpoint_config *transfer, uint64_t ns)
{
    return (ns / 1000000000) * transfer->hz +
           (ns % 1000000000) * transfer->hz / 1000000000;
}

static int retry_due_packets(const struct mrc_endpoint_config *transfer, struct mrc_requester *sender,
                             struct mrc_endpoint_counters *counts, uint32_t first, uint64_t now)
{
    for (uint32_t index = first; index < sender->next; index++) {
        uint32_t psn = mrc_psn_add(transfer->initial_psn, (int32_t)index);
        struct mrc_tx_packet *packet = mrc_tx_packet(&sender->qp, psn);
        struct send_record *record = &sender->metadata[index % WINDOW];
        uint64_t timeout_ns;
        if (!mrc_local_ack_timeout_ns(transfer->timeout_exponent,
                transfer->linear_retries, transfer->exponential_retries,
                packet->retransmissions, &timeout_ns))
            return -EPROTO;
        uint64_t age = now - packet->sent_at;
        if (packet->received && index != first)
            continue;
        bool fast = !packet->received && !record->fast_retransmitted &&
                    sender->highest_sacked - (int32_t)index >= FAST_LOSS_GAP &&
                    age >= transfer->initial_rto / 4;
        if (!fast && !record->nacked && age < mrc_endpoint_cycles(transfer, timeout_ns))
            continue;
        if (!retire_redundant_attempt(transfer, sender, index))
            return -EPROTO;
        if (!packet->received && record->counted_inflight) {
            if (!mrc_cc_on_inferred_loss(&sender->cc, nominal_at(transfer, index)))
                return -EPROTO;
            record->counted_inflight = false;
        }
        if (!mrc_mpr_can_transmit(&sender->mpr, sender->qp.delivery_cack_psn, psn))
            continue;
        if (transfer->exponential_retries != 25 && packet->retransmissions >=
                transfer->linear_retries + transfer->exponential_retries) {
            fprintf(stderr, "Retry limit exhausted for PSN %" PRIu32
                            " (first segment %u, next %u, last transmit path %u)\n",
                    psn, first, sender->next, record->path);
            return fail_send(transfer, sender, index, MRC_PROVIDER_WC_RETRY_EXCEEDED, -ETIMEDOUT);
        }
        if (!mrc_cc_can_send(&sender->cc, nominal_at(transfer, index)))
            continue;

        if (!packet->received && !record->nacked &&
            packet->sent_at >= sender->ev.paths[record->path].changed_at)
            mrc_ev_timeout(&sender->ev, record->path, now);

        uint32_t available = available_paths(sender, now);
        uint32_t avoid = UINT32_C(1) << record->path;
        int picked = mrc_ev_pick(&sender->ev, available, avoid, now);
        if (picked < 0)
            continue;
        unsigned int path = (unsigned int)picked;
        int sent = send_write(transfer, sender, counts, path, index, true);
        if (sent == -EAGAIN) {
            available &= ~(UINT32_C(1) << path);
            picked = mrc_ev_pick(&sender->ev, available, avoid, now);
            if (picked < 0)
                continue;
            path = (unsigned int)picked;
            sent = send_write(transfer, sender, counts, path, index, true);
            if (sent == -EAGAIN)
                continue;
        }
        if (sent)
            return sent;
        mrc_ev_sent(&sender->ev, path);
        if (!mrc_tx_note_retransmit(&sender->qp, psn, now))
            return -EPROTO;
        record->path = path;
        record->counted_inflight = true;
        record->redundant_attempt = packet->received;
        record->nacked = false;
        record->fast_retransmitted |= fast;
        counts->fast_retries += fast;
    }
    return 0;
}

static bool request_probe(const struct mrc_endpoint_config *transfer, struct mrc_requester *sender,
                           struct mrc_endpoint_counters *counts, unsigned int path, uint64_t now)
{
    uint16_t probe_id = sender->probe_id;
    bool outstanding;
    do {
        outstanding = false;
        probe_id++;
        for (unsigned int i = 0; i < PATH_COUNT; i++)
            outstanding |= sender->probes[i].pending && sender->probes[i].id == probe_id;
    } while (outstanding);
    struct mrc_wire_packet packet = mrc_endpoint_packet_for(transfer, path, MRC_RELIABILITY_PROBE, 0);
    packet.body.probe = (struct mrc_wire_probe){
        .probe_id = probe_id, .spdcid = (uint16_t)transfer->requestor_qpn,
        .dpdcid = (uint16_t)transfer->responder_qpn,
    };
    if (!blackholed(transfer, sender, path) && !mrc_endpoint_transmit(transfer, path, &packet))
        return false;
    sender->probe_id = probe_id;
    sender->probes[path] = (struct outstanding_probe){
        .sent_at = now, .entropy = reflect_entropy(&packet), .id = probe_id, .pending = true,
    };
    counts->probe_requests++;
    counts->probe_sent[path]++;
    return true;
}

static void update_ev_denials(struct mrc_requester *sender, uint32_t current, uint32_t updated,
                              uint64_t now)
{
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        uint32_t bit = UINT32_C(1) << path;
        if ((current ^ updated) & bit) {
            mrc_ev_deny(&sender->ev, path, (updated & bit) != 0, now);
            sender->probes[path].pending = false;
        }
    }
}

static void send_ack(const struct mrc_endpoint_config *transfer, struct mrc_endpoint_counters *counts,
                     unsigned int path, uint32_t psn, uint8_t syndrome, uint32_t msn)
{
    if (syndrome == ACK_SYNDROME && msn == transfer->work_requests &&
        counts->dropped_transport_acks < transfer->final_ack_drops) {
        counts->dropped_transport_acks++;
        return;
    }
    struct mrc_wire_packet packet = mrc_endpoint_packet_for(transfer, path, MRC_TRANSPORT_ACK, psn);
    packet.body.aeth = (struct mrc_wire_aeth){ .syndrome = syndrome, .msn = msn };
    if (mrc_endpoint_transmit(transfer, path, &packet))
        counts->transport_acks++;
}

static void send_sack(const struct mrc_endpoint_config *transfer, struct mrc_responder *receiver,
                      struct mrc_endpoint_counters *counts, unsigned int path,
                      const struct mrc_wire_packet *request, bool probe, uint64_t arrival_cycles)
{
    struct mrc_sack state;
    if (probe)
        mrc_rx_build_probe_sack(&receiver->qp, request->body.probe.probe_id, &state);
    else if (!mrc_rx_build_sack(&receiver->qp, request->bth.psn, &state))
        return;

    struct mrc_wire_packet packet = mrc_endpoint_packet_for(transfer, path, MRC_RELIABILITY_SACK,
                                                            probe ? 0 : state.cack_psn);
    packet.bth.retransmit = !probe && request->bth.retransmit;
    packet.body.sack = (struct mrc_wire_sack){
        .mark = packet_ecn(request) == 3 ? MRC_SACK_SKIP : MRC_SACK_NONE,
        .probe_response = probe,
        .ack_psn_offset = state.ack_psn_offset,
        .entropy = reflect_entropy(request),
        .spdcid = (uint16_t)transfer->requestor_qpn,
        .dpdcid = (uint16_t)transfer->responder_qpn,
        .cack_psn = state.cack_psn, .mpr = mrc_mpr_advertised(&receiver->mpr),
        .sack_offset = state.sack_offset,
        .bitmap = state.bitmap, .ooo_count = state.ooo_count,
        .rcvd_bytes = mrc_cc_rx_wire_count(&receiver->cc),
        .rcv_cwnd_pen = receiver->flow.penalty, .restore_cwnd = receiver->flow.restore,
    };
    const struct mrc_wire_probe *probe_request = &request->body.probe;
    packet.body.sack.tx_timestamp = probe ?
        mrc_endpoint_feedback_timestamp(transfer, arrival_cycles, probe_request->timestamp,
                                        probe_request->tsr, probe_request->tx_timestamp) :
        write_feedback_timestamp(transfer, request, arrival_cycles);
    if (mrc_endpoint_transmit(transfer, path, &packet)) {
        counts->sacks++;
        mrc_rx_sack_sent(&receiver->qp, &state);
    }
}

static bool send_nack(const struct mrc_endpoint_config *transfer, struct mrc_endpoint_counters *counts, unsigned int path,
                      const struct mrc_wire_bth *request, uint32_t entropy, uint8_t reason,
                      uint16_t original_udp_len, uint16_t tx_timestamp)
{
    struct mrc_wire_packet nack = mrc_endpoint_packet_for(transfer, path, MRC_RELIABILITY_NACK, request->psn);
    nack.bth.retransmit = request->retransmit;
    nack.body.nack = (struct mrc_wire_nack){
        .reason = reason, .entropy = entropy,
        .spdcid = (uint16_t)transfer->requestor_qpn, .dpdcid = (uint16_t)transfer->responder_qpn,
        .nack_psn = request->psn, .tx_timestamp = tx_timestamp,
        .original_udp_len = original_udp_len,
    };
    if (!mrc_endpoint_transmit(transfer, path, &nack))
        return false;
    counts->nack_sent_mask |= UINT32_C(1) << reason;
    return true;
}

static uint8_t write_error_syndrome(enum mrc_wr_result result)
{
    switch (result) {
    case MRC_WR_REMOTE_ACCESS:
        return REMOTE_ACCESS_NAK;
    case MRC_WR_RQ_EMPTY:
        return REMOTE_OPERATION_NAK;
    default:
        return INVALID_REQUEST_NAK;
    }
}

static enum mrc_wr_result advance_writes(struct mrc_responder *receiver)
{
    bool produced;
    enum mrc_wr_result result;
    do {
        struct mrc_wr_completion completion;
        result = mrc_wr_advance(&receiver->wr, receiver->qp.cack_psn, &completion, &produced);
    } while (result == MRC_WR_OK && produced);
    return result;
}

static int reject_write(const struct mrc_endpoint_config *transfer, const struct mrc_responder *receiver,
                         struct mrc_endpoint_counters *counts, unsigned int path,
                         const struct mrc_wire_packet *packet, enum mrc_wr_result result,
                         uint32_t ack_psn, uint64_t arrival_cycles)
{
    uint8_t reason;
    switch (result) {
    case MRC_WR_NO_RESOURCE:
        reason = MRC_NACK_NO_RESOURCE;
        break;
    case MRC_WR_NO_PKT_BUFFER:
        reason = MRC_NACK_NO_PKT_BUFFER;
        break;
    case MRC_WR_QP_ERROR:
        mrc_provider_qp_fail(receiver->provider_qp, 0, MRC_PROVIDER_WC_FLUSHED);
        reason = MRC_NACK_UNEXP_EVENT;
        break;
    default:
        mrc_provider_qp_fail(receiver->provider_qp, 0, MRC_PROVIDER_WC_FLUSHED);
        send_ack(transfer, counts, path, ack_psn, write_error_syndrome(result),
                 mrc_wr_ack_msn(&receiver->wr));
        return -EPROTO;
    }
    send_nack(transfer, counts, path, &packet->bth, reflect_entropy(packet), reason,
              0, write_feedback_timestamp(transfer, packet, arrival_cycles));
    return result == MRC_WR_QP_ERROR ? -EPROTO : 0;
}

static int receive_write(const struct mrc_endpoint_config *transfer, struct mrc_responder *receiver,
                         struct mrc_endpoint_counters *counts, unsigned int path,
                         const struct mrc_wire_packet *packet, uint64_t arrival_cycles)
{
    if (packet->bth.opcode == MRC_RELIABILITY_PROBE) {
        if (packet->body.probe.spdcid == (uint16_t)transfer->requestor_qpn &&
            packet->body.probe.dpdcid == (uint16_t)transfer->responder_qpn) {
            counts->probe_requests++;
            send_sack(transfer, receiver, counts, path, packet, true, arrival_cycles);
        }
        return 0;
    }
    if (!mrc_wire_is_write(packet->bth.opcode))
        return 0;

    uint32_t psn = packet->bth.psn;
    const struct mrc_wire_write *write = &packet->body.write;
    enum mrc_rx_disposition disposition = mrc_rx_classify(&receiver->qp, psn);
    if (disposition == MRC_RX_OUTSIDE_WINDOW) {
        send_nack(transfer, counts, path, &packet->bth, reflect_entropy(packet),
                  MRC_NACK_PSN_OOR_WINDOW, 0, write_feedback_timestamp(transfer, packet, arrival_cycles));
        return 0;
    }
    if (disposition == MRC_RX_DUPLICATE) {
        counts->duplicates++;
        send_sack(transfer, receiver, counts, path, packet, false, arrival_cycles);
        send_ack(transfer, counts, path, receiver->qp.cack_psn, ACK_SYNDROME,
                 mrc_wr_ack_msn(&receiver->wr));
        return 0;
    }

    if (receiver->qp.ooo_count >= transfer->rx_ooo_capacity &&
        psn != mrc_psn_add(receiver->qp.cack_psn, 1)) {
        send_nack(transfer, counts, path, &packet->bth, reflect_entropy(packet),
                  MRC_NACK_NO_BITMAP, 0, write_feedback_timestamp(transfer, packet, arrival_cycles));
        return 0;
    }

    uint32_t index = (uint32_t)mrc_psn_delta(psn, transfer->initial_psn);
    if (transfer->work && (index >= transfer->packets ||
        write->dma_len != transfer->work[request_index(transfer, index)].length))
        return reject_write(transfer, receiver, counts, path, packet, MRC_WR_INVALID, psn, arrival_cycles);

    enum mrc_wr_result result = mrc_wr_receive(&receiver->wr, psn, packet->bth.opcode, write,
                                               packet->bth.solicited);
    if (result != MRC_WR_OK)
        return reject_write(transfer, receiver, counts, path, packet, result, psn, arrival_cycles);

    uint32_t previous_cack = receiver->qp.cack_psn;
    int32_t distance = mrc_psn_delta(receiver->qp.max_rcv_psn, psn);
    if (distance > 0 && (uint32_t)distance > counts->reorder_max)
        counts->reorder_max = (uint32_t)distance;

    unsigned int flags = (packet->bth.ack_request ? MRC_RX_AR : 0) |
                         (packet->bth.retransmit ? MRC_RX_RTX : 0) |
                         (packet_ecn(packet) == 3 ? MRC_RX_ECN : 0);
    bool sack_due;
    if (mrc_rx_record(&receiver->qp, psn, (uint32_t)write->data_len, flags,
                      SACK_THRESHOLD, &sack_due) != MRC_RX_NEW)
        return reject_write(transfer, receiver, counts, path, packet, MRC_WR_QP_ERROR, psn, arrival_cycles);
    uint32_t nominal = nominal_size(packet->bth.opcode, write->data_len, packet->bth.timestamp);
    mrc_cc_rx_record(&receiver->cc, nominal, true, false);
    receiver->unique_packets++;
    if (receiver->unique_packets == transfer->mpr_change_at &&
        mrc_mpr_set_advertised(&receiver->mpr, transfer->mpr_change_to)) {
        counts->mpr_updates++;
        sack_due = true;
    }
    counts->received[path]++;
    bool complete = transfer->packets && receiver->unique_packets == transfer->packets;
    result = advance_writes(receiver);
    if (result != MRC_WR_OK)
        return reject_write(transfer, receiver, counts, path, packet, result, receiver->qp.cack_psn, arrival_cycles);

    uint32_t msn = mrc_wr_ack_msn(&receiver->wr);
    if (sack_due || complete)
        send_sack(transfer, receiver, counts, path, packet, false, arrival_cycles);
    if (previous_cack != receiver->qp.cack_psn && (sack_due || complete))
        send_ack(transfer, counts, path, receiver->qp.cack_psn, ACK_SYNDROME, msn);
    return 0;
}

static void receive_trimmed(const struct mrc_endpoint_config *transfer, const struct mrc_responder *receiver,
                            struct mrc_endpoint_counters *counts, unsigned int path,
                            const struct mrc_wire_trimmed *trimmed, uint32_t entropy, uint64_t arrival_cycles)
{
    enum mrc_rx_disposition disposition = mrc_rx_classify(&receiver->qp, trimmed->bth.psn);
    if (disposition == MRC_RX_OUTSIDE_WINDOW) {
        uint16_t timestamp = mrc_endpoint_feedback_timestamp(transfer, arrival_cycles, trimmed->timestamp,
                                                             trimmed->tsr, trimmed->tx_timestamp);
        send_nack(transfer, counts, path, &trimmed->bth, entropy, MRC_NACK_PSN_OOR_WINDOW,
                  0, timestamp);
        return;
    }
    if (disposition == MRC_RX_DUPLICATE || !mrc_cc_nominal_size(trimmed->original_udp_len))
        return;

    counts->trimmed++;
    if (transfer->trim_nack_enabled) {
        uint16_t timestamp = mrc_endpoint_feedback_timestamp(transfer, arrival_cycles, trimmed->timestamp,
                                                             trimmed->tsr, trimmed->tx_timestamp);
        if (send_nack(transfer, counts, path, &trimmed->bth, entropy, MRC_NACK_TRIMMED,
                      trimmed->original_udp_len, timestamp))
            counts->trim_nacks++;
    }
}

static int receive_frame(const struct mrc_endpoint_config *transfer, struct mrc_responder *receiver,
                         struct mrc_endpoint_counters *counts, unsigned int path,
                         const struct mrc_endpoint_rx_packet *received)
{
    uint64_t arrival_cycles = received->arrival_cycles;
    struct mrc_endpoint_rx_counters *rx = &counts->rx[path];
    rx->frames++;
    uint8_t scratch[MRC_ENDPOINT_FRAME_CAPACITY];
    size_t length;
    const uint8_t *frame = read_frame(received->mbuf, scratch, &length);
    if (!frame) {
        rx->rejected++;
        return 0;
    }

    struct mrc_wire_trimmed trimmed;
    struct mrc_wire_srv6 route;
    int result = mrc_wire_decode_trimmed_srv6(frame, length, transfer->trim_dscp, &trimmed, &route);
    struct mrc_wire_packet packet;
    if (result != -EPROTONOSUPPORT) {
        if (result || !mrc_endpoint_route_matches(transfer, path, &route)) {
            rx->rejected++;
        } else {
            packet.bth = trimmed.bth;
            packet.ip_version = trimmed.ip_version;
            if (trimmed.ip_version == MRC_WIRE_IP6)
                packet.ipv6 = trimmed.ipv6;
            else
                packet.ipv4 = trimmed.ipv4;
            if (mrc_endpoint_peer_matches(transfer, path, &packet)) {
                rx->matched++;
                receiver->last_frame = rte_get_timer_cycles();
                receive_trimmed(transfer, receiver, counts, path, &trimmed, reflect_entropy(&packet),
                                arrival_cycles);
            } else {
                rx->other_peer++;
            }
        }
        return 0;
    }

    result = decode_routed(transfer, path, frame, length, &packet);
    if (!result) {
        if (!mrc_endpoint_peer_matches(transfer, path, &packet)) {
            rx->other_peer++;
            return 0;
        }
        rx->matched++;
        if (mrc_endpoint_receive_op(transfer, receiver->node, counts, path, &packet, arrival_cycles))
            return 0;
        receiver->last_frame = rte_get_timer_cycles();
        return receive_write(transfer, receiver, counts, path, &packet, arrival_cycles);
    }

    if (result == -EPROTONOSUPPORT &&
        !mrc_wire_decode_unsupported_opcode(frame, length, &packet, &route) &&
        mrc_endpoint_route_matches(transfer, path, &route) && mrc_endpoint_peer_matches(transfer, path, &packet)) {
        rx->matched++;
        return reject_write(transfer, receiver, counts, path, &packet, MRC_WR_INVALID,
                            packet.bth.psn, arrival_cycles);
    }
    rx->rejected++;
    return 0;
}

bool mrc_endpoint_append_write(struct mrc_endpoint_config *transfer, size_t capacity,
                               uint32_t length, bool with_immediate,
                               uint32_t immediate)
{
    if (!transfer || !transfer->work || transfer->work_requests >= capacity ||
        (uint64_t)transfer->bytes + length > (1U << 30))
        return false;
    uint32_t packets = length ? 1 + (length - 1) / PMTU : 1;
    if ((uint64_t)transfer->packets + packets >= MRC_PSN_HALF)
        return false;

    transfer->work[transfer->work_requests++] = (struct mrc_endpoint_write){
        .first_packet = transfer->packets, .packets = packets, .offset = transfer->bytes,
        .length = length, .preceding_immediates = transfer->immediate_requests,
        .with_immediate = with_immediate, .immediate = immediate,
    };
    transfer->bytes += length;
    transfer->packets += packets;
    transfer->immediate_requests += with_immediate;
    return true;
}

static int load_requester_batch(struct mrc_requester *sender,
                                const struct mrc_provider_qp_attr *attr)
{
    struct mrc_endpoint_config *transfer = &sender->transfer;
    size_t pending = attr->send_pending;
    if (pending > MAX_WORK_REQUESTS)
        pending = MAX_WORK_REQUESTS;
    if (pending > sender->work_capacity) {
        struct mrc_endpoint_write *work = realloc(transfer->work, pending * sizeof(*work));
        if (!work)
            return -ENOMEM;
        transfer->work = work;
        sender->work_capacity = pending;
    }

    sender->first_sequence = attr->send_first_sequence;
    for (size_t wr = 0; wr < pending; wr++) {
        struct mrc_provider_write_view write;
        int result = mrc_provider_qp_get_write(sender->provider_qp, sender->first_sequence + wr, &write);
        if (result)
            return result;
        if (write.with_immediate && !transfer->max_wimm_inflight)
            return -EINVAL;
        if (!mrc_endpoint_append_write(transfer, pending, write.length,
                                       write.with_immediate, write.immediate)) {
            if (!transfer->continuous_send || !wr)
                return -EINVAL;
            break;
        }
        uint32_t packets = transfer->work[wr].packets;
        if (!wr)
            transfer->uniform_wr_packets = packets;
        else if (packets != transfer->uniform_wr_packets)
            transfer->uniform_wr_packets = 0;
    }
    return 0;
}

static void reset_requester_batch(struct mrc_requester *sender)
{
    struct mrc_endpoint_config *transfer = &sender->transfer;
    transfer->initial_psn = sender->qp.first_psn;
    transfer->packets = 0;
    transfer->bytes = 0;
    transfer->work_requests = 0;
    transfer->immediate_requests = 0;
    transfer->uniform_wr_packets = 0;
    sender->next = 0;
    sender->highest_sacked = -1;
}

int mrc_requester_create(const struct mrc_endpoint_config *configuration, struct mrc_provider_qp *qp,
                         struct mrc_endpoint_counters *counts, struct mrc_requester **requester)
{
    if (!configuration || !qp || !counts || !requester || !configuration->pool ||
        !configuration->hz || !configuration->initial_rto ||
        configuration->window > WINDOW)
        return -EINVAL;
    struct mrc_provider_qp_attr attr;
    int result = mrc_provider_qp_query(qp, &attr);
    if (result)
        return result;
    if (attr.state != MRC_PROVIDER_QP_RTS || attr.qpn != configuration->requestor_qpn ||
        (!configuration->continuous_send &&
         (!attr.send_pending || attr.send_pending > MAX_WORK_REQUESTS)))
        return -EINVAL;

    struct mrc_requester *sender = calloc(1, sizeof(*sender));
    if (!sender)
        return -ENOMEM;
    sender->event_fd = -1;
    sender->counts = counts;
    sender->provider_qp = qp;
    sender->initial_probe_paths = ALL_PATHS;
    sender->transfer = *configuration;
    sender->node = configuration->node ? configuration->node : &sender->local_node;
    struct mrc_endpoint_config *transfer = &sender->transfer;
    transfer->sender = true;
    transfer->work = NULL;
    if (!mrc_tx_init(&sender->qp, transfer->initial_psn, transfer->window, sender->slots) ||
        !mrc_cc_init(&sender->cc, MRC_NOMINAL_MTU, INITIAL_CWND, 0, TARGET_QUEUE_NS) ||
        (transfer->cc_profile.id &&
         !mrc_cc_apply_config(&sender->cc, transfer->cc_profile.configured ?
                              &transfer->cc_profile.config : NULL)) ||
        !mrc_ev_init(&sender->ev, PATH_COUNT, transfer->profile.config.min_active,
                     transfer->initial_rto / 8 + 1, transfer->initial_rto / 2 + 1) ||
        !mrc_mpr_init(&sender->mpr, transfer->dynamic_mpr, transfer->mpr_max)) {
        result = -EINVAL;
        goto failed;
    }
    if (transfer->profile_id && transfer->profile.config.event_mask) {
        result = mrc_ev_event_open_sender();
        if (result < 0)
            goto failed;
        sender->event_fd = result;
        sender->ev.notify = publish_ev_transition;
        sender->ev.notify_context = sender;
    }

    reset_requester_batch(sender);
    if (!transfer->continuous_send || attr.send_pending) {
        result = load_requester_batch(sender, &attr);
        if (result)
            goto failed;
    }

    sender->last_progress = rte_get_timer_cycles();
    sender->denied = transfer->ev_deny_mask | transfer->profile.config.denied;
    update_ev_denials(sender, 0, sender->denied, sender->last_progress);
    sender->profile_period = mrc_endpoint_cycles(transfer, UINT64_C(10000000));
    sender->next_profile_check = sender->last_progress + sender->profile_period;
    if (transfer->profile_id)
        fprintf(stderr, "MRC EV profile %" PRIu64 " active, deny mask 0x%" PRIx32 "\n",
                transfer->profile_id, sender->denied);
    sender->next_retry_scan = sender->last_progress;
    sender->next_probe = sender->last_progress;
    *requester = sender;
    return 0;

failed:
    mrc_requester_destroy(sender);
    return result;
}

static int requester_step(struct mrc_requester *sender)
{
    struct mrc_endpoint_config *transfer = &sender->transfer;
    struct mrc_endpoint_counters *counts = sender->counts;
    if (!transfer->packets && transfer->continuous_send) {
        struct mrc_provider_qp_attr attr;
        int result = mrc_provider_qp_query(sender->provider_qp, &attr);
        if (result)
            return result;
        if (attr.state != MRC_PROVIDER_QP_RTS)
            return -EINVAL;
        if (attr.send_pending) {
            result = load_requester_batch(sender, &attr);
            if (result)
                return result;
            sender->last_progress = rte_get_timer_cycles();
        }
    }
    uint32_t previous_psn = sender->qp.first_psn;
    int result = receive_control(transfer, sender, counts);
    if (result)
        return result;
    uint64_t now = rte_get_timer_cycles();
    if (transfer->profile_id && now >= sender->next_profile_check) {
        struct mrc_ctl_store_profile profile;
        result = mrc_ctl_store_get(&transfer->profile_store, transfer->profile_id, &profile);
        if (result || profile.state != MRC_CTL_STORE_ONLINE) {
            if (!result)
                result = -EPROTO;
            fprintf(stderr, "MRC EV profile refresh failed: %s\n", strerror(-result));
            return result;
        }
        now = rte_get_timer_cycles();
        uint32_t updated = transfer->ev_deny_mask | profile.config.denied;
        if (updated != sender->denied) {
            update_ev_denials(sender, sender->denied, updated, now);
            sender->denied = updated;
            counts->ev_admin_updates++;
        }
        sender->next_profile_check = now + sender->profile_period;
    }
    poll_port_status(transfer, sender->node, now);
    if (sender->qp.first_psn != previous_psn)
        sender->last_progress = now;

    uint32_t first = (uint32_t)mrc_psn_delta(sender->qp.first_psn, transfer->initial_psn);
    if (first == transfer->packets) {
        counts->ev_skipped = sender->ev.skipped;
        counts->ev_failed = sender->ev.failed;
        counts->ev_recovered = sender->ev.recovered;
        counts->ev_failed_mask = sender->ev.failed_mask;
        counts->ev_recovered_mask = sender->ev.recovered_mask;
        if (!transfer->continuous_send)
            return 1;
        if (transfer->packets) {
            sender->msn_base = mrc_psn_add(sender->msn_base, (int32_t)transfer->work_requests);
            sender->rqmsn_base = (uint16_t)(sender->rqmsn_base + transfer->immediate_requests);
            reset_requester_batch(sender);
        }
        return 0;
    }
    if (now - sender->last_progress >= transfer->idle_timeout) {
        fprintf(stderr, "TX stalled at segment %u, next %u, inflight %d, cwnd %u, "
                        "SACKs %u, ACKs %u, retries [%u,%u], drops %u\n",
                first, sender->next, sender->cc.inflight, sender->cc.cwnd, counts->sacks,
                counts->transport_acks, counts->retries[0], counts->retries[1], counts->dropped);
        return -ETIMEDOUT;
    }
    if (now >= sender->next_retry_scan) {
        result = retry_due_packets(transfer, sender, counts, first, now);
        if (result)
            return result;
        sender->next_retry_scan = now + transfer->initial_rto / 32 + 1;
    }
    bool probe_timer_due = now >= sender->next_probe;
    bool window_blocked = probe_timer_due && sender->next < transfer->packets &&
        !mrc_cc_can_send(&sender->cc, nominal_at(transfer, sender->next));
    bool general_probe = probe_timer_due && (sender->initial_probe_paths || window_blocked);
    bool sent_probe = false;
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        uint32_t bit = UINT32_C(1) << path;
        if (sender->node->local_down & bit)
            continue;
        bool due = mrc_ev_probe_due(&sender->ev, path, now);
        bool initial = (sender->initial_probe_paths & bit) != 0;
        if (due || (general_probe && (initial || window_blocked) &&
                    mrc_ev_probe_allowed(&sender->ev, path) &&
                    sender->ev.paths[path].state != MRC_EV_ASSUMED_BAD)) {
            sent_probe |= request_probe(transfer, sender, counts, path, now);
            mrc_ev_probe_attempt(&sender->ev, path, now);
        }
    }
    if (general_probe)
        sender->next_probe = now + (sent_probe ? transfer->initial_rto : transfer->initial_rto / 32 + 1);
    for (unsigned int i = 0; i < BURST && sender->next < transfer->packets &&
         sender->next - first < transfer->window; i++) {
        uint32_t index = sender->next;
        uint32_t psn = mrc_psn_add(transfer->initial_psn, (int32_t)index);
        if (!mrc_mpr_can_transmit(&sender->mpr, sender->qp.delivery_cack_psn, psn))
            break;
        const struct mrc_endpoint_write *work = &transfer->work[request_index(transfer, index)];
        uint32_t wimm_inflight = work->preceding_immediates -
            transfer->work[request_index(transfer, first)].preceding_immediates;
        if ((work->with_immediate && wimm_inflight >= transfer->max_wimm_inflight) ||
            !mrc_cc_can_send(&sender->cc, nominal_at(transfer, index)))
            break;
        int picked = mrc_ev_pick(&sender->ev, available_paths(sender, now), 0, now);
        if (picked < 0)
            break;
        unsigned int path = (unsigned int)picked;
        result = send_write(transfer, sender, counts, path, index, false);
        if (result == -EAGAIN) {
            if (!sender->blocked_since[path])
                sender->blocked_since[path] = now;
            if (now - sender->blocked_since[path] >= transfer->initial_rto)
                mrc_ev_timeout(&sender->ev, path, now);
            break;
        }
        if (result)
            return result;
        sender->blocked_since[path] = 0;
        mrc_ev_sent(&sender->ev, path);
        if (!mrc_tx_track(&sender->qp, psn, now))
            return -EPROTO;
        sender->metadata[index % WINDOW] = (struct send_record){
            .path = path, .counted_inflight = true,
        };
        sender->next++;
        sender->scheduled_packets++;
    }
    return 0;
}

int mrc_requester_progress(struct mrc_requester *requester)
{
    if (!requester)
        return -EINVAL;
    if (!requester->result) {
        requester->result = requester_step(requester);
        if (requester->result < 0)
            mrc_provider_qp_fail(requester->provider_qp, 0, MRC_PROVIDER_WC_FLUSHED);
    }
    return requester->result;
}

void mrc_requester_destroy(struct mrc_requester *requester)
{
    if (requester) {
        if (requester->event_fd >= 0)
            close(requester->event_fd);
        free(requester->transfer.work);
        free(requester);
    }
}

int mrc_responder_create(const struct mrc_endpoint_config *transfer, struct mrc_provider_qp *qp,
                         struct mrc_endpoint_counters *counts, struct mrc_responder **responder)
{
    if (!transfer || !qp || !counts || !responder || transfer->sender ||
        !transfer->pool || !transfer->hz || !transfer->initial_rto ||
        transfer->window > WINDOW || transfer->rx_staging_capacity > WINDOW ||
        transfer->rx_work_capacity > WINDOW)
        return -EINVAL;
    struct mrc_provider_qp_attr attr;
    int result = mrc_provider_qp_query(qp, &attr);
    if (result)
        return result;
    if ((attr.state != MRC_PROVIDER_QP_RTR && attr.state != MRC_PROVIDER_QP_RTS) ||
        attr.qpn != transfer->responder_qpn)
        return -EINVAL;

    struct mrc_responder *receiver = calloc(1, sizeof(*receiver));
    if (!receiver)
        return -ENOMEM;
    if (transfer->rx_staging_capacity) {
        receiver->staged_payload = malloc(transfer->rx_staging_capacity * PMTU);
        if (!receiver->staged_payload) {
            mrc_responder_destroy(receiver);
            return -ENOMEM;
        }
    }
    receiver->transfer = transfer;
    receiver->node = transfer->node ? transfer->node : &receiver->local_node;
    receiver->counts = counts;
    receiver->provider_qp = qp;
    struct mrc_wr_config config = {
        .pd = transfer->pd, .provider_qp = qp,
        .region = {
            .memory = transfer->data, .address = transfer->remote_address,
            .length = transfer->bytes, .r_key = transfer->remote_key,
        },
        .pmtu = PMTU, .initial_psn = transfer->initial_psn,
        .initial_msn = 1, .initial_rqmsn = 1,
        .max_wimm_inflight = transfer->max_wimm_inflight,
        .work = receiver->work, .work_capacity = transfer->rx_work_capacity,
        .staged = receiver->staged, .staged_payload = receiver->staged_payload,
        .staged_capacity = transfer->rx_staging_capacity,
    };
    if (!mrc_rx_init(&receiver->qp, transfer->initial_psn, transfer->window, receiver->slots) ||
        !mrc_wr_init(&receiver->wr, &config) ||
        !mrc_mpr_init(&receiver->mpr, transfer->dynamic_mpr, transfer->mpr_max)) {
        mrc_responder_destroy(receiver);
        return -EINVAL;
    }
    receiver->last_frame = receiver->last_progress = rte_get_timer_cycles();
    *responder = receiver;
    return 0;
}

static int responder_step(struct mrc_responder *receiver)
{
    const struct mrc_endpoint_config *transfer = receiver->transfer;
    struct mrc_endpoint_counters *counts = receiver->counts;
    uint32_t before = receiver->unique_packets;
    int result = 0;
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        struct mrc_endpoint_rx_packet packets[BURST];
        uint16_t n = receive_burst(transfer, path, packets);
        for (uint16_t i = 0; i < n; i++) {
            if (!result)
                result = receive_frame(transfer, receiver, counts, path, &packets[i]);
            rte_pktmbuf_free(packets[i].mbuf);
        }
    }
    if (result)
        return result;
    uint64_t now = rte_get_timer_cycles();
    poll_port_status(transfer, receiver->node, now);
    if (!transfer->packets)
        return 0;
    if (receiver->unique_packets != before)
        receiver->last_progress = now;
    if (receiver->unique_packets == transfer->packets) {
        if (now - receiver->last_frame >= 8 * transfer->initial_rto + transfer->hz / 4)
            return 1;
    } else if (now - receiver->last_progress >= transfer->idle_timeout) {
        fprintf(stderr, "RX stalled after %u of %u segments, cack PSN %u, "
                        "max received PSN %u\n",
                receiver->unique_packets, transfer->packets,
                receiver->qp.cack_psn, receiver->qp.max_rcv_psn);
        return -ETIMEDOUT;
    }
    return 0;
}

int mrc_responder_set_flow_control(struct mrc_responder *responder, uint8_t penalty, bool restore)
{
    if (!responder || penalty > 127)
        return -EINVAL;
    responder->flow = (struct mrc_cc_receiver_flow){ .penalty = penalty, .restore = restore };
    return 0;
}

int mrc_responder_progress(struct mrc_responder *responder)
{
    if (!responder)
        return -EINVAL;
    if (!responder->result) {
        responder->result = responder_step(responder);
        if (responder->result < 0)
            mrc_provider_qp_fail(responder->provider_qp, 0, MRC_PROVIDER_WC_FLUSHED);
    }
    return responder->result;
}

uint32_t mrc_responder_ack_msn(const struct mrc_responder *responder)
{
    return mrc_wr_ack_msn(&responder->wr);
}

void mrc_responder_destroy(struct mrc_responder *responder)
{
    if (responder) {
        mrc_wr_destroy(&responder->wr);
        free(responder->staged_payload);
        free(responder);
    }
}
