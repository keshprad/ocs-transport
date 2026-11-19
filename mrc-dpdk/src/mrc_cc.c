#include "mrc_cc.h"

#include <limits.h>
#include <math.h>

#define UDP_HEADER_SIZE 8u
#define MAX_NOMINAL_SIZE (UINT16_MAX + MRC_CC_NOMINAL_HDRSIZE)
#define RECVD_HALF_SPACE ((MRC_CC_RECVD_MASK + 1u) / 2u)
#define BASE_BDP_BYTES 150000.0
#define TARGET_DELAY_REFERENCE_NS 12000.0
#define NEAR_BASE_RTT_NS 128u
#define NS_PER_SECOND 1000000000.0
#define WINDOW_LIMIT ((RECVD_HALF_SPACE - 1u) * MRC_CC_RECVD_UNIT)

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

static struct mrc_cc_nscc nscc_defaults(uint32_t mtu, uint32_t maximum,
                                         double scaling_a, uint64_t period)
{
    return (struct mrc_cc_nscc){
        .max_wnd = maximum, .scaling_a = scaling_a,
        .adjust_period_ns = period, .adjust_bytes_threshold = 8u * mtu,
        .eta = 0.15 * mtu * scaling_a, .fi = 5.0 * mtu * scaling_a,
        .fi_scale = 0.25 * scaling_a, .gamma = 0.8, .max_md_jump = 0.5,
    };
}

bool mrc_cc_init(struct mrc_cc *cc, uint32_t nominal_mtu, uint32_t initial_cwnd,
                 uint64_t initial_base_rtt_ns, uint64_t target_queue_delay_ns)
{
    if (nominal_mtu < UDP_HEADER_SIZE + MRC_CC_NOMINAL_HDRSIZE ||
        nominal_mtu > MAX_NOMINAL_SIZE || initial_cwnd < nominal_mtu ||
        initial_cwnd > WINDOW_LIMIT || target_queue_delay_ns == 0)
        return false;

    *cc = (struct mrc_cc){
        .cwnd = initial_cwnd,
        .nominal_mtu = nominal_mtu,
        .base_rtt_ns = initial_base_rtt_ns,
        .target_queue_delay_ns = target_queue_delay_ns,
        .algorithm = MRC_CC_NSCC,
        .nscc = nscc_defaults(nominal_mtu, initial_cwnd,
                              initial_cwnd / (1.5 * BASE_BDP_BYTES),
                              initial_base_rtt_ns ? initial_base_rtt_ns : target_queue_delay_ns),
    };
    return true;
}

bool mrc_cc_config_valid(const struct mrc_cc_config *config, uint32_t nominal_mtu)
{
    return config && config->disable_qa && config->adjust_bytes_threshold &&
           config->adjust_period_threshold && config->base_rtt && config->target_delay &&
           config->max_cwnd >= nominal_mtu && config->max_cwnd <= WINDOW_LIMIT &&
           isfinite(config->eta) && config->eta >= 0 &&
           isfinite(config->fi) && config->fi >= 0 &&
           isfinite(config->fi_scale) && config->fi_scale >= 0 &&
           isfinite(config->gamma) && config->gamma >= 0 && config->gamma <= 1 &&
           isfinite(config->max_md_jump) && config->max_md_jump >= 0 && config->max_md_jump <= 1;
}

bool mrc_cc_apply_config(struct mrc_cc *cc, const struct mrc_cc_config *config)
{
    if (!config) {
        cc->algorithm = MRC_CC_DISABLED;
        cc->cwnd = WINDOW_LIMIT;
        return true;
    }
    if (!mrc_cc_config_valid(config, cc->nominal_mtu))
        return false;
    cc->algorithm = MRC_CC_NSCC;
    cc->base_rtt_ns = config->base_rtt;
    cc->target_queue_delay_ns = config->target_delay;
    cc->cwnd = config->max_cwnd;
    /* Infer the tuning BDP from MaxWnd when no link rates are supplied. */
    cc->nscc = (struct mrc_cc_nscc){
        .max_wnd = config->max_cwnd,
        .scaling_a = config->max_cwnd / (1.5 * BASE_BDP_BYTES),
        .adjust_period_ns = config->adjust_period_threshold,
        .adjust_bytes_threshold = config->adjust_bytes_threshold,
        .eta = config->eta, .fi = config->fi, .fi_scale = config->fi_scale,
        .gamma = config->gamma, .max_md_jump = config->max_md_jump,
    };
    return true;
}

static uint32_t bounded_window(double size, uint32_t minimum, uint32_t maximum)
{
    if (size < minimum)
        return minimum;
    if (size > maximum)
        return maximum;
    return (uint32_t)size;
}

bool mrc_cc_configure_nscc(struct mrc_cc *cc, uint64_t sender_link_bytes_per_second,
                            uint64_t receiver_link_bytes_per_second,
                            uint64_t config_base_rtt_ns, uint64_t now_ns)
{
    if (!sender_link_bytes_per_second || !config_base_rtt_ns)
        return false;

    uint64_t bdp_rate = sender_link_bytes_per_second;
    if (receiver_link_bytes_per_second && receiver_link_bytes_per_second < bdp_rate)
        bdp_rate = receiver_link_bytes_per_second;
    const double bdp = (double)bdp_rate * config_base_rtt_ns / NS_PER_SECOND;
    const uint32_t maximum = bounded_window(1.5 * bdp, cc->nominal_mtu, WINDOW_LIMIT);

    cc->base_rtt_ns = config_base_rtt_ns;
    cc->cwnd = bounded_window(cc->cwnd, cc->nominal_mtu, maximum);
    cc->algorithm = MRC_CC_NSCC;
    cc->nscc = nscc_defaults(cc->nominal_mtu, maximum, bdp / BASE_BDP_BYTES, config_base_rtt_ns);
    cc->nscc.sender_link_bytes_per_second = sender_link_bytes_per_second;
    cc->nscc.last_adjust_ns = now_ns;
    cc->nscc.last_decrease_ns = now_ns;
    cc->nscc.last_ack_ns = now_ns;
    cc->nscc.clock_started = true;
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
    if (cc->base_rtt_ns == 0 || rtt < cc->base_rtt_ns) {
        cc->base_rtt_ns = rtt;
        cc->nscc.current_delay = (struct mrc_cc_delay_window){0};
        cc->nscc.previous_delay = (struct mrc_cc_delay_window){0};

        const uint64_t rate = cc->nscc.sender_link_bytes_per_second;
        if (rate) {
            const double window = 1.5 * (double)rate * rtt / NS_PER_SECOND;
            cc->nscc.max_wnd = bounded_window(window, cc->nominal_mtu, WINDOW_LIMIT);
            cc->cwnd = bounded_window(cc->cwnd, cc->nominal_mtu, cc->nscc.max_wnd);
        }
    }
    return rtt;
}

static enum mrc_cc_adjustment classify_adjustment(bool ecn, uint64_t queue_delay_ns,
                                                  uint64_t target_ns)
{
    if (queue_delay_ns >= target_ns)
        return ecn ? MRC_CC_MULTIPLICATIVE_DECREASE : MRC_CC_FAIR_INCREASE;
    return ecn ? MRC_CC_NO_ADJUSTMENT : MRC_CC_PROPORTIONAL_INCREASE;
}

static double add_delay_sample(struct mrc_cc *cc, uint64_t now_ns, uint64_t queue_delay_ns)
{
    struct mrc_cc_nscc *state = &cc->nscc;
    const uint64_t horizon_ns = cc->base_rtt_ns;
    if (!state->current_delay.samples)
        state->delay_start_ns = now_ns;

    uint64_t elapsed_ns = now_ns - state->delay_start_ns;
    if (elapsed_ns >= horizon_ns) {
        if (elapsed_ns / horizon_ns == 1)
            state->previous_delay = state->current_delay;
        else
            state->previous_delay = (struct mrc_cc_delay_window){0};
        state->current_delay = (struct mrc_cc_delay_window){0};
        elapsed_ns %= horizon_ns;
        state->delay_start_ns = now_ns - elapsed_ns;
    }

    state->current_delay.sum_ns += queue_delay_ns;
    state->current_delay.samples++;

    /* Approximate the overlap of the prior bucket with the last base RTT. */
    const double weight = 1.0 - (double)elapsed_ns / (double)horizon_ns;
    const double total = state->current_delay.sum_ns + weight * state->previous_delay.sum_ns;
    const double samples = state->current_delay.samples + weight * state->previous_delay.samples;
    return total / samples;
}

static uint64_t add_bytes(uint64_t total, uint32_t additional)
{
    return UINT64_MAX - total < additional ? UINT64_MAX : total + additional;
}

static void apply_ack_adjustment(struct mrc_cc *cc, uint64_t now_ns,
                                 const struct mrc_cc_feedback *feedback)
{
    struct mrc_cc_nscc *state = &cc->nscc;
    const uint64_t delay = feedback->queue_delay_ns;
    const uint32_t newly_received = feedback->newly_received_bytes;
    const uint32_t mtu = cc->nominal_mtu;
    if (!state->clock_started) {
        state->clock_started = true;
        state->last_adjust_ns = now_ns;
        state->last_decrease_ns = now_ns;
    }
    state->last_ack_ns = now_ns;
    const double average = add_delay_sample(cc, now_ns, delay);

    switch (feedback->adjustment) {
    case MRC_CC_PROPORTIONAL_INCREASE:
        /* UE 1.0.1 §3.6.13.6 fast increase uses delay approximately zero. */
        if (delay <= NEAR_BASE_RTT_NS) {
            state->fast_bytes = add_bytes(state->fast_bytes, newly_received);
            if (state->fast_bytes > cc->cwnd || state->increase_mode) {
                const double growth = newly_received * state->fi_scale;
                cc->cwnd = bounded_window(cc->cwnd + growth, mtu, state->max_wnd);
                state->increase_mode = true;
                break;
            }
        } else {
            state->fast_bytes = 0;
        }
        state->increase_mode = false;
        /* scaling_b / target_qdelay reduces to 1 / 12 us in §3.6.13.3. */
        state->inc_bytes += 4.0 * state->scaling_a * mtu / TARGET_DELAY_REFERENCE_NS *
                            newly_received * (cc->target_queue_delay_ns - delay);
        break;
    case MRC_CC_FAIR_INCREASE:
        state->inc_bytes += state->fi * newly_received;
        break;
    case MRC_CC_MULTIPLICATIVE_DECREASE:
        state->increase_mode = false;
        state->fast_bytes = 0;
        if (average > (double)cc->target_queue_delay_ns &&
            now_ns - state->last_decrease_ns > cc->base_rtt_ns) {
            const double factor = 1.0 - state->gamma * (average - cc->target_queue_delay_ns) / average;
            const double reduction = factor < state->max_md_jump ? state->max_md_jump : factor;
            cc->cwnd = bounded_window(cc->cwnd * reduction, mtu, state->max_wnd);
            state->last_decrease_ns = now_ns;
        }
        break;
    case MRC_CC_NO_ADJUSTMENT:
        break;
    }

    const bool period_elapsed = now_ns - state->last_adjust_ns >= state->adjust_period_ns;
    if (period_elapsed || state->received_bytes > state->adjust_bytes_threshold) {
        double increase = state->inc_bytes / cc->cwnd;
        if (period_elapsed) {
            state->last_adjust_ns = now_ns;
            increase += state->eta;
        }
        cc->cwnd = bounded_window(cc->cwnd + increase, mtu, state->max_wnd);
        state->inc_bytes = 0;
        state->received_bytes = 0;
    }
}

bool mrc_cc_on_sack(struct mrc_cc *cc, uint32_t rcvd_bytes, bool ecn, bool rtx,
                    uint64_t arrive_ns, const struct mrc_cc_tx_sample *sample,
                    struct mrc_cc_feedback *feedback)
{
    if (rcvd_bytes > MRC_CC_RECVD_MASK)
        return false;

    struct mrc_cc_feedback result = {0};
    const uint32_t delta = (rcvd_bytes - cc->prev_rcvd_bytes) & MRC_CC_RECVD_MASK;
    if (delta >= RECVD_HALF_SPACE) {
        if (feedback)
            *feedback = result;
        return true;
    }
    if (delta) {
        const uint32_t newly_received = delta * MRC_CC_RECVD_UNIT;
        if (!retire_bytes(cc, newly_received))
            return false;
        cc->prev_rcvd_bytes = rcvd_bytes;
        result.newly_received_bytes = newly_received;
    }

    if (cc->algorithm == MRC_CC_NSCC) {
        cc->nscc.received_bytes = add_bytes(cc->nscc.received_bytes, result.newly_received_bytes);
        if (!cc->nscc.clock_started || arrive_ns >= cc->nscc.last_ack_ns)
            result.rtt_ns = measure_rtt(cc, arrive_ns, sample, rtx);
        if (result.rtt_ns) {
            result.rtt_valid = true;
            result.queue_delay_ns = result.rtt_ns - cc->base_rtt_ns;
            result.adjustment = classify_adjustment(ecn, result.queue_delay_ns,
                                                    cc->target_queue_delay_ns);
            apply_ack_adjustment(cc, arrive_ns, &result);
        }
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
    if (cc->algorithm == MRC_CC_NSCC) {
        measure_rtt(cc, arrive_ns, sample, rtx);
        if (trimmed)
            reduce_cwnd(cc, nominal_bytes);
    }
    return true;
}

bool mrc_cc_on_inferred_loss(struct mrc_cc *cc, uint32_t nominal_bytes)
{
    if (nominal_bytes == 0 || nominal_bytes > cc->nominal_mtu ||
        !retire_bytes(cc, nominal_bytes))
        return false;
    if (cc->algorithm == MRC_CC_NSCC)
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
