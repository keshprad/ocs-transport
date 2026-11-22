#include "mrc_qp.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
    RECEIVED = 1u << 0,
    AR_PENDING = 1u << 1,
    MIN_ACK_PACKET_SIZE = 1024,
};

int32_t mrc_psn_delta(uint32_t psn, uint32_t reference)
{
    uint32_t distance = (psn - reference) & MRC_PSN_MASK;
    if (distance >= MRC_PSN_HALF)
        return (int32_t)distance - (int32_t)(MRC_PSN_MASK + 1);
    return (int32_t)distance;
}

uint32_t mrc_psn_add(uint32_t psn, int32_t delta)
{
    return (psn + (uint32_t)delta) & MRC_PSN_MASK;
}

static bool valid_window(uint32_t window)
{
    return window >= MRC_MPR_UNIT && window <= MRC_MPR_MAX &&
           window % MRC_MPR_UNIT == 0;
}

bool mrc_rx_init(struct mrc_rx_qp *qp, uint32_t initial_psn, uint32_t window,
                 uint8_t *slots)
{
    if (!qp || !slots || initial_psn > MRC_PSN_MASK || !valid_window(window))
        return false;

    uint32_t initial_cack = mrc_psn_add(initial_psn, -1);
    memset(slots, 0, window);
    *qp = (struct mrc_rx_qp) {
        .slots = slots,
        .window = window,
        .cack_psn = initial_cack,
        .max_rcv_psn = initial_cack,
        .lowest_unsacked_psn = initial_cack,
    };
    return true;
}

enum mrc_rx_disposition mrc_rx_classify(const struct mrc_rx_qp *qp, uint32_t psn)
{
    if (psn > MRC_PSN_MASK)
        return MRC_RX_INVALID_PSN;

    uint32_t epsn = mrc_psn_add(qp->cack_psn, 1);
    int32_t offset = mrc_psn_delta(psn, epsn);
    if (offset < 0) {
        /* Pre-start PSNs were never received. The exact half-space is invalid. */
        return (uint32_t)(-offset) <= qp->contiguous_history
               ? MRC_RX_DUPLICATE : MRC_RX_INVALID_PSN;
    }
    if ((uint32_t)offset >= qp->window)
        return MRC_RX_AHEAD_OF_WINDOW;

    uint32_t index = (qp->first_index + (uint32_t)offset) % qp->window;
    return qp->slots[index] & RECEIVED ? MRC_RX_DUPLICATE : MRC_RX_NEW;
}

enum mrc_rx_disposition mrc_rx_record(struct mrc_rx_qp *qp, uint32_t psn,
                                      uint32_t payload_bytes, unsigned int flags,
                                      uint32_t sack_threshold, bool *sack_due)
{
    enum mrc_rx_disposition disposition = mrc_rx_classify(qp, psn);
    *sack_due = false;
    if (disposition == MRC_RX_INVALID_PSN || disposition == MRC_RX_AHEAD_OF_WINDOW)
        return disposition;

    uint32_t old_cack = qp->cack_psn;
    int32_t offset = mrc_psn_delta(psn, mrc_psn_add(old_cack, 1));
    if (disposition == MRC_RX_NEW) {
        uint32_t index = (qp->first_index + (uint32_t)offset) % qp->window;
        qp->slots[index] = RECEIVED | ((flags & MRC_RX_AR) ? AR_PENDING : 0);
        qp->ooo_count++;
        if (mrc_psn_delta(psn, qp->max_rcv_psn) > 0)
            qp->max_rcv_psn = psn;

        while (qp->slots[qp->first_index] & RECEIVED) {
            if (qp->slots[qp->first_index] & AR_PENDING)
                *sack_due = true;
            qp->slots[qp->first_index] = 0;
            qp->first_index = (qp->first_index + 1) % qp->window;
            qp->cack_psn = mrc_psn_add(qp->cack_psn, 1);
            if (qp->contiguous_history < MRC_PSN_HALF - 1)
                qp->contiguous_history++;
            qp->ooo_count--;
        }
    } else if (offset >= 0 && (flags & MRC_RX_AR)) {
        /* A retry can request an ACK for an already tracked PSN beyond a gap. */
        uint32_t index = (qp->first_index + (uint32_t)offset) % qp->window;
        qp->slots[index] |= AR_PENDING;
    }

    qp->sack_trigger_count += payload_bytes > MIN_ACK_PACKET_SIZE
                             ? payload_bytes : MIN_ACK_PACKET_SIZE;
    *sack_due = *sack_due || qp->sack_trigger_count > sack_threshold ||
                (flags & (MRC_RX_AR | MRC_RX_RTX | MRC_RX_ECN));

    /* §7.5.2.2 keeps the lower edge until a SACK is sent across it. */
    if (!*sack_due) {
        if (qp->cack_psn != old_cack) {
            if (mrc_psn_delta(qp->cack_psn, qp->lowest_unsacked_psn) >= 0)
                qp->lowest_unsacked_psn = qp->cack_psn;
        } else if (disposition == MRC_RX_NEW &&
                   mrc_psn_delta(psn, qp->lowest_unsacked_psn) < 0) {
            qp->lowest_unsacked_psn = psn;
        }
    }

    return disposition;
}

static void build_sack(const struct mrc_rx_qp *qp, int16_t ack_offset,
                       struct mrc_sack *sack)
{
    int32_t base = mrc_psn_delta(qp->lowest_unsacked_psn, qp->cack_psn);
    if (base < 0)
        base = 0;
    int32_t highest = mrc_psn_delta(qp->max_rcv_psn, qp->cack_psn);
    if (base + (int32_t)MRC_SACK_BITS >= highest)
        base = highest > (int32_t)MRC_SACK_BITS ? highest - (int32_t)MRC_SACK_BITS : 0;

    uint64_t bitmap = 0;
    for (unsigned int bit = 0; bit < MRC_SACK_BITS; bit++) {
        uint32_t psn = mrc_psn_add(qp->cack_psn, base + (int32_t)bit);
        if (mrc_rx_classify(qp, psn) == MRC_RX_DUPLICATE)
            bitmap |= UINT64_C(1) << bit;
    }

    *sack = (struct mrc_sack) {
        .cack_psn = qp->cack_psn,
        .ack_psn_offset = ack_offset,
        .sack_offset = (int16_t)base,
        .bitmap = bitmap,
        .ooo_count = qp->ooo_count,
    };
}

bool mrc_rx_build_sack(const struct mrc_rx_qp *qp, uint32_t trigger_psn,
                       struct mrc_sack *sack)
{
    if (mrc_rx_classify(qp, trigger_psn) != MRC_RX_DUPLICATE)
        return false;

    int32_t ack_offset = mrc_psn_delta(trigger_psn, qp->cack_psn);
    if (ack_offset < INT16_MIN || ack_offset > INT16_MAX)
        return false;

    build_sack(qp, (int16_t)ack_offset, sack);
    return true;
}

void mrc_rx_build_probe_sack(const struct mrc_rx_qp *qp, uint16_t probe_id,
                             struct mrc_sack *sack)
{
    int32_t encoded = probe_id;
    if (encoded > INT16_MAX)
        encoded -= UINT16_MAX + 1;
    build_sack(qp, (int16_t)encoded, sack);
}

void mrc_rx_sack_sent(struct mrc_rx_qp *qp, const struct mrc_sack *sack)
{
    qp->lowest_unsacked_psn = mrc_psn_add(
        sack->cack_psn, sack->sack_offset + (int32_t)MRC_SACK_BITS);
    qp->sack_trigger_count = 0;
}

bool mrc_tx_init(struct mrc_tx_qp *qp, uint32_t initial_psn, uint32_t window,
                 struct mrc_tx_packet *slots)
{
    if (!qp || !slots || initial_psn > MRC_PSN_MASK || !valid_window(window))
        return false;

    memset(slots, 0, window * sizeof(*slots));
    *qp = (struct mrc_tx_qp) {
        .slots = slots,
        .window = window,
        .first_psn = initial_psn,
        .delivery_cack_psn = mrc_psn_add(initial_psn, -1),
        .next_psn = initial_psn,
    };
    return true;
}

static uint32_t tx_outstanding(const struct mrc_tx_qp *qp)
{
    return (uint32_t)mrc_psn_delta(qp->next_psn, qp->first_psn);
}

bool mrc_tx_track(struct mrc_tx_qp *qp, uint32_t psn, uint64_t sent_at)
{
    uint32_t outstanding = tx_outstanding(qp);
    if (psn != qp->next_psn || outstanding == qp->window)
        return false;

    uint32_t index = (qp->first_index + outstanding) % qp->window;
    qp->slots[index] = (struct mrc_tx_packet) { .sent_at = sent_at };
    qp->next_psn = mrc_psn_add(qp->next_psn, 1);
    return true;
}

struct mrc_tx_packet *mrc_tx_packet(const struct mrc_tx_qp *qp, uint32_t psn)
{
    int32_t offset = mrc_psn_delta(psn, qp->first_psn);
    if (psn > MRC_PSN_MASK || offset < 0 || (uint32_t)offset >= tx_outstanding(qp))
        return NULL;

    return &qp->slots[(qp->first_index + (uint32_t)offset) % qp->window];
}

static void mark_received(const struct mrc_tx_qp *qp, uint32_t psn)
{
    struct mrc_tx_packet *packet = mrc_tx_packet(qp, psn);
    if (packet)
        packet->received = true;
}

bool mrc_tx_apply_sack(struct mrc_tx_qp *qp, const struct mrc_sack *sack,
                       bool probe_response)
{
    int32_t last = mrc_psn_delta(sack->cack_psn, qp->first_psn);
    if (sack->cack_psn > MRC_PSN_MASK || last >= (int32_t)tx_outstanding(qp))
        return false;

    /* Validate the data trigger before applying any cumulative or selective
     * delivery state. Probe responses carry an opaque token in this field. */
    if (!probe_response &&
        !mrc_tx_packet(qp, mrc_psn_add(sack->cack_psn, sack->ack_psn_offset)))
        return false;

    if (mrc_psn_delta(sack->cack_psn, qp->delivery_cack_psn) > 0) {
        int32_t previous = mrc_psn_delta(qp->delivery_cack_psn, qp->first_psn);
        int32_t start = previous < 0 ? 0 : previous + 1;
        for (int32_t offset = start; offset <= last; offset++)
            mark_received(qp, mrc_psn_add(qp->first_psn, offset));
        qp->delivery_cack_psn = sack->cack_psn;
    }
    if (!probe_response)
        mark_received(qp, mrc_psn_add(sack->cack_psn, sack->ack_psn_offset));
    for (unsigned int bit = 0; bit < MRC_SACK_BITS; bit++) {
        if (sack->bitmap & (UINT64_C(1) << bit))
            mark_received(qp, mrc_psn_add(sack->cack_psn, sack->sack_offset + (int32_t)bit));
    }
    return true;
}

bool mrc_tx_apply_transport_ack(struct mrc_tx_qp *qp, uint32_t psn)
{
    struct mrc_tx_packet *packet = mrc_tx_packet(qp, psn);
    if (!packet)
        return false;

    uint32_t count = (uint32_t)mrc_psn_delta(psn, qp->first_psn) + 1;
    qp->first_index = (qp->first_index + count) % qp->window;
    qp->first_psn = mrc_psn_add(psn, 1);
    if (mrc_psn_delta(psn, qp->delivery_cack_psn) > 0)
        qp->delivery_cack_psn = psn;
    return true;
}

bool mrc_tx_note_retransmit(const struct mrc_tx_qp *qp, uint32_t psn,
                            uint64_t sent_at)
{
    struct mrc_tx_packet *packet = mrc_tx_packet(qp, psn);
    if (!packet)
        return false;

    if (packet->retransmissions < UINT32_MAX)
        packet->retransmissions++;
    packet->sent_at = sent_at;
    return true;
}

bool mrc_local_ack_timeout_ns(unsigned int exponent, unsigned int linear_retries,
                              unsigned int exponential_retries,
                              unsigned int retries_sent, uint64_t *timeout)
{
    if (!timeout || exponent > 31 || linear_retries > 7 || exponential_retries > 25)
        return false;
    if (exponential_retries != 25 && retries_sent > linear_retries + exponential_retries)
        return false;

    if (retries_sent > linear_retries) {
        unsigned int shift = retries_sent - linear_retries;
        exponent = shift >= 24 || exponent >= 24 - shift ? 24 : exponent + shift;
    }
    *timeout = UINT64_C(1024) << exponent;
    return true;
}
