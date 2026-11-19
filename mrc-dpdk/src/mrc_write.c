#include "dpdk_io.h"
#include "mrc_cc.h"
#include "mrc_qp.h"
#include "mrc_wire.h"

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
    BURST = 32,
    FRAME_CAPACITY = 1514,
    SACK_THRESHOLD = 4096,
    UDP_HEADER_LEN = 8,
    NOMINAL_MTU = PMTU + UDP_HEADER_LEN + MRC_BTH_LEN + MRC_METH_LEN +
                  MRC_RETH_LEN + MRC_IMMDT_LEN + MRC_ICRC_LEN + MRC_CC_NOMINAL_HDRSIZE,
    INITIAL_CWND = 128 * NOMINAL_MTU,
    TARGET_QUEUE_NS = 20000,
    FAST_LOSS_GAP = 16,
    REQUESTOR_QPN = 0x101,
    RESPONDER_QPN = 0x102,
    PARTITION_KEY = 0xffff,
    REMOTE_KEY = 0x6d726301,
    DATA_DSCP = 0,
    CONTROL_DSCP = 48,
    TRIM_DSCP = 49,
    ACK_SYNDROME = 0x1f,
    INVALID_REQUEST_NAK = 0x61,
    REMOTE_ACCESS_NAK = 0x62,
};

static const uint64_t remote_address = UINT64_C(0x100000000);

struct path {
    uint16_t port;
    struct rte_ether_addr local;
    struct rte_ether_addr peer;
};

struct transfer {
    bool sender;
    bool with_immediate;
    uint32_t immediate;
    uint32_t bytes;
    uint32_t packets;
    uint32_t initial_psn;
    uint32_t local_ip;
    uint32_t peer_ip;
    uint32_t drop_after;
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
};

struct counters {
    uint32_t sent[PATH_COUNT];
    uint32_t received[PATH_COUNT];
    uint32_t retries[PATH_COUNT];
    uint32_t dropped;
    uint32_t trimmed;
    uint32_t trim_nacks;
    uint32_t fast_retries;
    uint32_t sacks;
    uint32_t transport_acks;
    uint32_t dropped_transport_acks;
    uint32_t duplicates;
    uint32_t reorder_max;
    bool immediate_completed;
};

struct send_record {
    unsigned int path;
    bool fast_retransmitted;
    bool nacked;
    bool counted_inflight;
    bool redundant_attempt;
};

struct sender {
    struct mrc_cc cc;
    struct mrc_tx_qp qp;
    struct mrc_tx_packet slots[WINDOW];
    struct send_record metadata[WINDOW];
    uint64_t path_retry_at[PATH_COUNT];
    uint64_t blocked_since[PATH_COUNT];
    uint32_t next;
    int32_t highest_sacked;
    uint16_t probe_id;
};

struct responder {
    struct mrc_rx_qp qp;
    uint8_t slots[WINDOW];
    uint32_t unique_packets;
    struct mrc_cc_rx cc;
    uint32_t immediate;
};

static uint8_t pattern(uint32_t offset)
{
    return (uint8_t)(offset ^ (offset >> 8) ^ (offset >> 16) ^ (offset >> 24));
}

static uint32_t length_at(const struct transfer *transfer, uint32_t index)
{
    uint32_t remaining = transfer->bytes - index * PMTU;
    return remaining < PMTU ? remaining : PMTU;
}

static uint8_t expected_opcode(const struct transfer *transfer, uint32_t index)
{
    if (transfer->packets == 1)
        return transfer->with_immediate ? MRC_WRITE_ONLY_IMM : MRC_WRITE_ONLY;
    if (index == 0)
        return MRC_WRITE_FIRST;
    if (index + 1 == transfer->packets)
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

static struct mrc_wire_packet packet_for(const struct transfer *transfer, unsigned int index,
                                          uint8_t opcode, uint32_t psn)
{
    const struct path *path = &transfer->paths[index];
    struct mrc_wire_packet packet = {
        .ipv4 = {
            .src_ip = transfer->local_ip, .dst_ip = transfer->peer_ip,
            .src_port = (uint16_t)((transfer->sender ? 0xc000 : 0xd000) + index),
            .dst_port = transfer->udp_port,
            .dscp = mrc_wire_is_write(opcode) ? DATA_DSCP : CONTROL_DSCP,
            .ecn = mrc_wire_is_write(opcode) ? 2 : 0,
        },
        .bth = {
            .opcode = opcode, .psn = psn, .p_key = PARTITION_KEY,
            .dqp = transfer->sender ? RESPONDER_QPN : REQUESTOR_QPN,
        },
    };
    memcpy(packet.ipv4.src_mac, path->local.addr_bytes, MRC_MAC_LEN);
    memcpy(packet.ipv4.dst_mac, path->peer.addr_bytes, MRC_MAC_LEN);
    return packet;
}

static bool transmit_frame(const struct transfer *transfer, unsigned int path,
                           const struct mrc_wire_packet *packet, bool inject_trim)
{
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(transfer->pool);
    if (!mbuf)
        return false;
    uint8_t *frame = rte_pktmbuf_mtod(mbuf, uint8_t *);
    size_t length;
    int result = mrc_wire_encode(frame, rte_pktmbuf_tailroom(mbuf), packet, &length);
    if (result) {
        rte_pktmbuf_free(mbuf);
        return false;
    }
    if (inject_trim) {
        /* Recreate switch trimming, preserving original UDP Length and omitting iCRC. */
        struct rte_ipv4_hdr ip;
        memcpy(&ip, frame + RTE_ETHER_HDR_LEN, sizeof(ip));
        length = MRC_WIRE_IPV4_HEADER_LEN + MRC_BTH_LEN + MRC_METH_LEN;
        ip.type_of_service = (uint8_t)((transfer->trim_dscp << 2) | (ip.type_of_service & 3));
        ip.total_length = rte_cpu_to_be_16((uint16_t)(length - RTE_ETHER_HDR_LEN));
        ip.hdr_checksum = 0;
        ip.hdr_checksum = rte_ipv4_cksum(&ip);
        memcpy(frame + RTE_ETHER_HDR_LEN, &ip, sizeof(ip));
    }
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
                         const struct mrc_wire_ipv4 *ipv4, const struct mrc_wire_bth *bth)
{
    const struct path *path = &transfer->paths[index];
    return ipv4->dst_ip == transfer->local_ip && ipv4->src_ip == transfer->peer_ip &&
           ipv4->dst_port == transfer->udp_port && bth->p_key == PARTITION_KEY &&
           bth->dqp == (transfer->sender ? REQUESTOR_QPN : RESPONDER_QPN) &&
           memcmp(ipv4->src_mac, path->peer.addr_bytes, MRC_MAC_LEN) == 0 &&
           memcmp(ipv4->dst_mac, path->local.addr_bytes, MRC_MAC_LEN) == 0;
}

static bool decode(const struct transfer *transfer, unsigned int index,
                   const struct rte_mbuf *mbuf, uint8_t scratch[FRAME_CAPACITY],
                   struct mrc_wire_packet *packet)
{
    size_t length;
    const uint8_t *frame = read_frame(mbuf, scratch, &length);
    return frame && !mrc_wire_decode(frame, length, packet) &&
           peer_matches(transfer, index, &packet->ipv4, &packet->bth);
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
    packet.body.write = (struct mrc_wire_write){
        .rqmsn = transfer->with_immediate ? 1 : 0,
        .msn = 1,
        .va = remote_address + index * PMTU,
        .r_key = REMOTE_KEY,
        .dma_len = transfer->bytes,
        .imm_data = transfer->immediate,
        .data = transfer->data + index * PMTU,
        .data_len = length_at(transfer, index),
    };

    if (path == 1 && sender->next >= transfer->drop_after) {
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
        uint32_t last_psn = mrc_psn_add(transfer->initial_psn, (int32_t)transfer->packets - 1);
        if (packet->bth.psn == last_psn && ack->msn != 1)
            return 0;
        int32_t last = mrc_psn_delta(packet->bth.psn, transfer->initial_psn);
        int32_t first = mrc_psn_delta(sender->qp.first_psn, transfer->initial_psn);
        if (last < first || last >= (int32_t)sender->next)
            return 0;
        for (int32_t index = first; index <= last; index++) {
            if (!retire_redundant_attempt(transfer, sender, (uint32_t)index))
                return -EPROTO;
        }
        if (mrc_tx_apply_transport_ack(&sender->qp, packet->bth.psn))
            counts->transport_acks++;
        return 0;
    }
    case MRC_RELIABILITY_SACK: {
        const struct mrc_wire_sack *wire = &packet->body.sack;
        if (wire->spdcid != REQUESTOR_QPN || wire->dpdcid != RESPONDER_QPN ||
            (!wire->probe_response && packet->bth.psn != wire->cack_psn))
            return 0;
        struct mrc_sack sack = {
            .cack_psn = wire->cack_psn, .ack_psn_offset = wire->ack_psn_offset,
            .sack_offset = wire->sack_offset, .bitmap = wire->bitmap, .ooo_count = wire->ooo_count,
        };
        if (!mrc_tx_apply_sack(&sender->qp, &sack, wire->probe_response))
            return 0;
        counts->sacks++;
        if (wire->mark != MRC_SACK_NONE && wire->entropy >= 0xc000 &&
            wire->entropy < 0xc000 + PATH_COUNT)
            sender->path_retry_at[wire->entropy - 0xc000] =
                now_cycles + 10 * transfer->initial_rto;
        struct mrc_cc_tx_sample sample;
        struct mrc_cc_tx_sample *rtt_sample = NULL;
        if (!wire->probe_response) {
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
        if (nack->spdcid != REQUESTOR_QPN || nack->dpdcid != RESPONDER_QPN ||
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
            if (!result && decode(transfer, path, packets[i], scratch, &packet))
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
        if (transfer->exponential_retries != 25 && packet->retransmissions >=
                transfer->linear_retries + transfer->exponential_retries) {
            fprintf(stderr, "Retry limit exhausted for PSN %" PRIu32 "\n", psn);
            return -ETIMEDOUT;
        }

        if (!retire_redundant_attempt(transfer, sender, index))
            return -EPROTO;
        if (!packet->received && record->counted_inflight) {
            if (!mrc_cc_on_inferred_loss(&sender->cc, nominal_at(transfer, index)))
                return -EPROTO;
            record->counted_inflight = false;
        }
        if (!mrc_cc_can_send(&sender->cc, nominal_at(transfer, index)))
            continue;

        unsigned int path = 1 - record->path;
        if (now < sender->path_retry_at[path] && now >= sender->path_retry_at[record->path])
            path = record->path;
        if (!send_write(transfer, sender, counts, path, index, true)) {
            path = 1 - path;
            if (!send_write(transfer, sender, counts, path, index, true))
                continue;
        }
        if (!packet->received && path != record->path)
            sender->path_retry_at[record->path] = now + 10 * transfer->initial_rto;
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

static void request_probe(const struct transfer *transfer, struct sender *sender,
                           unsigned int path)
{
    uint16_t probe_id = (uint16_t)(sender->probe_id + 1);
    struct mrc_wire_packet packet = packet_for(transfer, path, MRC_RELIABILITY_PROBE, 0);
    packet.body.probe = (struct mrc_wire_probe){
        .probe_id = probe_id, .spdcid = REQUESTOR_QPN, .dpdcid = RESPONDER_QPN,
    };
    if (transmit(transfer, path, &packet))
        sender->probe_id = probe_id;
}

static int run_sender(const struct transfer *transfer, struct counters *counts)
{
    struct sender sender = { .highest_sacked = -1 };
    if (!mrc_tx_init(&sender.qp, transfer->initial_psn, WINDOW, sender.slots) ||
        !mrc_cc_init(&sender.cc, NOMINAL_MTU, INITIAL_CWND, 0, TARGET_QUEUE_NS))
        return -EINVAL;

    uint64_t last_progress = rte_get_timer_cycles();
    uint64_t next_retry_scan = last_progress;
    uint64_t next_probe = last_progress + transfer->initial_rto;
    for (;;) {
        uint32_t previous_psn = sender.qp.first_psn;
        int result = receive_control(transfer, &sender, counts);
        if (result)
            return result;
        uint64_t now = rte_get_timer_cycles();
        if (sender.qp.first_psn != previous_psn)
            last_progress = now;

        uint32_t first = (uint32_t)mrc_psn_delta(sender.qp.first_psn, transfer->initial_psn);
        if (first == transfer->packets)
            return 0;
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
        if (now >= next_probe && sender.next < transfer->packets &&
            !mrc_cc_can_send(&sender.cc, nominal_at(transfer, sender.next))) {
            for (unsigned int path = 0; path < PATH_COUNT; path++)
                request_probe(transfer, &sender, path);
            next_probe = now + transfer->initial_rto;
        }
        for (unsigned int i = 0; i < BURST && sender.next < transfer->packets &&
             sender.next - first < WINDOW; i++) {
            uint32_t index = sender.next;
            if (!mrc_cc_can_send(&sender.cc, nominal_at(transfer, index)))
                break;
            unsigned int path = index % PATH_COUNT;
            if (now < sender.path_retry_at[path] && now >= sender.path_retry_at[1 - path])
                path = 1 - path;
            if (!send_write(transfer, &sender, counts, path, index, false)) {
                if (!sender.blocked_since[path])
                    sender.blocked_since[path] = now;
                if (now - sender.blocked_since[path] >= transfer->initial_rto)
                    sender.path_retry_at[path] = now + 10 * transfer->initial_rto;
                break;
            }
            sender.blocked_since[path] = 0;
            uint32_t psn = mrc_psn_add(transfer->initial_psn, (int32_t)index);
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

static void send_ack(const struct transfer *transfer, struct counters *counts,
                     unsigned int path, uint32_t psn, uint8_t syndrome, uint32_t msn)
{
    if (syndrome == ACK_SYNDROME && msn == 1 &&
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
        .mark = request->ipv4.ecn == 3 ? MRC_SACK_SKIP : MRC_SACK_NONE,
        .probe_response = probe,
        .ack_psn_offset = state.ack_psn_offset,
        .entropy = request->ipv4.src_port,
        .spdcid = REQUESTOR_QPN, .dpdcid = RESPONDER_QPN,
        .cack_psn = state.cack_psn, .sack_offset = state.sack_offset,
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

static int receive_write(const struct transfer *transfer, struct responder *receiver,
                         struct counters *counts, unsigned int path,
                         const struct mrc_wire_packet *packet)
{
    if (packet->bth.opcode == MRC_RELIABILITY_PROBE) {
        if (packet->body.probe.spdcid == REQUESTOR_QPN &&
            packet->body.probe.dpdcid == RESPONDER_QPN)
            send_sack(transfer, receiver, counts, path, packet, true);
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
                 receiver->unique_packets == transfer->packets);
        return 0;
    }

    uint32_t index = (uint32_t)mrc_psn_delta(psn, transfer->initial_psn);
    const struct mrc_wire_write *write = &packet->body.write;
    uint8_t syndrome = ACK_SYNDROME;
    if (index >= transfer->packets || packet->bth.opcode != expected_opcode(transfer, index) ||
        write->dma_len != transfer->bytes || write->data_len != length_at(transfer, index) ||
        write->msn != 1 || (transfer->with_immediate && write->rqmsn != 1)) {
        syndrome = INVALID_REQUEST_NAK;
    } else if (write->r_key != REMOTE_KEY || write->va != remote_address + index * PMTU) {
        syndrome = REMOTE_ACCESS_NAK;
    }
    if (syndrome != ACK_SYNDROME) {
        send_ack(transfer, counts, path, psn, syndrome, 0);
        return -EPROTO;
    }

    uint32_t previous_cack = receiver->qp.cack_psn;
    int32_t distance = mrc_psn_delta(receiver->qp.max_rcv_psn, psn);
    if (distance > 0 && (uint32_t)distance > counts->reorder_max)
        counts->reorder_max = (uint32_t)distance;

    size_t region_offset = (size_t)(write->va - remote_address);
    if (write->data_len)
        memcpy(transfer->data + region_offset, write->data, write->data_len);
    if (transfer->with_immediate && index + 1 == transfer->packets)
        receiver->immediate = write->imm_data;

    unsigned int flags = (packet->bth.ack_request ? MRC_RX_AR : 0) |
                         (packet->bth.retransmit ? MRC_RX_RTX : 0) |
                         (packet->ipv4.ecn == 3 ? MRC_RX_ECN : 0);
    bool sack_due;
    mrc_rx_record(&receiver->qp, psn, (uint32_t)write->data_len, flags, SACK_THRESHOLD, &sack_due);
    uint32_t nominal = nominal_size(packet->bth.opcode, write->data_len, packet->bth.timestamp);
    mrc_cc_rx_record(&receiver->cc, nominal, true, false);
    receiver->unique_packets++;
    counts->received[path]++;
    bool complete = receiver->unique_packets == transfer->packets;
    if (complete && transfer->with_immediate)
        counts->immediate_completed = true;
    if (sack_due || complete)
        send_sack(transfer, receiver, counts, path, packet, false);
    if (previous_cack != receiver->qp.cack_psn && (sack_due || complete))
        send_ack(transfer, counts, path, receiver->qp.cack_psn, ACK_SYNDROME, complete);
    return 0;
}

static void receive_trimmed(const struct transfer *transfer, const struct responder *receiver,
                            struct counters *counts, unsigned int path,
                            const struct mrc_wire_trimmed *trimmed)
{
    if (mrc_rx_classify(&receiver->qp, trimmed->bth.psn) != MRC_RX_NEW ||
        !mrc_cc_nominal_size(trimmed->original_udp_len))
        return;

    counts->trimmed++;
    struct mrc_wire_packet nack = packet_for(transfer, path, MRC_RELIABILITY_NACK,
                                              trimmed->bth.psn);
    nack.bth.retransmit = trimmed->bth.retransmit;
    nack.body.nack = (struct mrc_wire_nack){
        .reason = MRC_NACK_TRIMMED,
        .entropy = trimmed->ipv4.src_port,
        .spdcid = REQUESTOR_QPN, .dpdcid = RESPONDER_QPN,
        .nack_psn = trimmed->bth.psn,
        .tx_timestamp = trimmed->tx_timestamp,
        .original_udp_len = trimmed->original_udp_len,
    };
    if (transmit(transfer, path, &nack))
        counts->trim_nacks++;
}

static int receive_frame(const struct transfer *transfer, struct responder *receiver,
                         struct counters *counts, unsigned int path,
                         const struct rte_mbuf *mbuf, uint64_t *last_frame)
{
    uint8_t scratch[FRAME_CAPACITY];
    size_t length;
    const uint8_t *frame = read_frame(mbuf, scratch, &length);
    if (!frame)
        return 0;

    struct mrc_wire_trimmed trimmed;
    int result = mrc_wire_decode_trimmed(frame, length, transfer->trim_dscp, &trimmed);
    if (result != -EPROTONOSUPPORT) {
        if (!result && peer_matches(transfer, path, &trimmed.ipv4, &trimmed.bth)) {
            *last_frame = rte_get_timer_cycles();
            receive_trimmed(transfer, receiver, counts, path, &trimmed);
        }
        return 0;
    }

    struct mrc_wire_packet packet;
    result = mrc_wire_decode(frame, length, &packet);
    if (!result && peer_matches(transfer, path, &packet.ipv4, &packet.bth)) {
        *last_frame = rte_get_timer_cycles();
        return receive_write(transfer, receiver, counts, path, &packet);
    }

    struct mrc_wire_ipv4 ipv4;
    struct mrc_wire_bth bth;
    if (result == -EPROTONOSUPPORT && !mrc_wire_decode_header(frame, length, &ipv4, &bth) &&
        peer_matches(transfer, path, &ipv4, &bth) && !mrc_wire_is_write(bth.opcode) &&
        bth.opcode < MRC_TRANSPORT_ACK) {
        send_ack(transfer, counts, path, bth.psn, INVALID_REQUEST_NAK, 0);
        return -EPROTO;
    }
    return 0;
}

static int run_receiver(const struct transfer *transfer, struct counters *counts)
{
    struct responder receiver = {0};
    if (!mrc_rx_init(&receiver.qp, transfer->initial_psn, WINDOW, receiver.slots))
        return -EINVAL;
    uint64_t last_progress = rte_get_timer_cycles(), last_frame = last_progress;
    for (;;) {
        uint32_t before = receiver.unique_packets;
        int result = 0;
        for (unsigned int path = 0; path < PATH_COUNT; path++) {
            struct rte_mbuf *packets[BURST];
            uint16_t n = rte_eth_rx_burst(transfer->paths[path].port, 0, packets, BURST);
            for (uint16_t i = 0; i < n; i++) {
                if (!result)
                    result = receive_frame(transfer, &receiver, counts, path, packets[i],
                                           &last_frame);
                rte_pktmbuf_free(packets[i]);
            }
        }
        if (result)
            return result;
        uint64_t now = rte_get_timer_cycles();
        if (receiver.unique_packets != before)
            last_progress = now;
        if (receiver.unique_packets == transfer->packets) {
            if (now - last_frame >= 8 * transfer->initial_rto + transfer->hz / 4)
                break;
        } else if (now - last_progress >= transfer->idle_timeout) {
            return -ETIMEDOUT;
        }
        rte_pause();
    }

    if (transfer->with_immediate && receiver.immediate != transfer->immediate)
        return -EBADMSG;
    for (uint32_t i = 0; i < transfer->bytes; i++) {
        if (transfer->data[i] != pattern(i))
            return -EBADMSG;
    }
    return 0;
}

static bool number(const char *value, uint32_t min, uint32_t max, uint32_t *out)
{
    if (*value < '0' || *value > '9')
        return false;
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno || *end || parsed < min || parsed > max)
        return false;
    *out = (uint32_t)parsed;
    return true;
}

static bool parse_args(int argc, char **argv, struct transfer *transfer)
{
    if (argc < 9 || (argc - 9) % 2 || (strcmp(argv[1], "tx") && strcmp(argv[1], "rx")) ||
        !number(argv[2], 0, 1U << 30, &transfer->bytes))
        return false;
    transfer->sender = strcmp(argv[1], "tx") == 0;
    transfer->packets = transfer->bytes ? (transfer->bytes + PMTU - 1) / PMTU : 1;
    transfer->drop_after = UINT32_MAX;
    transfer->trim_after = UINT32_MAX;

    struct in_addr ip;
    if (inet_pton(AF_INET, argv[3], &ip) != 1)
        return false;
    transfer->local_ip = ntohl(ip.s_addr);
    if (inet_pton(AF_INET, argv[4], &ip) != 1)
        return false;
    transfer->peer_ip = ntohl(ip.s_addr);
    for (unsigned int i = 0; i < PATH_COUNT; i++) {
        uint32_t port;
        if (!number(argv[5 + 2 * i], 0, UINT16_MAX, &port) ||
            rte_ether_unformat_addr(argv[6 + 2 * i], &transfer->paths[i].peer))
            return false;
        transfer->paths[i].port = (uint16_t)port;
    }
    if (transfer->paths[0].port == transfer->paths[1].port)
        return false;

    uint32_t timeout_ms = 5000, udp_port = MRC_UDP_DEFAULT_PORT, trim_dscp = TRIM_DSCP;
    uint32_t exponent = 14, linear_retries = 3, exponential_retries = 4;
    for (int i = 9; i < argc; i += 2) {
        const char *key = argv[i], *value = argv[i + 1];
        if (!strcmp(key, "--udp-port") && number(value, 1, UINT16_MAX, &udp_port))
            continue;
        if (!strcmp(key, "--initial-psn") && number(value, 0, MRC_PSN_MASK, &transfer->initial_psn))
            continue;
        if (!strcmp(key, "--trim-dscp") && number(value, 1, 63, &trim_dscp) &&
            trim_dscp != CONTROL_DSCP)
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
        if (transfer->sender && !strcmp(key, "--drop-path-after") &&
            number(value, 0, transfer->packets - 1, &transfer->drop_after))
            continue;
        if (transfer->sender && !strcmp(key, "--trim-path-after") &&
            number(value, 0, transfer->packets - 1, &transfer->trim_after))
            continue;
        return false;
    }

    if (transfer->drop_after != UINT32_MAX && transfer->trim_after != UINT32_MAX)
        return false;
    transfer->udp_port = (uint16_t)udp_port;
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

int main(int argc, char **argv)
{
    int consumed = rte_eal_init(argc, argv);
    if (consumed < 0)
        return EXIT_FAILURE;

    struct transfer transfer = {0};
    struct counters counts = {0};
    int result = -EINVAL;
    unsigned int opened = 0;
    if (!parse_args(argc - consumed, argv + consumed, &transfer)) {
        fprintf(stderr, "Usage: %s [EAL options] -- tx|rx BYTES LOCAL_IPV4 PEER_IPV4\n"
                        "       PORT0 PEER_MAC0 PORT1 PEER_MAC1 [--initial-psn N] [--udp-port N]\n"
                        "       [--immediate N] [--ack-timeout N] [--linear-retries N]\n"
                        "       [--exp-retries N] [--timeout-ms N] [--drop-path-after N (tx)]\n"
                        "       [--trim-dscp N] [--trim-path-after N (tx)] [--drop-final-acks N (rx)]\n",
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
    for (; opened < PATH_COUNT; opened++) {
        struct path *path = &transfer.paths[opened];
        result = mrc_dpdk_open_port(path->port, transfer.pool, &path->local);
        if (result)
            goto done;
    }
    result = transfer.sender ? run_sender(&transfer, &counts) : run_receiver(&transfer, &counts);
    if (!result)
        printf("{\"role\":\"%s\",\"bytes\":%u,\"packets\":%u,\"sent\":[%u,%u],"
               "\"received\":[%u,%u],\"retries\":[%u,%u],\"dropped\":%u,\"sacks\":%u,"
               "\"trimmed\":%u,\"trim_nacks\":%u,\"transport_acks\":%u,"
               "\"dropped_transport_acks\":%u,"
               "\"duplicates\":%u,\"fast_retries\":%u,"
               "\"reorder_max\":%u,\"immediate_completed\":%s}\n",
               transfer.sender ? "tx" : "rx", transfer.bytes, transfer.packets,
               counts.sent[0], counts.sent[1], counts.received[0], counts.received[1],
               counts.retries[0], counts.retries[1], counts.dropped, counts.sacks,
               counts.trimmed, counts.trim_nacks, counts.transport_acks, counts.dropped_transport_acks,
               counts.duplicates, counts.fast_retries,
               counts.reorder_max, counts.immediate_completed ? "true" : "false");

done:
    if (result)
        fprintf(stderr, "MRC WRITE failed: %s\n", strerror(-result));
    for (unsigned int i = 0; i < opened; i++)
        mrc_dpdk_close_port(transfer.paths[i].port);
    if (transfer.pool)
        rte_mempool_free(transfer.pool);
    free(transfer.data);
    rte_eal_cleanup();
    return result ? EXIT_FAILURE : EXIT_SUCCESS;
}
