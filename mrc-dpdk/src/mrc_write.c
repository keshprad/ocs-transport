#include "dpdk_io.h"
#include "mrc_cc.h"
#include "mrc_ev.h"
#include "mrc_mpr.h"
#include "mrc_provider.h"
#include "mrc_oob.h"
#include "mrc_qp.h"
#include "mrc_wire.h"
#include "mrc_wr.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_pause.h>

enum {
    PATH_COUNT = 2,
    PMTU = 1024,
    WINDOW = 1024,
    MAX_WORK_REQUESTS = 4096,
    DEFAULT_WIMM_INFLIGHT = 8,
    BURST = 32,
    FRAME_CAPACITY = 1514,
    SACK_THRESHOLD = 4096,
    UDP_HEADER_LEN = 8,
    REQUESTOR_EV_PORT = 0xc000,
    RESPONDER_EV_PORT = 0xd000,
    EV_FLOW_LABEL = 0xa0100,
    SINGLE_SEGMENT_SRH_LEN = 24,
    NOMINAL_MTU = PMTU + UDP_HEADER_LEN + MRC_BTH_LEN + MRC_METH_LEN +
                  MRC_RETH_LEN + MRC_IMMDT_LEN + MRC_ICRC_LEN + MRC_CC_NOMINAL_HDRSIZE,
    INITIAL_CWND = 128 * NOMINAL_MTU,
    TARGET_QUEUE_NS = 20000,
    FAST_LOSS_GAP = 16,
    ALL_PATHS = (1 << PATH_COUNT) - 1,
    DEFAULT_REQUESTOR_QPN = 0x101,
    DEFAULT_RESPONDER_QPN = 0x102,
    DEFAULT_PARTITION_KEY = 0xffff,
    DEFAULT_REMOTE_KEY = 0x6d726301,
    DATA_DSCP = 0,
    CONTROL_DSCP = 48,
    TRIM_DSCP = 49,
    ACK_SYNDROME = 0x1f,
    INVALID_REQUEST_NAK = 0x61,
    REMOTE_ACCESS_NAK = 0x62,
    REMOTE_OPERATION_NAK = 0x63,
};

static const uint64_t default_remote_address = UINT64_C(0x100000000);

struct path {
    uint16_t port;
    struct rte_ether_addr local;
    struct rte_ether_addr peer;
};

struct transfer {
    bool sender;
    bool with_immediate;
    bool trim_nack_enabled;
    bool port_stats;
    bool remote_key_configured;
    const char *oob_address;
    uint16_t oob_port;
    uint32_t timeout_ms;
    uint32_t immediate;
    uint32_t bytes;
    uint32_t packets;
    uint32_t wr_bytes;
    uint32_t wr_packets;
    uint32_t work_requests;
    uint32_t posted_receives;
    uint32_t max_wimm_inflight;
    uint32_t initial_psn;
    uint32_t requestor_qpn;
    uint32_t responder_qpn;
    uint32_t remote_key;
    uint32_t window;
    uint64_t remote_address;
    uint16_t partition_key;
    uint16_t peer_partition_key;
    uint32_t local_ip;
    uint32_t peer_ip;
    uint8_t local_ip6[MRC_IPV6_ADDR_LEN];
    uint8_t peer_ip6[MRC_IPV6_ADDR_LEN];
    enum mrc_wire_ip_version ip_version;
    uint8_t local_sid[MRC_IPV6_ADDR_LEN];
    uint8_t peer_sid[MRC_IPV6_ADDR_LEN];
    bool srv6;
    bool srv6_srh;
    uint32_t drop_after;
    uint32_t ev_deny_mask;
    uint32_t restore_after;
    uint32_t mpr_change_at;
    uint8_t mpr_change_to;
    uint8_t mpr_max;
    bool dynamic_mpr;
    uint32_t trim_after;
    uint32_t final_ack_drops;
    uint16_t udp_port;
    uint8_t trim_dscp;
    unsigned int timeout_exponent;
    unsigned int linear_retries;
    unsigned int exponential_retries;
    uint64_t hz;
    uint64_t idle_timeout;
    uint64_t initial_rto;
    struct path paths[PATH_COUNT];
    struct rte_mempool *pool;
    uint8_t *data;
    struct mrc_provider_pd *pd;
    struct mrc_provider_mr_keys buffer_keys;
    struct mrc_provider_mr_lease source_lease;
};

struct rx_counters {
    uint64_t frames;
    uint64_t matched;
    uint64_t rejected;
    uint64_t other_peer;
};

struct counters {
    struct rx_counters rx[PATH_COUNT];
    uint32_t sent[PATH_COUNT];
    uint32_t received[PATH_COUNT];
    uint32_t retries[PATH_COUNT];
    uint32_t dropped;
    uint32_t trimmed;
    uint32_t trim_nacks;
    uint32_t port_status_updates;
    uint32_t port_status_replies;
    uint32_t probe_requests;
    uint32_t probe_replies;
    uint32_t probe_sent[PATH_COUNT];
    uint32_t ev_skipped, ev_failed, ev_recovered;
    uint32_t mpr_updates;
    uint32_t fast_retries;
    uint32_t sacks;
    uint32_t transport_acks;
    uint32_t dropped_transport_acks;
    uint32_t duplicates;
    uint32_t reorder_max;
    uint32_t immediate_completions;
    uint32_t write_completions;
};

struct send_record {
    unsigned int path;
    bool fast_retransmitted;
    bool nacked;
    bool counted_inflight;
    bool redundant_attempt;
};

struct node_status {
    uint32_t local_ports;
    uint32_t peer_ports;
    uint16_t token;
    uint16_t pending[PATH_COUNT];
    bool awaiting[PATH_COUNT];
    bool peer_seen;
    uint64_t next_poll;
    uint64_t fast_poll_until;
    uint64_t peer_valid_until;
};

struct outstanding_probe {
    uint64_t sent_at;
    uint16_t id;
    bool pending;
};

struct sender {
    struct mrc_provider_cq *cq;
    struct mrc_cc cc;
    struct node_status node;
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

struct responder {
    struct mrc_provider_cq *cq;
    struct mrc_rx_qp qp;
    struct mrc_wr_responder wr;
    struct mrc_mpr mpr;
    struct node_status node;
    struct mrc_wr_slot work[WINDOW];
    struct mrc_wr_staged staged[WINDOW];
    uint64_t receive_queue[MAX_WORK_REQUESTS];
    uint8_t slots[WINDOW];
    uint32_t unique_packets;
    struct mrc_cc_rx cc;
};

static uint8_t pattern(uint32_t offset)
{
    return (uint8_t)(offset ^ (offset >> 8) ^ (offset >> 16) ^ (offset >> 24));
}

static uint32_t request_index(const struct transfer *transfer, uint32_t index)
{
    return index / transfer->wr_packets;
}

static uint32_t buffer_offset(const struct transfer *transfer, uint32_t index)
{
    return request_index(transfer, index) * transfer->wr_bytes +
           (index % transfer->wr_packets) * PMTU;
}

static uint32_t length_at(const struct transfer *transfer, uint32_t index)
{
    uint32_t remaining = transfer->wr_bytes - (index % transfer->wr_packets) * PMTU;
    return remaining < PMTU ? remaining : PMTU;
}

static uint8_t expected_opcode(const struct transfer *transfer, uint32_t index)
{
    if (transfer->wr_packets == 1)
        return transfer->with_immediate ? MRC_WRITE_ONLY_IMM : MRC_WRITE_ONLY;
    uint32_t fragment = index % transfer->wr_packets;
    if (fragment == 0)
        return MRC_WRITE_FIRST;
    if (fragment + 1 == transfer->wr_packets)
        return transfer->with_immediate ? MRC_WRITE_LAST_IMM : MRC_WRITE_LAST;
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

static uint32_t nominal_at(const struct transfer *transfer, uint32_t index)
{
    return nominal_size(expected_opcode(transfer, index), length_at(transfer, index), false);
}

static uint64_t cycles_to_ns(const struct transfer *transfer, uint64_t cycles)
{
    return (cycles / transfer->hz) * 1000000000 +
           (cycles % transfer->hz) * 1000000000 / transfer->hz;
}

static uint32_t path_entropy(const struct transfer *transfer, unsigned int index,
                             bool requestor)
{
    uint32_t port = (requestor ? REQUESTOR_EV_PORT : RESPONDER_EV_PORT) + index;
    if (transfer->ip_version == MRC_WIRE_IP6)
        port |= ((EV_FLOW_LABEL + index) & UINT16_MAX) << 16;
    return port;
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

static struct mrc_wire_packet packet_for(const struct transfer *transfer, unsigned int index,
                                          uint8_t opcode, uint32_t psn)
{
    const struct path *path = &transfer->paths[index];
    struct mrc_wire_packet packet = {
        .ip_version = transfer->ip_version,
        .bth = {
            .opcode = opcode, .psn = psn, .p_key = transfer->partition_key,
            .dqp = transfer->sender ? transfer->responder_qpn : transfer->requestor_qpn,
        },
    };

    bool data = mrc_wire_is_write(opcode);
    uint16_t port = (uint16_t)path_entropy(transfer, index, transfer->sender);
    uint8_t dscp = data || opcode == MRC_RELIABILITY_PROBE ? DATA_DSCP : CONTROL_DSCP;
    uint8_t ecn = data ? 2 : 0;
    if (transfer->ip_version == MRC_WIRE_IP6) {
        packet.ipv6 = (struct mrc_wire_ipv6){
            .src_port = port, .dst_port = transfer->udp_port,
            .dscp = dscp, .ecn = ecn, .flow_label = EV_FLOW_LABEL + index,
        };
        memcpy(packet.ipv6.src_mac, path->local.addr_bytes, MRC_MAC_LEN);
        memcpy(packet.ipv6.dst_mac, path->peer.addr_bytes, MRC_MAC_LEN);
        memcpy(packet.ipv6.src_ip, transfer->local_ip6, MRC_IPV6_ADDR_LEN);
        memcpy(packet.ipv6.dst_ip, transfer->peer_ip6, MRC_IPV6_ADDR_LEN);
    } else {
        packet.ipv4 = (struct mrc_wire_ipv4){
            .src_port = port, .dst_port = transfer->udp_port, .dscp = dscp, .ecn = ecn,
            .src_ip = transfer->local_ip, .dst_ip = transfer->peer_ip,
        };
        memcpy(packet.ipv4.src_mac, path->local.addr_bytes, MRC_MAC_LEN);
        memcpy(packet.ipv4.dst_mac, path->peer.addr_bytes, MRC_MAC_LEN);
    }
    return packet;
}

static size_t simulate_trim(const struct transfer *transfer, uint8_t *frame)
{
    /* Preserve the nominal UDP length but truncate the body before the RETH. */
    uint8_t *ip = frame + RTE_ETHER_HDR_LEN;
    size_t trim_headers = MRC_BTH_LEN + MRC_METH_LEN;
    if (transfer->ip_version == MRC_WIRE_IP6) {
        struct rte_ipv6_hdr header;
        memcpy(&header, ip, sizeof(header));
        uint32_t vtc_flow = rte_be_to_cpu_32(header.vtc_flow);
        vtc_flow = (vtc_flow & ~(UINT32_C(63) << 22)) | (uint32_t)transfer->trim_dscp << 22;
        header.vtc_flow = rte_cpu_to_be_32(vtc_flow);
        size_t outer_payload = UDP_HEADER_LEN + trim_headers;
        if (transfer->srv6) {
            outer_payload += sizeof(struct rte_ipv6_hdr);
            if (transfer->srv6_srh)
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

static bool transmit_frame(const struct transfer *transfer, unsigned int path,
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
        struct mrc_wire_srv6_tx route = {
            .flow_label = packet->ipv6.flow_label,
            .dscp = packet->ipv6.dscp, .ecn = packet->ipv6.ecn,
            .segment_list = transfer->srv6_srh ? &transfer->peer_sid : NULL,
            .segment_count = transfer->srv6_srh ? 1u : 0u,
        };
        memcpy(route.outer_src_ip, transfer->local_sid, MRC_IPV6_ADDR_LEN);
        memcpy(route.outer_dst_ip, transfer->peer_sid, MRC_IPV6_ADDR_LEN);
        result = mrc_wire_wrap_srv6(frame, capacity, length, &route, &length);
    }
    if (result) {
        rte_pktmbuf_free(mbuf);
        return false;
    }
    if (inject_trim)
        length = simulate_trim(transfer, frame);
    if (!rte_pktmbuf_append(mbuf, (uint16_t)length)) {
        rte_pktmbuf_free(mbuf);
        return false;
    }
    return mrc_dpdk_transmit(transfer->paths[path].port, mbuf);
}

static bool transmit(const struct transfer *transfer, unsigned int path,
                     const struct mrc_wire_packet *packet)
{
    return transmit_frame(transfer, path, packet, false);
}

static const uint8_t *read_frame(const struct rte_mbuf *mbuf,
                                  uint8_t scratch[FRAME_CAPACITY], size_t *length)
{
    *length = rte_pktmbuf_pkt_len(mbuf);
    if (*length > FRAME_CAPACITY)
        return NULL;
    return rte_pktmbuf_read(mbuf, 0, (uint32_t)*length, scratch);
}

static bool peer_matches(const struct transfer *transfer, unsigned int index,
                         const struct mrc_wire_packet *packet)
{
    uint32_t expected_qpn = transfer->sender ? transfer->requestor_qpn : transfer->responder_qpn;
    if (packet->bth.opcode == MRC_ENDPOINT_REQUEST || packet->bth.opcode == MRC_ENDPOINT_RESPONSE)
        expected_qpn = MRC_ENDPOINT_QPN;
    if (packet->bth.p_key != transfer->peer_partition_key || packet->bth.dqp != expected_qpn ||
        packet->ip_version != transfer->ip_version)
        return false;

    const struct path *path = &transfer->paths[index];
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

static bool route_matches(const struct transfer *transfer, const struct mrc_wire_srv6 *route)
{
    return !route->encapsulated ||
           (transfer->srv6 &&
            memcmp(route->outer_src_ip, transfer->peer_sid, MRC_IPV6_ADDR_LEN) == 0 &&
            memcmp(route->outer_dst_ip, transfer->local_sid, MRC_IPV6_ADDR_LEN) == 0);
}

static int decode_routed(const struct transfer *transfer, const uint8_t *frame, size_t length,
                         struct mrc_wire_packet *packet)
{
    struct mrc_wire_srv6 route;
    int result = mrc_wire_decode_srv6(frame, length, packet, &route);
    if (result)
        return result;
    if (!route_matches(transfer, &route))
        return -EHOSTUNREACH;
    /* Propagate a fabric CE mark before building reliability feedback. */
    if (route.encapsulated && route.outer_ecn == 3 && packet->ip_version == MRC_WIRE_IP6)
        packet->ipv6.ecn = 3;
    return 0;
}

static bool decode(const struct transfer *transfer, unsigned int index,
                   const struct rte_mbuf *mbuf, uint8_t scratch[FRAME_CAPACITY],
                   struct mrc_wire_packet *packet, struct rx_counters *counts)
{
    size_t length;
    counts->frames++;
    const uint8_t *frame = read_frame(mbuf, scratch, &length);
    if (!frame || decode_routed(transfer, frame, length, packet)) {
        counts->rejected++;
        return false;
    }
    if (!peer_matches(transfer, index, packet)) {
        counts->other_peer++;
        return false;
    }
    counts->matched++;
    return true;
}

static void poll_port_status(const struct transfer *transfer, struct node_status *status,
                             uint64_t now)
{
    if (now < status->next_poll)
        return;
    if (!status->fast_poll_until)
        status->fast_poll_until = now + transfer->hz / 50;
    bool connecting = now < status->fast_poll_until || !status->local_ports;
    status->next_poll = now + transfer->hz / (connecting ? 1000 : 10);

    uint32_t active = 0;
    for (unsigned int i = 0; i < PATH_COUNT; i++) {
        struct rte_eth_link link;
        if (!rte_eth_link_get_nowait(transfer->paths[i].port, &link) && link.link_status)
            active |= 1u << i;
    }
    if (active == status->local_ports)
        return;

    status->local_ports = active;
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        status->awaiting[path] = false;
        if (!(active & (1u << path)))
            continue;
        uint16_t token = ++status->token;
        struct mrc_wire_packet request = packet_for(transfer, path, MRC_ENDPOINT_REQUEST, token);
        request.bth.dqp = MRC_ENDPOINT_QPN;
        request.body.endpoint_request = (struct mrc_wire_endpoint_request){
            .op = MRC_ENDPOINT_PORT_STATUS, .port_status_mask = active,
        };
        if (transmit(transfer, path, &request)) {
            status->pending[path] = token;
            status->awaiting[path] = true;
        }
    }
}

static bool receive_endpoint_op(const struct transfer *transfer, struct node_status *status,
                                struct counters *counts, unsigned int path,
                                const struct mrc_wire_packet *packet)
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
        status->peer_ports = request->port_status_mask & ALL_PATHS;
        status->peer_seen = true;
        status->peer_valid_until = rte_get_timer_cycles() + transfer->hz;
        counts->port_status_updates++;
    }
    struct mrc_wire_packet response = packet_for(transfer, path, MRC_ENDPOINT_RESPONSE,
                                                  (uint16_t)packet->bth.psn);
    response.bth.dqp = MRC_ENDPOINT_QPN;
    response.body.endpoint_response = (struct mrc_wire_endpoint_response){
        .op = request->op, .tx_timestamp = request->timestamp ? request->tx_timestamp : 0,
    };
    transmit(transfer, path, &response);
    return true;
}

static uint32_t available_paths(const struct sender *sender, uint64_t now)
{
    if (sender->node.peer_seen && now < sender->node.peer_valid_until)
        return sender->node.peer_ports;
    return ALL_PATHS;
}

static bool blackholed(const struct transfer *transfer, const struct sender *sender,
                        unsigned int path)
{
    return path == 1 && sender->next >= transfer->drop_after &&
           sender->next < transfer->restore_after;
}

static int reflected_path(const struct transfer *transfer, uint32_t entropy)
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

static bool send_write(const struct transfer *transfer, struct sender *sender,
                       struct counters *counts, unsigned int path, uint32_t index,
                       bool retry)
{
    struct mrc_wire_packet packet = packet_for(transfer, path, expected_opcode(transfer, index),
        mrc_psn_add(transfer->initial_psn, (int32_t)index));
    struct mrc_cc after_send = sender->cc;
    if (!mrc_cc_on_send(&after_send, nominal_at(transfer, index)))
        return false;
    packet.bth.retransmit = retry;
    packet.bth.ack_request = retry || (index + 1) % BURST == 0 ||
                             index + 1 == transfer->packets ||
                             mrc_cc_ack_request(&after_send, SACK_THRESHOLD);
    uint32_t wr = request_index(transfer, index);
    uint32_t offset = buffer_offset(transfer, index);
    packet.body.write = (struct mrc_wire_write){
        .rqmsn = transfer->with_immediate ? (uint16_t)(wr + 1) : 0,
        .msn = (uint16_t)(wr + 1),
        .va = transfer->remote_address + offset,
        .r_key = transfer->remote_key,
        .dma_len = transfer->wr_bytes,
        .imm_data = transfer->immediate + wr,
        .data = (const uint8_t *)(transfer->source_lease.owner ? transfer->source_lease.address
                                                                 : transfer->data) + offset,
        .data_len = length_at(transfer, index),
    };

    if (blackholed(transfer, sender, path)) {
        counts->dropped++;
    } else {
        bool inject_trim = path == 1 && sender->next >= transfer->trim_after;
        if (!transmit_frame(transfer, path, &packet, inject_trim))
            return false;
        counts->trimmed += inject_trim;
    }

    sender->cc = after_send;
    if (retry)
        counts->retries[path]++;
    else
        counts->sent[path]++;
    return true;
}

static bool retire_redundant_attempt(const struct transfer *transfer, struct sender *sender,
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

static int drain_completions(const struct transfer *transfer, struct mrc_provider_cq *cq,
                             struct counters *counts)
{
    uint32_t *total = transfer->sender ? &counts->write_completions
                                      : &counts->immediate_completions;
    uint32_t qpn = transfer->sender ? transfer->requestor_qpn : transfer->responder_qpn;
    enum mrc_provider_wc_opcode opcode = transfer->sender ? MRC_PROVIDER_WC_WRITE
                                                          : MRC_PROVIDER_WC_RECV_WRITE_IMM;
    for (;;) {
        struct mrc_provider_wc entries[BURST];
        int count = mrc_provider_cq_poll(cq, entries, BURST);
        if (count <= 0)
            return count;

        for (int i = 0; i < count; i++) {
            const struct mrc_provider_wc *wc = &entries[i];
            if (wc->status != MRC_PROVIDER_WC_SUCCESS || wc->opcode != opcode ||
                wc->qpn != qpn || wc->byte_length != transfer->wr_bytes ||
                wc->wr_id != *total + 1 || wc->with_immediate == transfer->sender ||
                (wc->with_immediate && wc->immediate != transfer->immediate + *total))
                return -EPROTO;
            (*total)++;
        }
    }
}

static int destroy_cq(struct mrc_provider_cq *cq)
{
    if (!cq)
        return 0;
    struct mrc_provider_wc unused[BURST];
    while (mrc_provider_cq_poll(cq, unused, BURST) > 0)
        ;
    enum mrc_provider_cq_event event;
    while (mrc_provider_cq_next_event(cq, &event) == 0)
        mrc_provider_cq_ack_events(cq, 1);
    return mrc_provider_cq_destroy(cq);
}

static int apply_control(const struct transfer *transfer, struct sender *sender,
                         struct counters *counts, const struct mrc_wire_packet *packet)
{
    uint64_t now_cycles = rte_get_timer_cycles();
    uint64_t now = cycles_to_ns(transfer, now_cycles);
    switch (packet->bth.opcode) {
    case MRC_TRANSPORT_ACK: {
        const struct mrc_wire_aeth *ack = &packet->body.aeth;
        if (ack->syndrome != ACK_SYNDROME) {
            fprintf(stderr, "Transport rejected WRITE, syndrome %#x\n", ack->syndrome);
            return -EPROTO;
        }
        int32_t last = mrc_psn_delta(packet->bth.psn, transfer->initial_psn);
        int32_t first = mrc_psn_delta(sender->qp.first_psn, transfer->initial_psn);
        if (last < first || last >= (int32_t)sender->next)
            return 0;
        uint32_t completed = request_index(transfer, (uint32_t)last + 1);
        if (ack->msn != completed)
            return -EPROTO;
        for (int32_t index = first; index <= last; index++) {
            if (!retire_redundant_attempt(transfer, sender, (uint32_t)index))
                return -EPROTO;
        }
        if (!mrc_tx_apply_transport_ack(&sender->qp, packet->bth.psn))
            return 0;
        counts->transport_acks++;
        uint32_t previous = request_index(transfer, (uint32_t)first);
        for (uint32_t wr = previous; wr < completed; wr++) {
            const struct mrc_provider_wc completion = {
                .wr_id = wr + 1, .qpn = transfer->requestor_qpn,
                .byte_length = transfer->wr_bytes,
                .opcode = MRC_PROVIDER_WC_WRITE, .status = MRC_PROVIDER_WC_SUCCESS,
            };
            int result = mrc_provider_cq_push(sender->cq, &completion, false);
            if (result)
                return result;
        }
        return 0;
    }
    case MRC_RELIABILITY_SACK: {
        const struct mrc_wire_sack *wire = &packet->body.sack;
        if (wire->spdcid != (uint16_t)transfer->requestor_qpn || wire->dpdcid != (uint16_t)transfer->responder_qpn ||
            (!wire->probe_response && packet->bth.psn != wire->cack_psn))
            return 0;
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
        int ev = reflected_path(transfer, wire->entropy);
        struct mrc_cc_tx_sample sample;
        struct mrc_cc_tx_sample *rtt_sample = NULL;
        if (wire->probe_response) {
            for (unsigned int path = 0; path < PATH_COUNT; path++) {
                struct outstanding_probe *probe = &sender->probes[path];
                if (probe->pending && probe->id == (uint16_t)wire->ack_psn_offset &&
                    wire->entropy == path_entropy(transfer, path, true)) {
                    sample = (struct mrc_cc_tx_sample){
                        .sent_at_ns = cycles_to_ns(transfer, probe->sent_at),
                    };
                    rtt_sample = &sample;
                    mrc_ev_feedback(&sender->ev, path, ev_mark(wire->mark), true,
                                    probe->sent_at, now_cycles);
                    probe->pending = false;
                    counts->probe_replies++;
                    break;
                }
            }
        } else {
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
                    .retransmissions = sent->retransmissions,
                };
                rtt_sample = &sample;
            }
        }
        if (!mrc_cc_on_sack(&sender->cc, wire->rcvd_bytes, wire->mark == MRC_SACK_SKIP,
                            packet->bth.retransmit, now, rtt_sample, NULL))
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
        if (nack->reason == MRC_NACK_UNEXP_EVENT)
            return -EREMOTEIO;
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

static int receive_control(const struct transfer *transfer, struct sender *sender,
                           struct counters *counts)
{
    int result = 0;
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        struct rte_mbuf *packets[BURST];
        uint16_t n = rte_eth_rx_burst(transfer->paths[path].port, 0, packets, BURST);
        for (uint16_t i = 0; i < n; i++) {
            uint8_t scratch[FRAME_CAPACITY];
            struct mrc_wire_packet packet;
            if (!result && decode(transfer, path, packets[i], scratch, &packet, &counts->rx[path]) &&
                !receive_endpoint_op(transfer, &sender->node, counts, path, &packet))
                result = apply_control(transfer, sender, counts, &packet);
            rte_pktmbuf_free(packets[i]);
        }
    }
    return result;
}

static uint64_t ns_to_cycles(const struct transfer *transfer, uint64_t ns)
{
    return (ns / 1000000000) * transfer->hz +
           (ns % 1000000000) * transfer->hz / 1000000000;
}

static int retry_due_packets(const struct transfer *transfer, struct sender *sender,
                             struct counters *counts, uint32_t first, uint64_t now)
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
        if (!fast && !record->nacked && age < ns_to_cycles(transfer, timeout_ns))
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
            return -ETIMEDOUT;
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
        if (!send_write(transfer, sender, counts, path, index, true)) {
            available &= ~(UINT32_C(1) << path);
            picked = mrc_ev_pick(&sender->ev, available, avoid, now);
            if (picked < 0 || !send_write(transfer, sender, counts, (unsigned int)picked, index, true))
                continue;
            path = (unsigned int)picked;
        }
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

static bool request_probe(const struct transfer *transfer, struct sender *sender,
                           struct counters *counts, unsigned int path, uint64_t now)
{
    uint16_t probe_id = (uint16_t)(sender->probe_id + 1);
    struct mrc_wire_packet packet = packet_for(transfer, path, MRC_RELIABILITY_PROBE, 0);
    packet.body.probe = (struct mrc_wire_probe){
        .probe_id = probe_id, .spdcid = (uint16_t)transfer->requestor_qpn,
        .dpdcid = (uint16_t)transfer->responder_qpn,
    };
    if (!blackholed(transfer, sender, path) && !transmit(transfer, path, &packet))
        return false;
    sender->probe_id = probe_id;
    sender->probes[path] = (struct outstanding_probe){
        .sent_at = now, .id = probe_id, .pending = true,
    };
    counts->probe_requests++;
    counts->probe_sent[path]++;
    return true;
}

static int poll_sender(const struct transfer *transfer, struct counters *counts,
                       struct mrc_provider_cq *cq)
{
    struct sender sender = { .highest_sacked = -1, .cq = cq };
    if (!mrc_tx_init(&sender.qp, transfer->initial_psn, transfer->window, sender.slots) ||
        !mrc_cc_init(&sender.cc, NOMINAL_MTU, INITIAL_CWND, 0, TARGET_QUEUE_NS) ||
        !mrc_ev_init(&sender.ev, PATH_COUNT, transfer->initial_rto / 8 + 1,
                      transfer->initial_rto / 2 + 1) ||
        !mrc_mpr_init(&sender.mpr, transfer->dynamic_mpr, transfer->mpr_max))
        return -EINVAL;

    uint64_t last_progress = rte_get_timer_cycles();
    for (unsigned int path = 0; path < PATH_COUNT; path++) {
        if (transfer->ev_deny_mask & (UINT32_C(1) << path))
            mrc_ev_deny(&sender.ev, path, true, last_progress);
    }
    uint64_t next_retry_scan = last_progress;
    uint64_t next_probe = last_progress;
    bool initial_probe = true;
    for (;;) {
        uint32_t previous_psn = sender.qp.first_psn;
        int result = receive_control(transfer, &sender, counts);
        int completion_result = drain_completions(transfer, cq, counts);
        if (result || completion_result)
            return result ? result : completion_result;
        uint64_t now = rte_get_timer_cycles();
        poll_port_status(transfer, &sender.node, now);
        if (sender.qp.first_psn != previous_psn)
            last_progress = now;

        uint32_t first = (uint32_t)mrc_psn_delta(sender.qp.first_psn, transfer->initial_psn);
        if (first == transfer->packets) {
            counts->ev_skipped = sender.ev.skipped;
            counts->ev_failed = sender.ev.failed;
            counts->ev_recovered = sender.ev.recovered;
            return counts->write_completions == transfer->work_requests ? 0 : -EPROTO;
        }
        if (now - last_progress >= transfer->idle_timeout) {
            fprintf(stderr, "TX stalled at segment %u, next %u, inflight %d, cwnd %u, "
                            "SACKs %u, ACKs %u, retries [%u,%u], drops %u\n",
                    first, sender.next, sender.cc.inflight, sender.cc.cwnd, counts->sacks,
                    counts->transport_acks, counts->retries[0], counts->retries[1], counts->dropped);
            return -ETIMEDOUT;
        }
        if (now >= next_retry_scan) {
            result = retry_due_packets(transfer, &sender, counts, first, now);
            if (result)
                return result;
            next_retry_scan = now + transfer->initial_rto / 32 + 1;
        }
        bool general_probe = now >= next_probe && (initial_probe ||
            (sender.next < transfer->packets &&
             !mrc_cc_can_send(&sender.cc, nominal_at(transfer, sender.next))));
        bool sent_probe = false;
        for (unsigned int path = 0; path < PATH_COUNT; path++) {
            bool due = mrc_ev_probe_due(&sender.ev, path, now);
            if (due || (general_probe && mrc_ev_probe_allowed(&sender.ev, path) &&
                        sender.ev.paths[path].state != MRC_EV_ASSUMED_BAD)) {
                sent_probe |= request_probe(transfer, &sender, counts, path, now);
                mrc_ev_probe_attempt(&sender.ev, path, now);
            }
        }
        if (general_probe) {
            initial_probe &= !sent_probe;
            next_probe = now + (sent_probe ? transfer->initial_rto : transfer->initial_rto / 32 + 1);
        }
        for (unsigned int i = 0; i < BURST && sender.next < transfer->packets &&
             sender.next - first < transfer->window; i++) {
            uint32_t index = sender.next;
            uint32_t psn = mrc_psn_add(transfer->initial_psn, (int32_t)index);
            if (!mrc_mpr_can_transmit(&sender.mpr, sender.qp.delivery_cack_psn, psn))
                break;
            uint32_t wimm_inflight = request_index(transfer, index) - request_index(transfer, first);
            if ((transfer->with_immediate && wimm_inflight >= transfer->max_wimm_inflight) ||
                !mrc_cc_can_send(&sender.cc, nominal_at(transfer, index)))
                break;
            int picked = mrc_ev_pick(&sender.ev, available_paths(&sender, now), 0, now);
            if (picked < 0)
                break;
            unsigned int path = (unsigned int)picked;
            if (!send_write(transfer, &sender, counts, path, index, false)) {
                if (!sender.blocked_since[path])
                    sender.blocked_since[path] = now;
                if (now - sender.blocked_since[path] >= transfer->initial_rto)
                    mrc_ev_timeout(&sender.ev, path, now);
                break;
            }
            sender.blocked_since[path] = 0;
            mrc_ev_sent(&sender.ev, path);
            if (!mrc_tx_track(&sender.qp, psn, now))
                return -EPROTO;
            sender.metadata[index % WINDOW] = (struct send_record){
                .path = path, .counted_inflight = true,
            };
            sender.next++;
        }
        rte_pause();
    }
}

static int run_sender(const struct transfer *transfer, struct counters *counts)
{
    struct mrc_provider_cq *cq = NULL;
    int result = mrc_provider_cq_create(transfer->work_requests, &cq);
    if (!result)
        result = poll_sender(transfer, counts, cq);
    int closed = destroy_cq(cq);
    return result ? result : closed;
}

static void send_ack(const struct transfer *transfer, struct counters *counts,
                     unsigned int path, uint32_t psn, uint8_t syndrome, uint32_t msn)
{
    if (syndrome == ACK_SYNDROME && msn == transfer->work_requests &&
        counts->dropped_transport_acks < transfer->final_ack_drops) {
        counts->dropped_transport_acks++;
        return;
    }
    struct mrc_wire_packet packet = packet_for(transfer, path, MRC_TRANSPORT_ACK, psn);
    packet.body.aeth = (struct mrc_wire_aeth){ .syndrome = syndrome, .msn = msn };
    if (transmit(transfer, path, &packet))
        counts->transport_acks++;
}

static void send_sack(const struct transfer *transfer, struct responder *receiver,
                      struct counters *counts, unsigned int path,
                      const struct mrc_wire_packet *request, bool probe)
{
    struct mrc_sack state;
    if (probe)
        mrc_rx_build_probe_sack(&receiver->qp, request->body.probe.probe_id, &state);
    else if (!mrc_rx_build_sack(&receiver->qp, request->bth.psn, &state))
        return;

    struct mrc_wire_packet packet = packet_for(transfer, path, MRC_RELIABILITY_SACK,
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
    };
    if (probe && request->body.probe.timestamp)
        packet.body.sack.tx_timestamp = request->body.probe.tx_timestamp;
    else if (!probe && request->bth.timestamp)
        packet.body.sack.tx_timestamp = request->body.write.tx_timestamp;
    if (transmit(transfer, path, &packet)) {
        counts->sacks++;
        mrc_rx_sack_sent(&receiver->qp, &state);
    }
}

static bool send_nack(const struct transfer *transfer, unsigned int path,
                      const struct mrc_wire_bth *request, uint32_t entropy, uint8_t reason,
                      uint16_t original_udp_len, uint16_t tx_timestamp)
{
    struct mrc_wire_packet nack = packet_for(transfer, path, MRC_RELIABILITY_NACK, request->psn);
    nack.bth.retransmit = request->retransmit;
    nack.body.nack = (struct mrc_wire_nack){
        .reason = reason, .entropy = entropy,
        .spdcid = (uint16_t)transfer->requestor_qpn, .dpdcid = (uint16_t)transfer->responder_qpn,
        .nack_psn = request->psn, .tx_timestamp = tx_timestamp,
        .original_udp_len = original_udp_len,
    };
    return transmit(transfer, path, &nack);
}

static uint8_t write_error_syndrome(enum mrc_wr_result result)
{
    switch (result) {
    case MRC_WR_REMOTE_ACCESS:
        return REMOTE_ACCESS_NAK;
    case MRC_WR_RQ_EMPTY:
    case MRC_WR_QP_ERROR:
        return REMOTE_OPERATION_NAK;
    default:
        return INVALID_REQUEST_NAK;
    }
}

static enum mrc_wr_result advance_writes(const struct transfer *transfer,
                                         struct responder *receiver)
{
    bool produced;
    do {
        struct mrc_wr_completion completion;
        enum mrc_wr_result result = mrc_wr_advance(&receiver->wr, receiver->qp.cack_psn,
                                                   &completion, &produced);
        if (result != MRC_WR_OK)
            return result;
        if (produced) {
            const struct mrc_provider_wc wc = {
                .wr_id = completion.wr_id, .qpn = transfer->responder_qpn,
                .byte_length = completion.byte_len, .immediate = completion.immediate,
                .opcode = MRC_PROVIDER_WC_RECV_WRITE_IMM,
                .status = MRC_PROVIDER_WC_SUCCESS, .with_immediate = true,
            };
            if (completion.wr_id != completion.msn || mrc_provider_cq_push(receiver->cq, &wc, false))
                return MRC_WR_QP_ERROR;
        }
    } while (produced);
    return MRC_WR_OK;
}

static int receive_write(const struct transfer *transfer, struct responder *receiver,
                         struct counters *counts, unsigned int path,
                         const struct mrc_wire_packet *packet)
{
    if (packet->bth.opcode == MRC_RELIABILITY_PROBE) {
        if (packet->body.probe.spdcid == (uint16_t)transfer->requestor_qpn &&
            packet->body.probe.dpdcid == (uint16_t)transfer->responder_qpn) {
            counts->probe_requests++;
            send_sack(transfer, receiver, counts, path, packet, true);
        }
        return 0;
    }
    if (!mrc_wire_is_write(packet->bth.opcode))
        return 0;

    uint32_t psn = packet->bth.psn;
    enum mrc_rx_disposition disposition = mrc_rx_classify(&receiver->qp, psn);
    if (disposition == MRC_RX_OUTSIDE_WINDOW)
        return 0;
    if (disposition == MRC_RX_DUPLICATE) {
        counts->duplicates++;
        send_sack(transfer, receiver, counts, path, packet, false);
        send_ack(transfer, counts, path, receiver->qp.cack_psn, ACK_SYNDROME,
                 mrc_wr_ack_msn(&receiver->wr));
        return 0;
    }

    uint32_t index = (uint32_t)mrc_psn_delta(psn, transfer->initial_psn);
    const struct mrc_wire_write *write = &packet->body.write;
    if (index >= transfer->packets || write->dma_len != transfer->wr_bytes) {
        send_ack(transfer, counts, path, psn, INVALID_REQUEST_NAK, mrc_wr_ack_msn(&receiver->wr));
        return -EPROTO;
    }

    enum mrc_wr_result result = mrc_wr_receive(&receiver->wr, psn, packet->bth.opcode, write);
    if (result == MRC_WR_NO_RESOURCE) {
        send_nack(transfer, path, &packet->bth, reflect_entropy(packet), MRC_NACK_NO_RESOURCE,
                  0, write->tx_timestamp);
        return 0;
    }
    if (result != MRC_WR_OK) {
        send_ack(transfer, counts, path, psn, write_error_syndrome(result),
                 mrc_wr_ack_msn(&receiver->wr));
        return -EPROTO;
    }

    uint32_t previous_cack = receiver->qp.cack_psn;
    int32_t distance = mrc_psn_delta(receiver->qp.max_rcv_psn, psn);
    if (distance > 0 && (uint32_t)distance > counts->reorder_max)
        counts->reorder_max = (uint32_t)distance;

    unsigned int flags = (packet->bth.ack_request ? MRC_RX_AR : 0) |
                         (packet->bth.retransmit ? MRC_RX_RTX : 0) |
                         (packet_ecn(packet) == 3 ? MRC_RX_ECN : 0);
    bool sack_due;
    mrc_rx_record(&receiver->qp, psn, (uint32_t)write->data_len, flags, SACK_THRESHOLD, &sack_due);
    uint32_t nominal = nominal_size(packet->bth.opcode, write->data_len, packet->bth.timestamp);
    mrc_cc_rx_record(&receiver->cc, nominal, true, false);
    receiver->unique_packets++;
    if (receiver->unique_packets == transfer->mpr_change_at &&
        mrc_mpr_set_advertised(&receiver->mpr, transfer->mpr_change_to)) {
        counts->mpr_updates++;
        sack_due = true;
    }
    counts->received[path]++;
    bool complete = receiver->unique_packets == transfer->packets;
    result = advance_writes(transfer, receiver);
    uint32_t msn = mrc_wr_ack_msn(&receiver->wr);
    if (result != MRC_WR_OK) {
        send_ack(transfer, counts, path, receiver->qp.cack_psn, write_error_syndrome(result), msn);
        return -EPROTO;
    }
    if (sack_due || complete)
        send_sack(transfer, receiver, counts, path, packet, false);
    if (previous_cack != receiver->qp.cack_psn && (sack_due || complete))
        send_ack(transfer, counts, path, receiver->qp.cack_psn, ACK_SYNDROME, msn);
    return 0;
}

static void receive_trimmed(const struct transfer *transfer, const struct responder *receiver,
                            struct counters *counts, unsigned int path,
                            const struct mrc_wire_trimmed *trimmed, uint32_t entropy)
{
    if (mrc_rx_classify(&receiver->qp, trimmed->bth.psn) != MRC_RX_NEW ||
        !mrc_cc_nominal_size(trimmed->original_udp_len))
        return;

    counts->trimmed++;
    if (transfer->trim_nack_enabled && send_nack(transfer, path, &trimmed->bth, entropy, MRC_NACK_TRIMMED,
                  trimmed->original_udp_len, trimmed->tx_timestamp))
        counts->trim_nacks++;
}

static int receive_frame(const struct transfer *transfer, struct responder *receiver,
                         struct counters *counts, unsigned int path,
                         const struct rte_mbuf *mbuf, uint64_t *last_frame)
{
    struct rx_counters *rx = &counts->rx[path];
    rx->frames++;
    uint8_t scratch[FRAME_CAPACITY];
    size_t length;
    const uint8_t *frame = read_frame(mbuf, scratch, &length);
    if (!frame) {
        rx->rejected++;
        return 0;
    }

    struct mrc_wire_trimmed trimmed;
    struct mrc_wire_srv6 route;
    int result = mrc_wire_decode_trimmed_srv6(frame, length, transfer->trim_dscp, &trimmed, &route);
    struct mrc_wire_packet packet;
    if (result != -EPROTONOSUPPORT) {
        if (result || !route_matches(transfer, &route)) {
            rx->rejected++;
        } else {
            packet.bth = trimmed.bth;
            packet.ip_version = trimmed.ip_version;
            if (trimmed.ip_version == MRC_WIRE_IP6)
                packet.ipv6 = trimmed.ipv6;
            else
                packet.ipv4 = trimmed.ipv4;
            if (peer_matches(transfer, path, &packet)) {
                rx->matched++;
                *last_frame = rte_get_timer_cycles();
                receive_trimmed(transfer, receiver, counts, path, &trimmed, reflect_entropy(&packet));
            } else {
                rx->other_peer++;
            }
        }
        return 0;
    }

    result = decode_routed(transfer, frame, length, &packet);
    if (!result) {
        if (!peer_matches(transfer, path, &packet)) {
            rx->other_peer++;
            return 0;
        }
        rx->matched++;
        if (receive_endpoint_op(transfer, &receiver->node, counts, path, &packet))
            return 0;
        *last_frame = rte_get_timer_cycles();
        return receive_write(transfer, receiver, counts, path, &packet);
    }

    if (result == -EPROTONOSUPPORT) {
        packet.ip_version = transfer->ip_version;
        if (packet.ip_version == MRC_WIRE_IP6)
            result = mrc_wire_decode_ipv6_header(frame, length, &packet.ipv6, &packet.bth);
        else
            result = mrc_wire_decode_header(frame, length, &packet.ipv4, &packet.bth);
        if (!result && peer_matches(transfer, path, &packet) &&
            !mrc_wire_is_write(packet.bth.opcode) && packet.bth.opcode < MRC_TRANSPORT_ACK) {
            rx->matched++;
            send_ack(transfer, counts, path, packet.bth.psn, INVALID_REQUEST_NAK, 0);
            return -EPROTO;
        }
    }
    rx->rejected++;
    return 0;
}

static int poll_receiver(const struct transfer *transfer, struct responder *receiver,
                         struct counters *counts)
{
    uint64_t last_progress = rte_get_timer_cycles(), last_frame = last_progress;
    for (;;) {
        uint32_t before = receiver->unique_packets;
        int result = 0;
        for (unsigned int path = 0; path < PATH_COUNT; path++) {
            struct rte_mbuf *packets[BURST];
            uint16_t n = rte_eth_rx_burst(transfer->paths[path].port, 0, packets, BURST);
            for (uint16_t i = 0; i < n; i++) {
                if (!result)
                    result = receive_frame(transfer, receiver, counts, path, packets[i],
                                           &last_frame);
                rte_pktmbuf_free(packets[i]);
            }
        }
        int completion_result = drain_completions(transfer, receiver->cq, counts);
        if (result || completion_result)
            return result ? result : completion_result;
        uint64_t now = rte_get_timer_cycles();
        poll_port_status(transfer, &receiver->node, now);
        if (receiver->unique_packets != before)
            last_progress = now;
        if (receiver->unique_packets == transfer->packets) {
            if (now - last_frame >= 8 * transfer->initial_rto + transfer->hz / 4)
                break;
        } else if (now - last_progress >= transfer->idle_timeout) {
            fprintf(stderr, "RX stalled after %u of %u segments, cack PSN %u, "
                            "max received PSN %u\n",
                    receiver->unique_packets, transfer->packets,
                    receiver->qp.cack_psn, receiver->qp.max_rcv_psn);
            return -ETIMEDOUT;
        }
        rte_pause();
    }

    if (mrc_wr_ack_msn(&receiver->wr) != transfer->work_requests ||
        (transfer->with_immediate && counts->immediate_completions != transfer->work_requests))
        return -EBADMSG;
    for (uint32_t i = 0; i < transfer->bytes; i++) {
        if (transfer->data[i] != pattern(i))
            return -EBADMSG;
    }
    return 0;
}

static int run_receiver(const struct transfer *transfer, struct counters *counts)
{
    struct responder *receiver = calloc(1, sizeof(*receiver));
    uint8_t *staged_payload = malloc(transfer->window * PMTU);
    int result = -ENOMEM;
    if (!receiver || !staged_payload)
        goto done;
    result = mrc_provider_cq_create(transfer->work_requests, &receiver->cq);
    if (result)
        goto done;

    struct mrc_wr_config config = {
        .pd = transfer->pd,
        .region = {
            .memory = transfer->data, .address = transfer->remote_address,
            .length = transfer->bytes, .r_key = transfer->remote_key,
        },
        .pmtu = PMTU, .initial_psn = transfer->initial_psn,
        .initial_msn = 1, .initial_rqmsn = 1,
        .max_wimm_inflight = transfer->max_wimm_inflight,
        .work = receiver->work, .work_capacity = transfer->window,
        .staged = receiver->staged, .staged_payload = staged_payload,
        .staged_capacity = transfer->window,
        .recv_queue = receiver->receive_queue, .recv_capacity = transfer->work_requests,
    };
    result = -EINVAL;
    if (!mrc_rx_init(&receiver->qp, transfer->initial_psn, transfer->window, receiver->slots) ||
        !mrc_wr_init(&receiver->wr, &config) ||
        !mrc_mpr_init(&receiver->mpr, transfer->dynamic_mpr, transfer->mpr_max))
        goto done;
    for (uint32_t id = 1; id <= transfer->posted_receives; id++) {
        if (!mrc_wr_post_receive(&receiver->wr, id))
            goto done;
    }
    result = poll_receiver(transfer, receiver, counts);
    if (result == -EPROTO)
        rte_delay_us_sleep(10000); /* Leave time to transmit the terminal NAK. */

done:
    if (receiver) {
        mrc_wr_destroy(&receiver->wr);
        int closed = destroy_cq(receiver->cq);
        if (!result)
            result = closed;
    }
    free(staged_payload);
    free(receiver);
    return result;
}

static bool number64(const char *value, uint64_t min, uint64_t max, uint64_t *out)
{
    if (*value < '0' || *value > '9')
        return false;
    char *end;
    errno = 0;
    int base = value[0] == '0' && (value[1] == 'x' || value[1] == 'X') ? 16 : 10;
    unsigned long long parsed = strtoull(value, &end, base);
    if (errno || *end || parsed < min || parsed > max)
        return false;
    *out = (uint64_t)parsed;
    return true;
}

static bool number(const char *value, uint32_t min, uint32_t max, uint32_t *out)
{
    uint64_t parsed;
    if (!number64(value, min, max, &parsed))
        return false;
    *out = (uint32_t)parsed;
    return true;
}

static bool parse_port(const char *device, uint16_t *port)
{
    uint32_t id;
    if (!number(device, 0, UINT16_MAX, &id))
        return rte_eth_dev_get_port_by_name(device, port) == 0;
    *port = (uint16_t)id;
    return rte_eth_dev_is_valid_port(*port);
}

static bool parse_args(int argc, char **argv, struct transfer *transfer)
{
    if (argc < 9 || (argc - 9) % 2 || (strcmp(argv[1], "tx") && strcmp(argv[1], "rx")) ||
        !number(argv[2], 0, 1U << 30, &transfer->wr_bytes))
        return false;
    transfer->sender = strcmp(argv[1], "tx") == 0;
    transfer->work_requests = 1;
    transfer->max_wimm_inflight = DEFAULT_WIMM_INFLIGHT;
    transfer->posted_receives = UINT32_MAX;
    transfer->wr_packets = transfer->wr_bytes ? (transfer->wr_bytes + PMTU - 1) / PMTU : 1;
    transfer->drop_after = UINT32_MAX;
    transfer->restore_after = UINT32_MAX;
    transfer->mpr_change_at = UINT32_MAX;
    transfer->trim_after = UINT32_MAX;

    struct in_addr local, peer;
    if (inet_pton(AF_INET, argv[3], &local) == 1 && inet_pton(AF_INET, argv[4], &peer) == 1) {
        transfer->ip_version = MRC_WIRE_IP4;
        transfer->local_ip = ntohl(local.s_addr);
        transfer->peer_ip = ntohl(peer.s_addr);
    } else if (inet_pton(AF_INET6, argv[3], transfer->local_ip6) == 1 &&
               inet_pton(AF_INET6, argv[4], transfer->peer_ip6) == 1) {
        transfer->ip_version = MRC_WIRE_IP6;
    } else {
        return false;
    }
    for (unsigned int i = 0; i < PATH_COUNT; i++) {
        struct path *path = &transfer->paths[i];
        if (!parse_port(argv[5 + 2 * i], &path->port) ||
            rte_ether_unformat_addr(argv[6 + 2 * i], &path->peer))
            return false;
    }
    if (transfer->paths[0].port == transfer->paths[1].port)
        return false;

    transfer->requestor_qpn = DEFAULT_REQUESTOR_QPN;
    transfer->responder_qpn = DEFAULT_RESPONDER_QPN;
    transfer->remote_address = default_remote_address;
    transfer->remote_key = DEFAULT_REMOTE_KEY;
    uint32_t timeout_ms = 5000, udp_port = MRC_UDP_DEFAULT_PORT, trim_dscp = TRIM_DSCP;
    uint32_t oob_port = 0, trim_nack = 1, srv6_srh = 0, port_stats = 0;
    bool local_sid = false, peer_sid = false;
    uint32_t exponent = 14, linear_retries = 3, exponential_retries = 4;
    uint32_t mpr = WINDOW / MRC_MPR_UNIT, p_key = DEFAULT_PARTITION_KEY;
    uint32_t dynamic_mpr = 0, mpr_change_to = 0;
    for (int i = 9; i < argc; i += 2) {
        const char *key = argv[i], *value = argv[i + 1];
        if (!strcmp(key, "--udp-port") && number(value, 1, UINT16_MAX, &udp_port))
            continue;
        if (!strcmp(key, "--initial-psn") && number(value, 0, MRC_PSN_MASK, &transfer->initial_psn))
            continue;
        if (!strcmp(key, "--oob-host") && value[0]) {
            transfer->oob_address = value;
            continue;
        }
        if (!strcmp(key, "--oob-port") && number(value, 1, UINT16_MAX, &oob_port))
            continue;
        if (!strcmp(key, "--trim-nack") && number(value, 0, 1, &trim_nack))
            continue;
        if (!strcmp(key, "--srv6-local") && inet_pton(AF_INET6, value, transfer->local_sid) == 1) {
            local_sid = true;
            continue;
        }
        if (!strcmp(key, "--srv6-peer") && inet_pton(AF_INET6, value, transfer->peer_sid) == 1) {
            peer_sid = true;
            continue;
        }
        if (!strcmp(key, "--srv6-srh") && number(value, 0, 1, &srv6_srh))
            continue;
        if (!strcmp(key, "--trim-dscp") && number(value, 1, 63, &trim_dscp) &&
            trim_dscp != CONTROL_DSCP)
            continue;
        if (!strcmp(key, "--requestor-qpn") && number(value, 3, MRC_PSN_MASK, &transfer->requestor_qpn))
            continue;
        if (!strcmp(key, "--responder-qpn") && number(value, 3, MRC_PSN_MASK, &transfer->responder_qpn))
            continue;
        if (!strcmp(key, "--p-key") && number(value, 1, UINT16_MAX, &p_key))
            continue;
        if (!strcmp(key, "--mpr") && number(value, 1, WINDOW / MRC_MPR_UNIT, &mpr))
            continue;
        if (!strcmp(key, "--dynamic-mpr") && number(value, 0, 1, &dynamic_mpr))
            continue;
        if (!transfer->sender && !strcmp(key, "--mpr-change-at") &&
            number(value, 1, MRC_PSN_HALF - 1, &transfer->mpr_change_at))
            continue;
        if (!transfer->sender && !strcmp(key, "--mpr-change-to") &&
            number(value, 1, WINDOW / MRC_MPR_UNIT, &mpr_change_to))
            continue;
        if (!strcmp(key, "--remote-key") && number(value, 0, UINT32_MAX, &transfer->remote_key)) {
            transfer->remote_key_configured = true;
            continue;
        }
        if (!strcmp(key, "--remote-address") &&
            number64(value, 0, UINT64_MAX, &transfer->remote_address))
            continue;
        if (!strcmp(key, "--work-requests") &&
            number(value, 1, MAX_WORK_REQUESTS, &transfer->work_requests))
            continue;
        if (!strcmp(key, "--max-wimm-inflight") &&
            number(value, 1, WINDOW, &transfer->max_wimm_inflight))
            continue;
        if (!transfer->sender && !strcmp(key, "--posted-receives") &&
            number(value, 0, MAX_WORK_REQUESTS, &transfer->posted_receives))
            continue;
        if (!strcmp(key, "--port-stats") && number(value, 0, 1, &port_stats))
            continue;
        if (!strcmp(key, "--ack-timeout") && number(value, 0, 24, &exponent))
            continue;
        if (!strcmp(key, "--linear-retries") && number(value, 0, 7, &linear_retries))
            continue;
        if (!strcmp(key, "--exp-retries") && number(value, 0, 25, &exponential_retries))
            continue;
        if (!strcmp(key, "--timeout-ms") && number(value, 1, 300000, &timeout_ms))
            continue;
        if (!transfer->sender && !strcmp(key, "--drop-final-acks") &&
            number(value, 0, 25, &transfer->final_ack_drops))
            continue;
        if (!strcmp(key, "--immediate") && number(value, 0, UINT32_MAX, &transfer->immediate)) {
            transfer->with_immediate = true;
            continue;
        }
        if (transfer->sender && !strcmp(key, "--deny-path-mask") &&
            number(value, 0, ALL_PATHS, &transfer->ev_deny_mask))
            continue;
        if (transfer->sender && !strcmp(key, "--restore-path-after") &&
            number(value, 1, MRC_PSN_HALF - 1, &transfer->restore_after))
            continue;
        if (transfer->sender && !strcmp(key, "--drop-path-after") &&
            number(value, 0, MRC_PSN_HALF - 1, &transfer->drop_after))
            continue;
        if (transfer->sender && !strcmp(key, "--trim-path-after") &&
            number(value, 0, MRC_PSN_HALF - 1, &transfer->trim_after))
            continue;
        return false;
    }

    uint64_t total_bytes = (uint64_t)transfer->wr_bytes * transfer->work_requests;
    uint64_t total_packets = (uint64_t)transfer->wr_packets * transfer->work_requests;
    if (total_bytes > (1U << 30) || total_packets >= MRC_PSN_HALF ||
        transfer->remote_address > UINT64_MAX - total_bytes ||
        (transfer->drop_after != UINT32_MAX && transfer->drop_after >= total_packets) ||
        (transfer->restore_after != UINT32_MAX &&
         (transfer->restore_after <= transfer->drop_after || transfer->restore_after >= total_packets)) ||
        (dynamic_mpr && !oob_port) ||
        ((transfer->mpr_change_at != UINT32_MAX) != (mpr_change_to != 0)) ||
        (mpr_change_to && (!dynamic_mpr || mpr_change_to > mpr ||
                           transfer->mpr_change_at >= total_packets)) ||
        (transfer->trim_after != UINT32_MAX && transfer->trim_after >= total_packets) ||
        (transfer->drop_after != UINT32_MAX && transfer->trim_after != UINT32_MAX) ||
        (!!transfer->oob_address != !!oob_port) || (oob_port && transfer->remote_key_configured) ||
        local_sid != peer_sid ||
        (local_sid && transfer->ip_version != MRC_WIRE_IP6) || (srv6_srh && !local_sid))
        return false;
    transfer->bytes = (uint32_t)total_bytes;
    transfer->packets = (uint32_t)total_packets;
    if (transfer->posted_receives == UINT32_MAX)
        transfer->posted_receives = transfer->with_immediate ? transfer->work_requests : 0;
    if (transfer->posted_receives > transfer->work_requests)
        return false;
    transfer->udp_port = (uint16_t)udp_port;
    transfer->window = mpr * MRC_MPR_UNIT;
    transfer->mpr_max = (uint8_t)mpr;
    transfer->mpr_change_to = (uint8_t)mpr_change_to;
    transfer->dynamic_mpr = dynamic_mpr != 0;
    transfer->partition_key = (uint16_t)p_key;
    transfer->peer_partition_key = (uint16_t)p_key;
    transfer->timeout_ms = timeout_ms;
    transfer->oob_port = (uint16_t)oob_port;
    transfer->trim_nack_enabled = trim_nack != 0;
    transfer->port_stats = port_stats != 0;
    transfer->srv6 = local_sid;
    transfer->srv6_srh = srv6_srh != 0;
    transfer->trim_dscp = (uint8_t)trim_dscp;
    transfer->timeout_exponent = exponent;
    transfer->linear_retries = linear_retries;
    transfer->exponential_retries = exponential_retries;
    transfer->hz = rte_get_timer_hz();
    uint64_t first_timeout;
    if (!mrc_local_ack_timeout_ns(exponent, linear_retries, exponential_retries, 0, &first_timeout))
        return false;
    transfer->initial_rto = ns_to_cycles(transfer, first_timeout);
    transfer->idle_timeout = ns_to_cycles(transfer, (uint64_t)timeout_ms * 1000000);
    return transfer->idle_timeout > transfer->initial_rto;
}

static int register_buffer(struct transfer *transfer)
{
    if (!transfer->oob_address)
        return 0;

    int result = mrc_provider_pd_create(1, &transfer->pd);
    if (result)
        return result;
    uint64_t iova = transfer->sender ? (uint64_t)(uintptr_t)transfer->data : transfer->remote_address;
    size_t allocation = transfer->bytes ? transfer->bytes : 1;
    unsigned int flags = transfer->sender ? 0 :
        MRC_PROVIDER_MR_LOCAL_WRITE | MRC_PROVIDER_MR_REMOTE_WRITE;
    result = mrc_provider_mr_register(transfer->pd, transfer->data, allocation, iova,
                                      flags, &transfer->buffer_keys);
    if (result)
        return result;
    if (!transfer->sender) {
        transfer->remote_key = transfer->buffer_keys.rkey;
        return 0;
    }

    const struct mrc_provider_mr_access source = {
        .iova = iova, .length = transfer->bytes, .key = transfer->buffer_keys.lkey,
        .operation = MRC_PROVIDER_MR_READ_LOCAL,
    };
    return mrc_provider_mr_acquire(transfer->pd, &source, &transfer->source_lease);
}

static int deregister_buffer(struct transfer *transfer)
{
    if (!transfer->pd)
        return 0;
    if (transfer->source_lease.owner) {
        int result = mrc_provider_mr_release(&transfer->source_lease);
        if (result)
            return result;
    }
    if (transfer->buffer_keys.lkey) {
        int result = mrc_provider_mr_deregister(transfer->pd, transfer->buffer_keys.lkey);
        if (result)
            return result;
    }
    return mrc_provider_pd_destroy(transfer->pd);
}

static int setup_qp(struct transfer *transfer)
{
    if (!transfer->oob_address)
        return 0;

    struct mrc_connect_offer local = {
        .receive = {
            .qpn = transfer->sender ? transfer->requestor_qpn : transfer->responder_qpn,
            .initial_psn = transfer->initial_psn, .initial_msn = 1, .initial_rqmsn = 1,
            .max_mpr = (uint8_t)(transfer->window / MRC_MPR_UNIT),
            .max_wimm = (uint16_t)transfer->max_wimm_inflight,
        },
        .pmtu = PMTU, .p_key = transfer->partition_key,
        .supported = MRC_CONNECT_TRIM_NACK | MRC_CONNECT_DYNAMIC_MPR,
        .offered = (transfer->trim_nack_enabled ? MRC_CONNECT_TRIM_NACK : 0) |
                   (transfer->dynamic_mpr ? MRC_CONNECT_DYNAMIC_MPR : 0),
    };
    if (!transfer->sender) {
        local.receive.memory = (struct mrc_connect_memory){
            .present = true, .address = transfer->remote_address,
            .length = transfer->bytes, .r_key = transfer->remote_key,
        };
    }

    struct mrc_connect_qp qp;
    int result = mrc_oob_exchange(transfer->oob_address, transfer->oob_port, !transfer->sender,
                                  transfer->timeout_ms, &local, &qp);
    if (result)
        return result;
    if (qp.pmtu != PMTU || qp.send.initial_msn != 1 || qp.send.initial_rqmsn != 1)
        return -EPROTONOSUPPORT;

    transfer->peer_partition_key = qp.peer_p_key;
    transfer->dynamic_mpr = qp.dynamic_mpr;
    transfer->trim_nack_enabled = (qp.local_responder_features & MRC_CONNECT_TRIM_NACK) != 0;
    if (transfer->sender) {
        transfer->responder_qpn = qp.send.qpn;
        transfer->initial_psn = qp.send.initial_psn;
        transfer->mpr_max = qp.send.max_mpr;
        uint32_t peer_window = qp.send.max_mpr * MRC_MPR_UNIT;
        if (peer_window < transfer->window)
            transfer->window = peer_window;
        if (transfer->with_immediate && !qp.send.max_wimm)
            return -EPROTONOSUPPORT;
        if (qp.send.max_wimm < transfer->max_wimm_inflight)
            transfer->max_wimm_inflight = qp.send.max_wimm;
        if (!mrc_connect_write_range(&qp.send, 0, transfer->bytes,
                                     &transfer->remote_address, &transfer->remote_key))
            return -EACCES;
    } else {
        transfer->requestor_qpn = qp.send.qpn;
    }

    fprintf(stderr, "Negotiated QP %u -> %u, PSN %u, window %u, WRITEIMM credits %u\n",
            transfer->requestor_qpn, transfer->responder_qpn, transfer->initial_psn,
            transfer->window, transfer->max_wimm_inflight);
    return 0;
}

struct port_snapshot {
    struct rte_eth_stats stats;
    bool valid;
};

static void print_diagnostics(const struct transfer *transfer, const struct counters *counts,
                              const struct port_snapshot start[PATH_COUNT], unsigned int opened)
{
    fprintf(stderr, "MRC progress: data TX=%u/%u retry=%u/%u data RX=%u/%u "
                    "SACK=%u transport-ACK=%u probe-reply=%u\n",
            counts->sent[0], counts->sent[1], counts->retries[0], counts->retries[1],
            counts->received[0], counts->received[1], counts->sacks,
            counts->transport_acks, counts->probe_replies);
    for (unsigned int path = 0; path < opened; path++) {
        const struct rx_counters *rx = &counts->rx[path];
        fprintf(stderr, "MRC path %u: RX frames=%" PRIu64 " matched=%" PRIu64
                        " decode-rejected=%" PRIu64 " other-peer=%" PRIu64 "\n",
                path, rx->frames, rx->matched, rx->rejected, rx->other_peer);
        struct rte_eth_stats end;
        if (!start[path].valid || rte_eth_stats_get(transfer->paths[path].port, &end))
            continue;
        const struct rte_eth_stats *base = &start[path].stats;
        fprintf(stderr, "DPDK path %u deltas: ipackets=%" PRIu64 " opackets=%" PRIu64
                        " imissed=%" PRIu64 " ierrors=%" PRIu64 " oerrors=%" PRIu64
                        " rx_nombuf=%" PRIu64 "\n",
                path, end.ipackets - base->ipackets, end.opackets - base->opackets,
                end.imissed - base->imissed, end.ierrors - base->ierrors,
                end.oerrors - base->oerrors, end.rx_nombuf - base->rx_nombuf);
    }
}

int main(int argc, char **argv)
{
    int consumed = rte_eal_init(argc, argv);
    if (consumed < 0)
        return EXIT_FAILURE;

    struct transfer transfer = {0};
    struct counters counts = {0};
    int result = -EINVAL;
    unsigned int opened = 0;
    struct port_snapshot initial_stats[PATH_COUNT] = {0};
    if (!parse_args(argc - consumed, argv + consumed, &transfer)) {
        fprintf(stderr, "Usage: %s [EAL options] -- tx|rx BYTES LOCAL_IP PEER_IP\n"
                        "       PORT0 PEER_MAC0 PORT1 PEER_MAC1 [--initial-psn N] [--udp-port N]\n"
                        "       [--requestor-qpn N] [--responder-qpn N] [--p-key N] [--mpr N]\n"
                        "       [--remote-key N] [--remote-address N] [--immediate N]\n"
                        "       [--work-requests N] [--max-wimm-inflight N] [--posted-receives N (rx)]\n"
                        "       [--ack-timeout N] [--linear-retries N]\n"
                        "       [--exp-retries N] [--timeout-ms N] [--drop-path-after N (tx)] [--deny-path-mask N (tx)]\n"
                        "       [--trim-dscp N] [--trim-nack 0|1] [--trim-path-after N (tx)]\n"
                        "       [--drop-final-acks N (rx)] [--oob-host IP --oob-port N]\n"
                        "       [--restore-path-after N (tx)] [--dynamic-mpr 0|1]\n"
                        "       [--mpr-change-at N --mpr-change-to N (rx)]\n"
                        "       [--srv6-local SID --srv6-peer SID] [--srv6-srh 0|1] [--port-stats 0|1]\n",
                argv[0]);
        goto done;
    }

    transfer.data = malloc(transfer.bytes ? transfer.bytes : 1);
    transfer.pool = mrc_dpdk_packet_pool();
    if (!transfer.data || !transfer.pool) {
        result = -ENOMEM;
        goto done;
    }
    if (transfer.sender) {
        for (uint32_t i = 0; i < transfer.bytes; i++)
            transfer.data[i] = pattern(i);
    }
    result = register_buffer(&transfer);
    if (result)
        goto done;
    for (; opened < PATH_COUNT; opened++) {
        struct path *path = &transfer.paths[opened];
        result = mrc_dpdk_open_port(path->port, transfer.pool, &path->local);
        if (result)
            goto done;
        initial_stats[opened].valid = rte_eth_stats_get(path->port, &initial_stats[opened].stats) == 0;
    }
    result = setup_qp(&transfer);
    if (result) {
        fprintf(stderr, "MRC QP setup failed: %s\n", strerror(-result));
        goto done;
    }
    result = transfer.sender ? run_sender(&transfer, &counts) : run_receiver(&transfer, &counts);
    if (!result)
        printf("{\"role\":\"%s\",\"bytes\":%u,\"packets\":%u,\"sent\":[%u,%u],"
               "\"received\":[%u,%u],\"retries\":[%u,%u],\"dropped\":%u,\"sacks\":%u,"
               "\"trimmed\":%u,\"trim_nacks\":%u,\"transport_acks\":%u,"
               "\"port_status_updates\":%u,\"port_status_replies\":%u,"
               "\"probe_requests\":%u,\"probe_replies\":%u,\"probe_sent\":[%u,%u],"
               "\"ev_skipped\":%u,\"ev_failed\":%u,\"ev_recovered\":%u,\"mpr_updates\":%u,"
               "\"dropped_transport_acks\":%u,"
               "\"duplicates\":%u,\"fast_retries\":%u,"
               "\"reorder_max\":%u,\"write_completions\":%u,\"immediate_completions\":%u,"
               "\"immediate_completed\":%s,\"registered_memory\":%s}\n",
               transfer.sender ? "tx" : "rx", transfer.bytes, transfer.packets,
               counts.sent[0], counts.sent[1], counts.received[0], counts.received[1],
               counts.retries[0], counts.retries[1], counts.dropped, counts.sacks,
               counts.trimmed, counts.trim_nacks, counts.transport_acks,
               counts.port_status_updates, counts.port_status_replies, counts.probe_requests,
               counts.probe_replies, counts.probe_sent[0], counts.probe_sent[1],
               counts.ev_skipped, counts.ev_failed, counts.ev_recovered,
               counts.mpr_updates, counts.dropped_transport_acks,
               counts.duplicates, counts.fast_retries,
               counts.reorder_max, counts.write_completions, counts.immediate_completions,
               counts.immediate_completions ? "true" : "false", transfer.pd ? "true" : "false");

done:
    if (opened && (result || transfer.port_stats))
        print_diagnostics(&transfer, &counts, initial_stats, opened);
    if (result)
        fprintf(stderr, "MRC WRITE failed: %s\n", strerror(-result));
    for (unsigned int i = 0; i < opened; i++)
        mrc_dpdk_close_port(transfer.paths[i].port);
    if (transfer.pool)
        rte_mempool_free(transfer.pool);
    int deregistered = deregister_buffer(&transfer);
    if (deregistered) {
        fprintf(stderr, "MRC buffer cleanup failed: %s\n", strerror(-deregistered));
        if (!result)
            result = deregistered;
    } else {
        free(transfer.data);
    }
    rte_eal_cleanup();
    return result ? EXIT_FAILURE : EXIT_SUCCESS;
}
