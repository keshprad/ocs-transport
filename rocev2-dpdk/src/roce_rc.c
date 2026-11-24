#include "roce_rc.h"

#include <string.h>

enum { ACK_INTERVAL = 32, PSN_HALF_RANGE = (ROCE_PSN_MASK + 1) / 2 };

bool roce_valid_pmtu(uint16_t pmtu)
{
    return pmtu >= 256 && pmtu <= ROCE_MAX_PMTU && (pmtu & (pmtu - 1)) == 0;
}

static struct roce_packet outgoing(const struct roce_qp *qp, enum roce_opcode opcode,
                                   uint32_t psn)
{
    return (struct roce_packet) {
        .src = qp->local, .dst = qp->peer, .opcode = opcode,
        .qpn = qp->peer_qpn, .psn = psn & ROCE_PSN_MASK,
        .pkey = ROCE_PKEY,
        .udp_source = qp->udp_source ? qp->udp_source :
            (uint16_t)(0xc000 | ((qp->local_qpn ^ qp->peer_qpn) & 0x3fff)),
    };
}

static bool for_qp(const struct roce_packet *packet, const struct roce_qp *qp)
{
    /* The local partition is a full member of the default partition. */
    return packet->qpn == qp->local_qpn && (packet->pkey & 0x7fff) == 0x7fff &&
           packet->src.ipv4 == qp->peer.ipv4 && packet->dst.ipv4 == qp->local.ipv4;
}

bool roce_requester_init(struct roce_requester *r, const struct roce_qp *qp,
                         uint32_t psn, struct roce_region remote,
                         const uint8_t *source, unsigned int retry_limit,
                         uint32_t window, uint32_t writes)
{
    if (!roce_valid_pmtu(qp->pmtu) || psn > ROCE_PSN_MASK || !remote.size || !source ||
        remote.size > UINT32_MAX || !writes || !window || window > ROCE_SEND_WINDOW_MAX)
        return false;
    uint32_t packets = (uint32_t)((remote.size - 1) / qp->pmtu + 1);
    uint64_t bytes = remote.size * writes;
    if (packets > UINT32_MAX / writes || bytes > SIZE_MAX ||
        bytes - 1 > UINT64_MAX - remote.address)
        return false;
    *r = (struct roce_requester) {
        .qp = *qp, .remote = remote, .source = source, .initial_psn = psn,
        .packets_per_write = packets, .total = packets * writes, .window = window,
        .retry_limit = retry_limit, .retries_left = retry_limit,
    };
    return true;
}

void roce_requester_write_packet(const struct roce_requester *r, uint32_t index,
                                 uint32_t segment, const uint8_t *payload,
                                 uint64_t remote_address, uint32_t length,
                                 struct roce_packet *packet)
{
    size_t segment_offset = (size_t)segment * r->qp.pmtu;
    size_t remaining = length - segment_offset;
    bool first = segment == 0;
    bool last = remaining <= r->qp.pmtu;
    enum roce_opcode opcode;
    if (first)
        opcode = last ? ROCE_WRITE_ONLY : ROCE_WRITE_FIRST;
    else
        opcode = last ? ROCE_WRITE_LAST : ROCE_WRITE_MIDDLE;
    *packet = outgoing(&r->qp, opcode, r->initial_psn + index);
    packet->payload = payload;
    packet->payload_length = last ? remaining : r->qp.pmtu;
    packet->ack_requested = last || (index + 1) % ACK_INTERVAL == 0 ||
                            index + 1 - r->acked == r->window;
    packet->remote_address = remote_address;
    packet->rkey = r->remote.rkey;
    packet->write_length = length;
}

uint32_t roce_requester_stage(const struct roce_requester *r, uint32_t staged,
                              struct roce_packet *packet)
{
    if (!roce_requester_can_stage(r, staged))
        return UINT32_MAX;
    uint32_t index = r->next + staged;
    uint32_t write = index / r->packets_per_write;
    uint32_t segment = index % r->packets_per_write;
    size_t offset = (size_t)write * r->remote.size;
    roce_requester_write_packet(r, index, segment, r->source + offset + (size_t)segment * r->qp.pmtu,
                                r->remote.address + offset, (uint32_t)r->remote.size, packet);
    return write;
}

void roce_requester_sent(struct roce_requester *r, uint32_t count, uint64_t now)
{
    if (!count)
        return;
    if (r->next == r->acked)
        r->last_progress = now;
    r->next += count;
    if (r->next > r->sent)
        r->sent = r->next;
}

static void advance(struct roce_requester *r, uint32_t next, uint64_t now)
{
    if (next > r->acked) {
        r->acked = next;
        if (r->next < next)
            r->next = next;
        r->retries_left = r->retry_limit;
        r->last_progress = now;
    }
}

static void retry(struct roce_requester *r, uint64_t now)
{
    if (!r->retries_left) {
        r->failure = ROCE_REQUESTER_RETRY_EXCEEDED;
        return;
    }
    --r->retries_left;
    ++r->retries;
    r->next = r->acked;
    r->last_progress = now;
}

void roce_requester_feedback(struct roce_requester *r,
                             const struct roce_packet *packet, uint64_t now)
{
    if (r->failure || packet->opcode != ROCE_ACK || !for_qp(packet, &r->qp))
        return;
    /* Only PSNs in the current outstanding window can acknowledge data. */
    uint32_t delta = (packet->psn - r->initial_psn - r->acked) & ROCE_PSN_MASK;
    if (delta >= r->sent - r->acked)
        return;
    uint32_t index = r->acked + delta;

    if (packet->syndrome <= ROCE_ACK_NO_CREDIT) {
        advance(r, index + 1, now);
    } else if (packet->syndrome == ROCE_NAK_SEQUENCE && index < r->next) {
        advance(r, index, now);
        retry(r, now);
    } else if (packet->syndrome >= ROCE_NAK_INVALID && packet->syndrome <= ROCE_NAK_OPERATION) {
        /* A fatal NAK stops at the affected WRITE's packet. Earlier WRITEs can complete. */
        advance(r, index, now);
        r->failure = (enum roce_requester_failure)packet->syndrome;
    }
}

void roce_requester_poll_timer(struct roce_requester *r, uint64_t now, uint64_t timeout)
{
    if (!r->failure && r->acked < r->sent && now - r->last_progress >= timeout)
        retry(r, now);
}

static bool respond(const struct roce_responder *r, struct roce_packet *packet,
                    uint8_t syndrome, uint32_t psn)
{
    *packet = outgoing(&r->qp, ROCE_ACK, psn);
    packet->syndrome = syndrome;
    packet->msn = r->msn;
    return true;
}

static bool starts_write(const struct roce_packet *packet)
{
    return packet->opcode == ROCE_WRITE_FIRST || packet->opcode == ROCE_WRITE_ONLY;
}

static bool valid_segment(const struct roce_responder *r, const struct roce_packet *packet,
                          bool first, uint32_t remaining)
{
    bool last = packet->opcode == ROCE_WRITE_LAST || packet->opcode == ROCE_WRITE_ONLY;
    return first == (r->remaining == 0) && remaining && last == (remaining <= r->qp.pmtu) &&
           packet->payload_length == (last ? remaining : r->qp.pmtu);
}

static bool valid_region(const struct roce_region *region, const struct roce_packet *packet)
{
    uint64_t offset = packet->remote_address - region->address;
    return packet->rkey == region->rkey && packet->remote_address >= region->address &&
           offset <= region->size && packet->write_length <= region->size - offset;
}

bool roce_responder_can_establish(const struct roce_responder *r, const struct roce_packet *packet)
{
    return !r->failed && !r->accepted && packet->psn == r->expected && for_qp(packet, &r->qp) &&
           starts_write(packet) && valid_segment(r, packet, true, packet->write_length) &&
           valid_region(&r->region, packet);
}

bool roce_responder_receive(struct roce_responder *r, const struct roce_packet *packet,
                            struct roce_packet *feedback)
{
    if (r->failed || packet->opcode == ROCE_ACK || !for_qp(packet, &r->qp))
        return false;

    uint32_t distance = (packet->psn - r->expected) & ROCE_PSN_MASK;
    if (distance > PSN_HALF_RANGE) {
        uint32_t behind = (r->expected - packet->psn) & ROCE_PSN_MASK;
        if (behind <= r->accepted)
            return respond(r, feedback, ROCE_ACK_NO_CREDIT, r->expected - 1);
        return false;
    }
    if (distance) {
        if (distance == PSN_HALF_RANGE || r->reported_gap)
            return false;
        r->reported_gap = true;
        return respond(r, feedback, ROCE_NAK_SEQUENCE, r->expected);
    }

    bool first = starts_write(packet);
    bool last = packet->opcode == ROCE_WRITE_LAST || packet->opcode == ROCE_WRITE_ONLY;
    uint32_t remaining = first ? packet->write_length : r->remaining;
    if (!valid_segment(r, packet, first, remaining)) {
        r->failed = true;
        return respond(r, feedback, ROCE_NAK_INVALID, r->expected);
    }

    if (first) {
        if (!valid_region(&r->region, packet)) {
            r->failed = true;
            return respond(r, feedback, ROCE_NAK_ACCESS, r->expected);
        }
        r->offset = packet->remote_address - r->region.address;
    }

    size_t expected_length = packet->payload_length;
    memcpy(r->buffer + r->offset, packet->payload, expected_length);
    r->offset += (uint32_t)expected_length;
    r->remaining = remaining - (uint32_t)expected_length;
    r->expected = (r->expected + 1) & ROCE_PSN_MASK;
    ++r->accepted;
    r->bytes_received += expected_length;
    r->reported_gap = false;
    if (last)
        r->msn = (r->msn + 1) & ROCE_PSN_MASK;

    if (packet->ack_requested)
        return respond(r, feedback, ROCE_ACK_NO_CREDIT, r->expected - 1);
    return false;
}
