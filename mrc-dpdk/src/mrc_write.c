#include "dpdk_io.h"
#include "mrc_endpoint.h"
#include "mrc_oob.h"
#include "mrc_qp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_pause.h>

enum {
    BURST = 32,
    DEFAULT_WIMM_INFLIGHT = 8,
    ALL_PATHS = (1 << PATH_COUNT) - 1,
    DEFAULT_REQUESTOR_QPN = 0x101,
    DEFAULT_RESPONDER_QPN = 0x102,
    DEFAULT_PARTITION_KEY = 0xffff,
    DEFAULT_REMOTE_KEY = 0x6d726301,
};

struct benchmark {
    struct mrc_endpoint_config transfer;
    bool port_stats;
    bool remote_key_configured;
    const char *oob_address;
    uint16_t oob_port;
    uint32_t timeout_ms;
    uint32_t posted_receives;
    uint32_t rx_cq_capacity;
    const char *profile_directory;
    struct mrc_ctl_store_ref profile_lease;
    struct mrc_provider_mr_keys buffer_keys;
};

static const uint64_t default_remote_address = UINT64_C(0x100000000);

static uint8_t pattern(uint32_t offset)
{
    return (uint8_t)(offset ^ (offset >> 8) ^ (offset >> 16) ^ (offset >> 24));
}

static int drain_completions(const struct mrc_endpoint_config *transfer, struct mrc_provider_cq *cq,
                             struct mrc_endpoint_counters *counts)
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
            if (wc->opcode != opcode || wc->qpn != qpn ||
                wc->wr_id != *total + counts->work_errors + counts->work_flushed + 1)
                return -EPROTO;
            if (wc->status == MRC_PROVIDER_WC_FLUSHED) {
                counts->work_flushed++;
            } else if (wc->status != MRC_PROVIDER_WC_SUCCESS) {
                if (!counts->work_errors)
                    counts->first_work_error = wc->status;
                counts->work_errors++;
            } else {
                uint32_t index = *total;
                if (!transfer->sender) {
                    index = counts->next_receive_work;
                    while (index < transfer->work_requests && !transfer->work[index].with_immediate)
                        index++;
                    counts->next_receive_work = index + 1;
                }
                if (index >= transfer->work_requests || wc->with_immediate == transfer->sender ||
                    wc->byte_length != transfer->work[index].length ||
                    (wc->with_immediate && wc->immediate != transfer->work[index].immediate))
                    return -EPROTO;
                (*total)++;
            }
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

static int open_provider_qp(const struct mrc_endpoint_config *transfer, struct mrc_provider_cq *cq,
                             struct mrc_provider_qp **qp)
{
    const struct mrc_provider_qp_config config = {
        .pd = transfer->pd, .qpn = transfer->sender ? transfer->requestor_qpn : transfer->responder_qpn,
        .send_cq = transfer->sender ? cq : NULL, .recv_cq = transfer->sender ? NULL : cq,
        .send_capacity = transfer->sender ? transfer->work_requests : 0,
        .recv_capacity = transfer->sender ? 0 : transfer->work_requests, .sq_sig_all = true,
    };
    int result = mrc_provider_qp_create(&config, qp);
    if (!result)
        result = mrc_provider_qp_transition(*qp, MRC_PROVIDER_QP_RESET, MRC_PROVIDER_QP_INIT);
    if (!result)
        result = mrc_provider_qp_transition(*qp, MRC_PROVIDER_QP_INIT, MRC_PROVIDER_QP_RTR);
    if (!result && transfer->sender)
        result = mrc_provider_qp_transition(*qp, MRC_PROVIDER_QP_RTR, MRC_PROVIDER_QP_RTS);
    return result;
}

static int close_provider_qp(const struct mrc_endpoint_config *transfer, struct mrc_provider_qp *qp,
                              struct mrc_provider_cq *cq, struct mrc_endpoint_counters *counts, int result)
{
    if (!qp)
        return result;
    struct mrc_provider_qp_attr attr;
    mrc_provider_qp_query(qp, &attr);
    if (attr.state != MRC_PROVIDER_QP_RESET &&
        (result || attr.send_pending || attr.recv_pending)) {
        int flushed = 0;
        if (attr.state != MRC_PROVIDER_QP_ERROR)
            flushed = mrc_provider_qp_fail(qp, 0, MRC_PROVIDER_WC_FLUSHED);
        int drained = drain_completions(transfer, cq, counts);
        if (!result)
            result = flushed ? flushed : drained;
        mrc_provider_qp_query(qp, &attr);
        counts->qp_error = result && attr.state == MRC_PROVIDER_QP_ERROR;
    }
    int closed = mrc_provider_qp_destroy(qp);
    return result ? result : closed;
}

static int run_sender(const struct benchmark *benchmark, struct mrc_endpoint_counters *counts)
{
    const struct mrc_endpoint_config *transfer = &benchmark->transfer;
    struct mrc_provider_cq *cq = NULL;
    struct mrc_provider_qp *qp = NULL;
    int result = mrc_provider_cq_create(transfer->work_requests, &cq);
    if (!result)
        result = open_provider_qp(transfer, cq, &qp);
    for (uint32_t wr = 0; !result && wr < transfer->work_requests; wr++) {
        const struct mrc_endpoint_write *work = &transfer->work[wr];
        const struct mrc_provider_write write = {
            .wr_id = wr + 1,
            .local_buffer = transfer->pd ? NULL : transfer->data + work->offset,
            .local_iova = transfer->pd ? (uint64_t)(uintptr_t)(transfer->data + work->offset) : 0,
            .remote_address = transfer->remote_address + work->offset,
            .length = work->length, .lkey = benchmark->buffer_keys.lkey,
            .rkey = transfer->remote_key, .with_immediate = work->with_immediate,
            .immediate = work->immediate,
        };
        uint64_t sequence;
        result = mrc_provider_qp_post_write(qp, &write, &sequence);
        if (!result && sequence != wr + 1)
            result = -EPROTO;
    }
    struct mrc_requester *requester = NULL;
    if (!result)
        result = mrc_requester_create(transfer, qp, counts, &requester);
    if (!result) {
        do {
            result = mrc_requester_progress(requester);
            int completion = drain_completions(transfer, cq, counts);
            if (result >= 0 && completion)
                result = completion;
            rte_pause();
        } while (!result);
        if (result == 1)
            result = counts->write_completions == transfer->work_requests ? 0 : -EPROTO;
    }
    mrc_requester_destroy(requester);
    result = close_provider_qp(transfer, qp, cq, counts, result);
    int closed = destroy_cq(cq);
    return result ? result : closed;
}


static int run_receiver(const struct benchmark *benchmark, struct mrc_endpoint_counters *counts)
{
    const struct mrc_endpoint_config *transfer = &benchmark->transfer;
    struct mrc_provider_cq *cq = NULL;
    struct mrc_provider_qp *qp = NULL;
    struct mrc_responder *receiver = NULL;
    int result = mrc_provider_cq_create(benchmark->rx_cq_capacity, &cq);
    if (!result)
        result = open_provider_qp(transfer, cq, &qp);
    for (uint32_t id = 1; !result && id <= benchmark->posted_receives; id++)
        result = mrc_provider_qp_post_recv(qp, id);
    if (!result)
        result = mrc_responder_create(transfer, qp, counts, &receiver);
    if (!result) {
        do {
            result = mrc_responder_progress(receiver);
            int completion = drain_completions(transfer, cq, counts);
            if (result >= 0 && completion)
                result = completion;
            rte_pause();
        } while (!result);
        if (result == 1) {
            result = 0;
            if (mrc_responder_ack_msn(receiver) != transfer->work_requests ||
                counts->immediate_completions != transfer->immediate_requests)
                result = -EBADMSG;
            for (uint32_t i = 0; !result && i < transfer->bytes; i++) {
                if (transfer->data[i] != pattern(i))
                    result = -EBADMSG;
            }
        }
    }
    if (result == -EPROTO)
        rte_delay_us_sleep(10000); /* Leave time to transmit the terminal NAK. */
    mrc_responder_destroy(receiver);
    result = close_provider_qp(transfer, qp, cq, counts, result);
    int closed = destroy_cq(cq);
    return result ? result : closed;
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

static bool parse_writes(struct mrc_endpoint_config *transfer, const char *input, uint32_t repeat,
                          bool with_immediate, uint32_t immediate)
{
    bool list = strpbrk(input, ",@") != NULL;
    if (!list) {
        uint32_t length;
        if (!number(input, 0, 1U << 30, &length))
            return false;
        if (!repeat)
            repeat = 1;
        transfer->work = calloc(repeat, sizeof(*transfer->work));
        if (!transfer->work)
            return false;
        for (uint32_t wr = 0; wr < repeat; wr++) {
            if (!mrc_endpoint_append_write(transfer, repeat, length, with_immediate, immediate + wr))
                return false;
        }
        transfer->uniform_wr_packets = transfer->work[0].packets;
        return true;
    }

    if (repeat || with_immediate)
        return false;
    uint32_t entries = 1;
    for (const char *cursor = input; *cursor; cursor++) {
        if (*cursor == ',' && ++entries > MAX_WORK_REQUESTS)
            return false;
    }
    transfer->work = calloc(entries, sizeof(*transfer->work));
    char *copy = strdup(input);
    if (!transfer->work || !copy) {
        free(copy);
        return false;
    }
    bool valid = true;
    char *cursor = copy;
    for (char *entry = strsep(&cursor, ","); entry; entry = strsep(&cursor, ",")) {
        char *imm = strchr(entry, '@');
        uint32_t length, value = 0;
        if (imm)
            *imm++ = '\0';
        if (!number(entry, 0, 1U << 30, &length) ||
            (imm && !number(imm, 0, UINT32_MAX, &value)) ||
            !mrc_endpoint_append_write(transfer, entries, length, imm != NULL, value)) {
            valid = false;
            break;
        }
    }
    free(copy);
    return valid;
}

static bool parse_args(int argc, char **argv, struct benchmark *benchmark)
{
    struct mrc_endpoint_config *transfer = &benchmark->transfer;
    if (argc < 9 || (argc - 9) % 2 || (strcmp(argv[1], "tx") && strcmp(argv[1], "rx")))
        return false;
    transfer->sender = strcmp(argv[1], "tx") == 0;
    transfer->max_wimm_inflight = DEFAULT_WIMM_INFLIGHT;
    benchmark->posted_receives = UINT32_MAX;
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
        struct mrc_endpoint_path *path = &transfer->paths[i];
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
    uint32_t timeout_ms = 5000, udp_port = MRC_UDP_DEFAULT_PORT;
    uint32_t trim_dscp = MRC_ENDPOINT_TRIM_DSCP, trim_last_hop_dscp = 0;
    uint32_t oob_port = 0, trim_nack = 1, service_time = 0, srv6_srh = 0, port_stats = 0;
    bool local_sid = false, peer_sid = false, with_immediate = false;
    uint8_t local_sid_value[MRC_IPV6_ADDR_LEN], peer_sid_value[MRC_IPV6_ADDR_LEN];
    uint32_t repeat = 0, immediate = 0;
    uint32_t exponent = 14, linear_retries = 3, exponential_retries = 4;
    uint32_t mpr = WINDOW / MRC_MPR_UNIT, p_key = DEFAULT_PARTITION_KEY;
    uint32_t rx_ooo_capacity = UINT32_MAX, rx_staging_capacity = UINT32_MAX;
    uint32_t rx_work_capacity = 0, rx_cq_capacity = 0;
    uint32_t dynamic_mpr = 0, mpr_change_to = 0;
    for (int i = 9; i < argc; i += 2) {
        const char *key = argv[i], *value = argv[i + 1];
        if (!strcmp(key, "--udp-port") && number(value, 1, UINT16_MAX, &udp_port))
            continue;
        if (!strcmp(key, "--initial-psn") && number(value, 0, MRC_PSN_MASK, &transfer->initial_psn))
            continue;
        if (!strcmp(key, "--oob-host") && value[0]) {
            benchmark->oob_address = value;
            continue;
        }
        if (!strcmp(key, "--oob-port") && number(value, 1, UINT16_MAX, &oob_port))
            continue;
        if (!strcmp(key, "--trim-nack") && number(value, 0, 1, &trim_nack))
            continue;
        if (!strcmp(key, "--service-time") && number(value, 0, 1, &service_time))
            continue;
        if (!strcmp(key, "--srv6-local") && inet_pton(AF_INET6, value, local_sid_value) == 1) {
            local_sid = true;
            continue;
        }
        if (!strcmp(key, "--srv6-peer") && inet_pton(AF_INET6, value, peer_sid_value) == 1) {
            peer_sid = true;
            continue;
        }
        if (!strcmp(key, "--srv6-srh") && number(value, 0, 1, &srv6_srh))
            continue;
        if (!strcmp(key, "--trim-dscp") && number(value, 1, 63, &trim_dscp) &&
            trim_dscp != MRC_ENDPOINT_CONTROL_DSCP)
            continue;
        if (!strcmp(key, "--trim-last-hop-dscp") && number(value, 0, 63, &trim_last_hop_dscp) &&
            trim_last_hop_dscp != MRC_ENDPOINT_CONTROL_DSCP)
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
            benchmark->remote_key_configured = true;
            continue;
        }
        if (!strcmp(key, "--remote-address") &&
            number64(value, 0, UINT64_MAX, &transfer->remote_address))
            continue;
        if (!strcmp(key, "--work-requests") &&
            number(value, 1, MAX_WORK_REQUESTS, &repeat))
            continue;
        if (!strcmp(key, "--max-wimm-inflight") &&
            number(value, 1, WINDOW, &transfer->max_wimm_inflight))
            continue;
        if (!transfer->sender && !strcmp(key, "--rx-ooo-capacity") &&
            number(value, 0, WINDOW, &rx_ooo_capacity))
            continue;
        if (!transfer->sender && !strcmp(key, "--rx-staging-capacity") &&
            number(value, 0, WINDOW, &rx_staging_capacity))
            continue;
        if (!transfer->sender && !strcmp(key, "--rx-work-capacity") &&
            number(value, 1, WINDOW, &rx_work_capacity))
            continue;
        if (!transfer->sender && !strcmp(key, "--rx-cq-capacity") &&
            number(value, 1, MAX_WORK_REQUESTS, &rx_cq_capacity))
            continue;
        if (!transfer->sender && !strcmp(key, "--posted-receives") &&
            number(value, 0, MAX_WORK_REQUESTS, &benchmark->posted_receives))
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
        if (!strcmp(key, "--immediate") && number(value, 0, UINT32_MAX, &immediate)) {
            with_immediate = true;
            continue;
        }
        if (transfer->sender && !strcmp(key, "--profile-dir") && value[0]) {
            benchmark->profile_directory = value;
            continue;
        }
        if (transfer->sender && !strcmp(key, "--profile-id") &&
            number64(value, 1, UINT64_MAX, &transfer->profile_id))
            continue;
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

    if (!parse_writes(transfer, argv[2], repeat, with_immediate, immediate))
        return false;
    if (transfer->remote_address > UINT64_MAX - transfer->bytes ||
        trim_last_hop_dscp == trim_dscp ||
        (transfer->drop_after != UINT32_MAX && transfer->drop_after >= transfer->packets) ||
        (transfer->restore_after != UINT32_MAX &&
         (transfer->restore_after <= transfer->drop_after || transfer->restore_after >= transfer->packets)) ||
        (dynamic_mpr && !oob_port) ||
        ((transfer->mpr_change_at != UINT32_MAX) != (mpr_change_to != 0)) ||
        (mpr_change_to && (!dynamic_mpr || mpr_change_to > mpr ||
                           transfer->mpr_change_at >= transfer->packets)) ||
        (transfer->trim_after != UINT32_MAX && transfer->trim_after >= transfer->packets) ||
        (transfer->drop_after != UINT32_MAX && transfer->trim_after != UINT32_MAX) ||
        (!!benchmark->profile_directory != !!transfer->profile_id) ||
        (!!benchmark->oob_address != !!oob_port) || (oob_port && benchmark->remote_key_configured) ||
        local_sid != peer_sid ||
        (srv6_srh && !local_sid))
        return false;
    if (benchmark->posted_receives == UINT32_MAX)
        benchmark->posted_receives = transfer->immediate_requests;
    if (benchmark->posted_receives > transfer->work_requests)
        return false;
    transfer->udp_port = (uint16_t)udp_port;
    transfer->window = mpr * MRC_MPR_UNIT;
    if ((rx_ooo_capacity != UINT32_MAX && rx_ooo_capacity > transfer->window) ||
        (rx_staging_capacity != UINT32_MAX && rx_staging_capacity > transfer->window) ||
        rx_work_capacity > transfer->window || rx_cq_capacity > transfer->work_requests)
        return false;
    transfer->rx_ooo_capacity = rx_ooo_capacity == UINT32_MAX ? transfer->window : rx_ooo_capacity;
    transfer->rx_staging_capacity = rx_staging_capacity == UINT32_MAX ? transfer->window : rx_staging_capacity;
    transfer->rx_work_capacity = rx_work_capacity ? rx_work_capacity : transfer->window;
    benchmark->rx_cq_capacity = rx_cq_capacity ? rx_cq_capacity : transfer->work_requests;
    if (!transfer->sender && transfer->max_wimm_inflight > transfer->rx_work_capacity)
        transfer->max_wimm_inflight = transfer->rx_work_capacity;
    transfer->mpr_max = (uint8_t)mpr;
    transfer->mpr_change_to = (uint8_t)mpr_change_to;
    transfer->dynamic_mpr = dynamic_mpr != 0;
    transfer->partition_key = (uint16_t)p_key;
    transfer->peer_partition_key = (uint16_t)p_key;
    benchmark->timeout_ms = timeout_ms;
    benchmark->oob_port = (uint16_t)oob_port;
    transfer->trim_nack_enabled = trim_nack != 0;
    transfer->report_service_time = transfer->peer_service_time = service_time != 0;
    benchmark->port_stats = port_stats != 0;
    transfer->srv6 = local_sid;
    if (local_sid) {
        for (unsigned int i = 0; i < PATH_COUNT; i++) {
            struct mrc_endpoint_srv6_path *path = &transfer->paths[i].srv6;
            memcpy(path->local_sid, local_sid_value, MRC_IPV6_ADDR_LEN);
            memcpy(path->peer_sid, peer_sid_value, MRC_IPV6_ADDR_LEN);
            memcpy(path->initial_dst, peer_sid_value, MRC_IPV6_ADDR_LEN);
            if (srv6_srh) {
                memcpy(path->segments[0], peer_sid_value, MRC_IPV6_ADDR_LEN);
                path->segment_count = 1;
            }
        }
    }
    transfer->trim_dscp = (uint8_t)trim_dscp;
    transfer->trim_last_hop_dscp = (uint8_t)trim_last_hop_dscp;
    transfer->timeout_exponent = exponent;
    transfer->linear_retries = linear_retries;
    transfer->exponential_retries = exponential_retries;
    transfer->hz = rte_get_timer_hz();
    uint64_t first_timeout;
    if (!mrc_local_ack_timeout_ns(exponent, linear_retries, exponential_retries, 0, &first_timeout))
        return false;
    transfer->initial_rto = mrc_endpoint_cycles(transfer, first_timeout);
    transfer->idle_timeout = mrc_endpoint_cycles(transfer, (uint64_t)timeout_ms * 1000000);
    return transfer->idle_timeout > transfer->initial_rto;
}

static int open_ev_profile(struct benchmark *benchmark)
{
    struct mrc_endpoint_config *transfer = &benchmark->transfer;
    if (!transfer->profile_id)
        return 0;
    int result = mrc_ctl_store_open(&transfer->profile_store, benchmark->profile_directory);
    if (result)
        return result;
    result = mrc_ctl_store_acquire(&transfer->profile_store, transfer->profile_id,
                                   &benchmark->profile_lease, &transfer->profile);
    if (result)
        return result;

    return mrc_endpoint_apply_ev_profile(transfer) ? 0 : -EINVAL;
}

static int register_buffer(struct benchmark *benchmark)
{
    struct mrc_endpoint_config *transfer = &benchmark->transfer;
    if (!benchmark->oob_address)
        return 0;

    int result = mrc_provider_pd_create(1, &transfer->pd);
    if (result)
        return result;
    uint64_t iova = transfer->sender ? (uint64_t)(uintptr_t)transfer->data : transfer->remote_address;
    size_t allocation = transfer->bytes ? transfer->bytes : 1;
    unsigned int flags = transfer->sender ? 0 :
        MRC_PROVIDER_MR_LOCAL_WRITE | MRC_PROVIDER_MR_REMOTE_WRITE;
    result = mrc_provider_mr_register(transfer->pd, transfer->data, allocation, iova,
                                      flags, &benchmark->buffer_keys);
    if (result)
        return result;
    if (!transfer->sender)
        transfer->remote_key = benchmark->buffer_keys.rkey;
    return 0;
}

static int deregister_buffer(struct benchmark *benchmark)
{
    struct mrc_endpoint_config *transfer = &benchmark->transfer;
    if (!transfer->pd)
        return 0;
    if (benchmark->buffer_keys.lkey) {
        int result = mrc_provider_mr_deregister(transfer->pd, benchmark->buffer_keys.lkey);
        if (result)
            return result;
    }
    return mrc_provider_pd_destroy(transfer->pd);
}

static int setup_qp(struct benchmark *benchmark)
{
    struct mrc_endpoint_config *transfer = &benchmark->transfer;
    if (!benchmark->oob_address)
        return 0;

    struct mrc_connect_offer local = {
        .receive = {
            .qpn = transfer->sender ? transfer->requestor_qpn : transfer->responder_qpn,
            .initial_psn = transfer->initial_psn, .initial_msn = 1, .initial_rqmsn = 1,
            .max_mpr = (uint8_t)(transfer->window / MRC_MPR_UNIT),
            .max_wimm = (uint16_t)transfer->max_wimm_inflight,
        },
        .pmtu = PMTU, .p_key = transfer->partition_key,
        .supported = MRC_CONNECT_TRIM_NACK | MRC_CONNECT_DYNAMIC_MPR | MRC_CONNECT_SERVICE_TIME,
        .offered = (transfer->trim_nack_enabled ? MRC_CONNECT_TRIM_NACK : 0) |
                   (transfer->dynamic_mpr ? MRC_CONNECT_DYNAMIC_MPR : 0) |
                   (transfer->report_service_time ? MRC_CONNECT_SERVICE_TIME : 0),
    };
    if (!transfer->sender) {
        local.receive.memory = (struct mrc_connect_memory){
            .present = true, .address = transfer->remote_address,
            .length = transfer->bytes, .r_key = transfer->remote_key,
        };
    }

    struct mrc_connect_qp qp;
    int result = mrc_oob_exchange(benchmark->oob_address, benchmark->oob_port, !transfer->sender,
                                  benchmark->timeout_ms, &local, &qp);
    if (result)
        return result;
    if (qp.pmtu != PMTU || qp.send.initial_msn != 1 || qp.send.initial_rqmsn != 1)
        return -EPROTONOSUPPORT;

    transfer->peer_partition_key = qp.peer_p_key;
    transfer->dynamic_mpr = qp.dynamic_mpr;
    transfer->trim_nack_enabled = (qp.local_responder_features & MRC_CONNECT_TRIM_NACK) != 0;
    transfer->report_service_time =
        (qp.local_responder_features & qp.peer_supported & MRC_CONNECT_SERVICE_TIME) != 0;
    transfer->peer_service_time = (qp.peer_responder_features & MRC_CONNECT_SERVICE_TIME) != 0;
    if (transfer->sender) {
        transfer->responder_qpn = qp.send.qpn;
        transfer->initial_psn = qp.send.initial_psn;
        transfer->mpr_max = qp.send.max_mpr;
        uint32_t peer_window = qp.send.max_mpr * MRC_MPR_UNIT;
        if (peer_window < transfer->window)
            transfer->window = peer_window;
        if (transfer->immediate_requests && !qp.send.max_wimm)
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

static void print_diagnostics(const struct mrc_endpoint_config *transfer, const struct mrc_endpoint_counters *counts,
                              const struct port_snapshot start[PATH_COUNT], unsigned int opened)
{
    fprintf(stderr, "MRC progress: data TX=%u/%u retry=%u/%u data RX=%u/%u "
                    "SACK=%u transport-ACK=%u probe-reply=%u NACK-TX=%#x NACK-RX=%#x\n",
            counts->sent[0], counts->sent[1], counts->retries[0], counts->retries[1],
            counts->received[0], counts->received[1], counts->sacks,
            counts->transport_acks, counts->probe_replies,
            counts->nack_sent_mask, counts->nack_received_mask);
    if (counts->qp_error) {
        static const char *const statuses[] = {
            [MRC_PROVIDER_WC_SUCCESS] = "none",
            [MRC_PROVIDER_WC_LOCAL_LENGTH] = "local-length",
            [MRC_PROVIDER_WC_LOCAL_PROTECTION] = "local-protection",
            [MRC_PROVIDER_WC_LOCAL_OPERATION] = "local-operation",
            [MRC_PROVIDER_WC_BAD_RESPONSE] = "bad-response",
            [MRC_PROVIDER_WC_REMOTE_INVALID_REQUEST] = "remote-invalid-request",
            [MRC_PROVIDER_WC_REMOTE_ACCESS] = "remote-access",
            [MRC_PROVIDER_WC_REMOTE_OPERATION] = "remote-operation",
            [MRC_PROVIDER_WC_RETRY_EXCEEDED] = "retry-exceeded",
            [MRC_PROVIDER_WC_FLUSHED] = "flushed",
        };
        fprintf(stderr, "MRC QP state=ERROR work-errors=%u first-status=%s work-flushed=%u\n",
                counts->work_errors, statuses[counts->first_work_error], counts->work_flushed);
    }
    for (unsigned int path = 0; path < opened; path++) {
        const struct mrc_endpoint_rx_counters *rx = &counts->rx[path];
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

    struct benchmark benchmark = {
        .transfer.profile_store = MRC_CTL_STORE_INITIALIZER,
        .profile_lease = MRC_CTL_STORE_REF_INITIALIZER,
    };
    struct mrc_endpoint_config *transfer = &benchmark.transfer;
    struct mrc_endpoint_counters counts = {0};
    int result = -EINVAL;
    unsigned int opened = 0;
    struct port_snapshot initial_stats[PATH_COUNT] = {0};
    if (!parse_args(argc - consumed, argv + consumed, &benchmark)) {
        fprintf(stderr, "Usage: %s [EAL options] -- tx|rx BYTES_OR_WRITE_LIST LOCAL_IP PEER_IP\n"
                        "       PORT0 PEER_MAC0 PORT1 PEER_MAC1 [--initial-psn N] [--udp-port N]\n"
                        "       [--requestor-qpn N] [--responder-qpn N] [--p-key N] [--mpr N]\n"
                        "       [--remote-key N] [--remote-address N] [--immediate N]\n"
                        "       [--work-requests N] [--max-wimm-inflight N] [--posted-receives N (rx)]\n"
                        "       [--ack-timeout N] [--linear-retries N]\n"
                        "       [--exp-retries N] [--timeout-ms N] [--drop-path-after N (tx)] [--deny-path-mask N (tx)]\n"
                        "       [--trim-dscp N] [--trim-last-hop-dscp N] [--trim-nack 0|1] [--trim-path-after N (tx)]\n"
                        "       [--drop-final-acks N (rx)] [--oob-host IP --oob-port N]\n"
                        "       [--restore-path-after N (tx)] [--profile-dir DIR --profile-id ID (tx)]\n"
                        "       [--dynamic-mpr 0|1] [--service-time 0|1]\n"
                        "       [--mpr-change-at N --mpr-change-to N (rx)]\n"
                        "       [--rx-ooo-capacity N] [--rx-staging-capacity N] (rx)\n"
                        "       [--rx-work-capacity N] [--rx-cq-capacity N] (rx)\n"
                        "       [--srv6-local SID --srv6-peer SID] [--srv6-srh 0|1] [--port-stats 0|1]\n",
                argv[0]);
        goto done;
    }

    result = open_ev_profile(&benchmark);
    if (result) {
        fprintf(stderr, "MRC EV profile %" PRIu64 " setup failed: %s\n",
                transfer->profile_id, strerror(-result));
        goto done;
    }

    transfer->data = malloc(transfer->bytes ? transfer->bytes : 1);
    transfer->pool = mrc_dpdk_packet_pool();
    if (!transfer->data || !transfer->pool) {
        result = -ENOMEM;
        goto done;
    }
    if (transfer->sender) {
        for (uint32_t i = 0; i < transfer->bytes; i++)
            transfer->data[i] = pattern(i);
    }
    result = register_buffer(&benchmark);
    if (result)
        goto done;
    for (; opened < PATH_COUNT; opened++) {
        struct mrc_endpoint_path *path = &transfer->paths[opened];
        result = mrc_dpdk_open_port(path->port, transfer->pool, &path->local);
        if (result)
            goto done;
        initial_stats[opened].valid = rte_eth_stats_get(path->port, &initial_stats[opened].stats) == 0;
    }
    result = setup_qp(&benchmark);
    if (result) {
        fprintf(stderr, "MRC QP setup failed: %s\n", strerror(-result));
        goto done;
    }
    result = transfer->sender ? run_sender(&benchmark, &counts) : run_receiver(&benchmark, &counts);
    if (!result)
        printf("{\"role\":\"%s\",\"bytes\":%u,\"packets\":%u,\"sent\":[%u,%u],"
               "\"received\":[%u,%u],\"retries\":[%u,%u],\"dropped\":%u,\"sacks\":%u,"
               "\"trimmed\":%u,\"trim_nacks\":%u,\"transport_acks\":%u,"
               "\"nack_sent_mask\":%u,\"nack_received_mask\":%u,"
               "\"port_status_updates\":%u,\"port_status_replies\":%u,"
               "\"probe_requests\":%u,\"probe_replies\":%u,\"probe_sent\":[%u,%u],"
               "\"ev_skipped\":%u,\"ev_failed\":%u,\"ev_recovered\":%u,\"ev_admin_updates\":%u,"
               "\"ev_failed_mask\":%u,\"ev_recovered_mask\":%u,\"mpr_updates\":%u,"
               "\"dropped_transport_acks\":%u,"
               "\"duplicates\":%u,\"fast_retries\":%u,"
               "\"reorder_max\":%u,\"write_completions\":%u,\"immediate_completions\":%u,"
               "\"immediate_completed\":%s,\"registered_memory\":%s}\n",
               transfer->sender ? "tx" : "rx", transfer->bytes, transfer->packets,
               counts.sent[0], counts.sent[1], counts.received[0], counts.received[1],
               counts.retries[0], counts.retries[1], counts.dropped, counts.sacks,
               counts.trimmed, counts.trim_nacks, counts.transport_acks,
               counts.nack_sent_mask, counts.nack_received_mask, counts.port_status_updates, counts.port_status_replies, counts.probe_requests,
               counts.probe_replies, counts.probe_sent[0], counts.probe_sent[1],
               counts.ev_skipped, counts.ev_failed, counts.ev_recovered, counts.ev_admin_updates,
               counts.ev_failed_mask, counts.ev_recovered_mask, counts.mpr_updates,
               counts.dropped_transport_acks,
               counts.duplicates, counts.fast_retries,
               counts.reorder_max, counts.write_completions, counts.immediate_completions,
               counts.immediate_completions ? "true" : "false", transfer->pd ? "true" : "false");

done:
    if (opened && (result || benchmark.port_stats))
        print_diagnostics(transfer, &counts, initial_stats, opened);
    if (result)
        fprintf(stderr, "MRC WRITE failed: %s\n", strerror(-result));
    for (unsigned int i = 0; i < opened; i++)
        mrc_dpdk_close_port(transfer->paths[i].port);
    if (transfer->pool)
        rte_mempool_free(transfer->pool);
    int deregistered = deregister_buffer(&benchmark);
    if (deregistered) {
        fprintf(stderr, "MRC buffer cleanup failed: %s\n", strerror(-deregistered));
        if (!result)
            result = deregistered;
    } else {
        free(transfer->data);
    }
    mrc_ctl_store_release(&benchmark.profile_lease);
    mrc_ctl_store_close(&transfer->profile_store);
    free(transfer->work);
    rte_eal_cleanup();
    return result ? EXIT_FAILURE : EXIT_SUCCESS;
}
