#include "dpdk_io.h"
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
#include <rte_mbuf.h>
#include <rte_pause.h>

enum {
    PATH_COUNT = 2,
    PMTU = 1024,
    WINDOW = 1024,
    BURST = 32,
    FRAME_CAPACITY = 1514,
    SACK_THRESHOLD = 4096,
    FAST_LOSS_GAP = 16,
    REQUESTOR_QPN = 0x101,
    RESPONDER_QPN = 0x102,
    PARTITION_KEY = 0xffff,
    REMOTE_KEY = 0x6d726301,
    DATA_DSCP = 0,
    CONTROL_DSCP = 48,
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
    uint32_t final_ack_drops;
    uint16_t udp_port;
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
};

struct sender {
    struct mrc_tx_qp qp;
    struct mrc_tx_packet slots[WINDOW];
    struct send_record metadata[WINDOW];
    uint64_t path_retry_at[PATH_COUNT];
    uint64_t blocked_since[PATH_COUNT];
    uint32_t next;
    int32_t highest_sacked;
};

struct responder {
    struct mrc_rx_qp qp;
    uint8_t slots[WINDOW];
    uint32_t unique_packets;
    uint64_t nominal_bytes;
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

static bool transmit(const struct transfer *transfer, unsigned int path,
                     const struct mrc_wire_packet *packet)
{
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(transfer->pool);
    if (!mbuf)
        return false;
    uint8_t *frame = rte_pktmbuf_mtod(mbuf, uint8_t *);
    size_t length;
    int result = mrc_wire_encode(frame, rte_pktmbuf_tailroom(mbuf), packet, &length);
    if (result || !rte_pktmbuf_append(mbuf, (uint16_t)length)) {
        rte_pktmbuf_free(mbuf);
        return false;
    }
    return mrc_dpdk_transmit(transfer->paths[path].port, mbuf);
}

static bool decode(const struct transfer *transfer, unsigned int index,
                   const struct rte_mbuf *mbuf, uint8_t scratch[FRAME_CAPACITY],
                   struct mrc_wire_packet *packet)
{
    uint32_t length = rte_pktmbuf_pkt_len(mbuf);
    if (length > FRAME_CAPACITY)
        return false;
    const uint8_t *frame = rte_pktmbuf_read(mbuf, 0, length, scratch);
    if (!frame || mrc_wire_decode(frame, length, packet))
        return false;
    const struct path *path = &transfer->paths[index];
    return packet->ipv4.dst_ip == transfer->local_ip &&
           packet->ipv4.src_ip == transfer->peer_ip &&
           packet->ipv4.dst_port == transfer->udp_port &&
           packet->bth.p_key == PARTITION_KEY &&
           packet->bth.dqp == (transfer->sender ? REQUESTOR_QPN : RESPONDER_QPN) &&
           memcmp(packet->ipv4.src_mac, path->peer.addr_bytes, MRC_MAC_LEN) == 0 &&
           memcmp(packet->ipv4.dst_mac, path->local.addr_bytes, MRC_MAC_LEN) == 0;
}

static bool send_write(const struct transfer *transfer, struct sender *sender,
                       struct counters *counts, unsigned int path, uint32_t index,
                       bool retry)
{
    struct mrc_wire_packet packet = packet_for(transfer, path, expected_opcode(transfer, index),
        mrc_psn_add(transfer->initial_psn, (int32_t)index));
    packet.bth.retransmit = retry;
    packet.bth.ack_request = retry || (index + 1) % BURST == 0 ||
                             index + 1 == transfer->packets;
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
    } else if (!transmit(transfer, path, &packet)) {
        return false;
    }

    if (retry)
        counts->retries[path]++;
    else
        counts->sent[path]++;
    return true;
}

static int apply_control(const struct transfer *transfer, struct sender *sender,
                         struct counters *counts, const struct mrc_wire_packet *packet)
{
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
            sender->metadata[index % WINDOW].nacked = true;
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
        record->nacked = false;
        record->fast_retransmitted |= fast;
        counts->fast_retries += fast;
    }
    return 0;
}

static int run_sender(const struct transfer *transfer, struct counters *counts)
{
    struct sender sender = { .highest_sacked = -1 };
    if (!mrc_tx_init(&sender.qp, transfer->initial_psn, WINDOW, sender.slots))
        return -EINVAL;

    uint64_t last_progress = rte_get_timer_cycles();
    uint64_t next_retry_scan = last_progress;
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
        if (now - last_progress >= transfer->idle_timeout)
            return -ETIMEDOUT;
        if (now >= next_retry_scan) {
            result = retry_due_packets(transfer, &sender, counts, first, now);
            if (result)
                return result;
            next_retry_scan = now + transfer->initial_rto / 32 + 1;
        }
        for (unsigned int i = 0; i < BURST && sender.next < transfer->packets &&
             sender.next - first < WINDOW; i++) {
            uint32_t index = sender.next;
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
            sender.metadata[index % WINDOW] = (struct send_record){ .path = path };
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
        .rcvd_bytes = (uint32_t)((receiver->nominal_bytes + 255) / 256) & MRC_PSN_MASK,
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
    /* UDP, BTH, METH, RETH, optional headers, payload, pad, iCRC, and nominal 40 bytes. */
    size_t body = MRC_BTH_LEN + MRC_METH_LEN + MRC_RETH_LEN + write->data_len;
    if (packet->bth.timestamp)
        body += MRC_TSETH_LEN;
    if (packet->bth.opcode == MRC_WRITE_LAST_IMM || packet->bth.opcode == MRC_WRITE_ONLY_IMM)
        body += MRC_IMMDT_LEN;
    receiver->nominal_bytes += 8 + ((body + 3) & ~(size_t)3) + MRC_ICRC_LEN + 40;
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
                uint8_t scratch[FRAME_CAPACITY];
                struct mrc_wire_packet packet;
                if (!result && decode(transfer, path, packets[i], scratch, &packet)) {
                    result = receive_write(transfer, &receiver, counts, path, &packet);
                    last_frame = rte_get_timer_cycles();
                }
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

    uint32_t timeout_ms = 5000, udp_port = MRC_UDP_DEFAULT_PORT;
    uint32_t exponent = 14, linear_retries = 3, exponential_retries = 4;
    for (int i = 9; i < argc; i += 2) {
        const char *key = argv[i], *value = argv[i + 1];
        if (!strcmp(key, "--udp-port") && number(value, 1, UINT16_MAX, &udp_port))
            continue;
        if (!strcmp(key, "--initial-psn") && number(value, 0, MRC_PSN_MASK, &transfer->initial_psn))
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
        return false;
    }

    transfer->udp_port = (uint16_t)udp_port;
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
                        "       [--drop-final-acks N (rx)]\n",
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
               "\"transport_acks\":%u,\"dropped_transport_acks\":%u,"
               "\"duplicates\":%u,\"fast_retries\":%u,"
               "\"reorder_max\":%u,\"immediate_completed\":%s}\n",
               transfer.sender ? "tx" : "rx", transfer.bytes, transfer.packets,
               counts.sent[0], counts.sent[1], counts.received[0], counts.received[1],
               counts.retries[0], counts.retries[1], counts.dropped, counts.sacks,
               counts.transport_acks, counts.dropped_transport_acks,
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
