#include "mrc_cc.h"

#include <limits.h>

#define UDP_HEADER_SIZE 8u
#define MAX_NOMINAL_SIZE (UINT16_MAX + MRC_CC_NOMINAL_HDRSIZE)
#define RECVD_HALF_SPACE ((MRC_CC_RECVD_MASK + 1u) / 2u)

uint32_t mrc_cc_nominal_size(uint16_t udp_length)
{
    if (udp_length < UDP_HEADER_SIZE)
        return 0;
    return (uint32_t)udp_length + MRC_CC_NOMINAL_HDRSIZE;
}

void mrc_cc_rx_record(struct mrc_cc_rx *rx, uint32_t nominal_bytes,
                      bool new_data, bool trimmed)
{
    if (new_data && !trimmed)
        rx->received_bytes += nominal_bytes;
}

uint32_t mrc_cc_rx_wire_count(const struct mrc_cc_rx *rx)
{
    const uint32_t bytes = rx->received_bytes;
    const uint32_t extra = bytes % MRC_CC_RECVD_UNIT != 0;
    return (bytes / MRC_CC_RECVD_UNIT + extra) & MRC_CC_RECVD_MASK;
}

bool mrc_cc_init(struct mrc_cc *cc, uint32_t nominal_mtu, uint32_t initial_cwnd,
                 uint64_t initial_base_rtt_ns, uint64_t target_queue_delay_ns)
{
    if (nominal_mtu < UDP_HEADER_SIZE + MRC_CC_NOMINAL_HDRSIZE ||
        nominal_mtu > MAX_NOMINAL_SIZE || initial_cwnd < nominal_mtu ||
        initial_cwnd > INT32_MAX || target_queue_delay_ns == 0)
        return false;

    *cc = (struct mrc_cc){
        .cwnd = initial_cwnd,
        .nominal_mtu = nominal_mtu,
        .base_rtt_ns = initial_base_rtt_ns,
        .target_queue_delay_ns = target_queue_delay_ns,
    };
    return true;
}

bool mrc_cc_can_send(const struct mrc_cc *cc, uint32_t nominal_bytes)
{
    /* An exact packet-size gate is permitted by UE 1.0.1 §3.6.13.5. */
    return nominal_bytes != 0 && nominal_bytes <= cc->nominal_mtu &&
           (int64_t)cc->inflight + nominal_bytes <= cc->cwnd;
}

bool mrc_cc_on_send(struct mrc_cc *cc, uint32_t nominal_bytes)
{
    if (!mrc_cc_can_send(cc, nominal_bytes))
        return false;
    cc->inflight += (int32_t)nominal_bytes;
    return true;
}

bool mrc_cc_ack_request(const struct mrc_cc *cc, uint32_t ack_trigger_bytes)
{
    return (int64_t)cc->cwnd - cc->inflight < cc->nominal_mtu ||
           cc->cwnd < ack_trigger_bytes;
}

static bool retire_bytes(struct mrc_cc *cc, uint32_t nominal_bytes)
{
    const int64_t updated = (int64_t)cc->inflight - nominal_bytes;
    if (updated < INT32_MIN)
        return false;
    cc->inflight = (int32_t)updated;
    return true;
}

static uint64_t measure_rtt(struct mrc_cc *cc, uint64_t arrive_ns,
                            const struct mrc_cc_tx_sample *sample, bool rtx)
{
    if (!sample || sample->retransmissions != (rtx ? 1u : 0u) ||
        arrive_ns <= sample->sent_at_ns)
        return 0;
    const uint64_t rtt = arrive_ns - sample->sent_at_ns;
    if (cc->base_rtt_ns == 0 || rtt < cc->base_rtt_ns)
        cc->base_rtt_ns = rtt;
    return rtt;
}

static enum mrc_cc_adjustment classify_adjustment(bool ecn, uint64_t queue_delay_ns,
                                                  uint64_t target_ns)
{
    if (queue_delay_ns >= target_ns)
        return ecn ? MRC_CC_MULTIPLICATIVE_DECREASE : MRC_CC_FAIR_INCREASE;
    return ecn ? MRC_CC_NO_ADJUSTMENT : MRC_CC_PROPORTIONAL_INCREASE;
}

bool mrc_cc_on_sack(struct mrc_cc *cc, uint32_t rcvd_bytes, bool ecn, bool rtx,
                    uint64_t arrive_ns, const struct mrc_cc_tx_sample *sample,
                    struct mrc_cc_feedback *feedback)
{
    if (rcvd_bytes > MRC_CC_RECVD_MASK)
        return false;

    struct mrc_cc_feedback result = {0};
    const uint32_t delta = (rcvd_bytes - cc->prev_rcvd_bytes) & MRC_CC_RECVD_MASK;
    if (delta && delta < RECVD_HALF_SPACE) {
        const uint32_t newly_received = delta * MRC_CC_RECVD_UNIT;
        if (!retire_bytes(cc, newly_received))
            return false;
        cc->prev_rcvd_bytes = rcvd_bytes;
        result.newly_received_bytes = newly_received;
    }

    result.rtt_ns = measure_rtt(cc, arrive_ns, sample, rtx);
    if (result.rtt_ns) {
        result.rtt_valid = true;
        result.queue_delay_ns = result.rtt_ns - cc->base_rtt_ns;
        result.adjustment = classify_adjustment(ecn, result.queue_delay_ns,
                                                cc->target_queue_delay_ns);
    }
    if (feedback)
        *feedback = result;
    return true;
}

static void reduce_cwnd(struct mrc_cc *cc, uint32_t nominal_bytes)
{
    const uint32_t room = cc->cwnd - cc->nominal_mtu;
    cc->cwnd -= nominal_bytes < room ? nominal_bytes : room;
}

bool mrc_cc_on_nack(struct mrc_cc *cc, uint32_t nominal_bytes, bool trimmed,
                    bool rtx, uint64_t arrive_ns, const struct mrc_cc_tx_sample *sample)
{
    if (nominal_bytes > cc->nominal_mtu || !retire_bytes(cc, nominal_bytes))
        return false;
    measure_rtt(cc, arrive_ns, sample, rtx);
    if (trimmed)
        reduce_cwnd(cc, nominal_bytes);
    return true;
}

bool mrc_cc_on_inferred_loss(struct mrc_cc *cc, uint32_t nominal_bytes)
{
    if (nominal_bytes == 0 || nominal_bytes > cc->nominal_mtu ||
        !retire_bytes(cc, nominal_bytes))
        return false;
    reduce_cwnd(cc, nominal_bytes);
    return true;
}

enum mrc_cc_qp_state mrc_cc_scheduler_state(const struct mrc_cc *cc,
                                           uint32_t next_nominal_bytes,
                                           bool outstanding)
{
    if (next_nominal_bytes)
        return mrc_cc_can_send(cc, next_nominal_bytes) ? MRC_CC_QP_READY : MRC_CC_QP_ACTIVE;
    return outstanding ? MRC_CC_QP_PENDING : MRC_CC_QP_IDLE;
}
