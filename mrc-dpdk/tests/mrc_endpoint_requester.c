/* Drive the requester with a deterministic clock and no live ethdevs. */
#include <rte_cycles.h>
static uint64_t clock_cycles;
#define rte_get_timer_cycles() clock_cycles
#include "../src/mrc_endpoint.c"
#undef rte_get_timer_cycles
#include <assert.h>

static uint16_t empty_receive(void *context, unsigned int path, struct mrc_endpoint_rx_packet *packets,
                              uint16_t capacity)
{
    (void)context;
    (void)path;
    (void)packets;
    (void)capacity;
    return 0;
}

static void test_first_send_timeout(unsigned int retries)
{
    struct mrc_provider_cq *cq;
    struct mrc_provider_qp *qp;
    assert(mrc_provider_cq_create(1, &cq) == 0);
    const struct mrc_provider_qp_config provider = {
        .send_cq = cq, .send_capacity = 1, .qpn = 3,
    };
    assert(mrc_provider_qp_create(&provider, &qp) == 0);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_RESET, MRC_PROVIDER_QP_INIT) == 0);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_INIT, MRC_PROVIDER_QP_RTR) == 0);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_RTR, MRC_PROVIDER_QP_RTS) == 0);
    const uint8_t data = 0;
    const struct mrc_provider_write write = {
        .wr_id = 42, .local_buffer = &data, .length = sizeof(data), .signaled = true,
    };
    uint64_t sequence;
    assert(mrc_provider_qp_post_write(qp, &write, &sequence) == 0);

    struct rte_mempool unused_pool;
    struct mrc_endpoint_node node = { .local_down = 1, .next_poll = UINT64_MAX };
    const struct mrc_endpoint_config config = {
        .pool = &unused_pool, .node = &node, .receive = empty_receive, .path_count = 1,
        .requestor_qpn = provider.qpn, .responder_qpn = 4, .initial_psn = 100,
        .pmtu = 1024, .window = 128, .mpr_max = 1, .continuous_send = true,
        .hz = 1000000000, .initial_rto = 16384, .idle_timeout = UINT64_MAX,
        .timeout_exponent = 4, .exponential_retries = retries,
    };
    struct mrc_endpoint_counters counts = {0};
    struct mrc_requester *sender;
    clock_cycles = 1000;
    assert(mrc_requester_create(&config, qp, &counts, &sender) == 0);
    assert(mrc_requester_progress(sender) == 0);
    clock_cycles += config.initial_rto - 1;
    assert(mrc_requester_progress(sender) == 0);
    clock_cycles++;
    if (retries) {
        assert(mrc_requester_progress(sender) == 0);
        clock_cycles += 2 * config.initial_rto - 1;
        assert(mrc_requester_progress(sender) == 0);
        clock_cycles++;
    }
    struct mrc_provider_wc wc;
    assert(mrc_provider_cq_poll(cq, &wc, 1) == 0);
    assert(mrc_requester_progress(sender) == -ETIMEDOUT);
    assert(mrc_provider_cq_poll(cq, &wc, 1) == 1);
    assert(wc.wr_id == 42 && wc.status == MRC_PROVIDER_WC_RETRY_EXCEEDED);
    struct mrc_provider_qp_attr attr;
    assert(mrc_provider_qp_query(qp, &attr) == 0 && attr.state == MRC_PROVIDER_QP_ERROR);
    assert(sender->qp.next_psn == config.initial_psn && !counts.sent[0] && !counts.retries[0]);
    assert(mrc_provider_cq_poll(cq, &wc, 1) == 0);
    mrc_requester_destroy(sender);
    assert(mrc_provider_qp_destroy(qp) == 0);
    assert(mrc_provider_cq_destroy(cq) == 0);
}

static void test_sack_after_transport_ack(void)
{
    const uint32_t psn = MRC_PSN_MASK;
    struct mrc_endpoint_write work = { .packets = 2, .length = 2048 };
    struct mrc_endpoint_config config = {
        .work = &work, .work_requests = 1, .packets = 2, .window = 128, .initial_psn = psn,
        .hz = 1000000000, .pmtu = 1024, .requestor_qpn = 3, .responder_qpn = 4,
    };
    struct mrc_endpoint_counters counts = {0};
    struct mrc_tx_packet slots[128];
    struct send_record records[128] = {0};
    struct mrc_requester sender = { .next = 1, .metadata = records };
    const uint32_t bytes = nominal_at(&config, 0);
    clock_cycles = 1000;
    assert(mrc_tx_init(&sender.qp, psn, config.window, slots));
    assert(mrc_cc_init(&sender.cc, 1200, 12000, 0, 12000));
    assert(mrc_cc_on_send(&sender.cc, bytes));
    assert(mrc_tx_track(&sender.qp, psn, clock_cycles));
    assert(mrc_mpr_init(&sender.mpr, false, 1));

    const struct mrc_wire_packet ack = {
        .bth = { .opcode = MRC_TRANSPORT_ACK, .psn = psn },
        .body.aeth = { .syndrome = ACK_SYNDROME, .msn = 0 },
    };
    assert(apply_control(&config, &sender, &counts, &ack, clock_cycles) == 0);
    assert(counts.transport_acks == 1 && mrc_tx_packet(&sender.qp, psn) == NULL);
    assert(sender.cc.inflight == (int32_t)bytes);

    const uint32_t credited_units = (bytes + 255) / 256;
    struct mrc_wire_packet sack = {
        .bth = { .opcode = MRC_RELIABILITY_SACK, .psn = psn },
        .body.sack = { .spdcid = 3, .dpdcid = 4, .cack_psn = psn, .rcvd_bytes = credited_units },
    };
    assert(apply_control(&config, &sender, &counts, &sack, clock_cycles) == 0);
    assert(counts.sacks == 1 && sender.cc.prev_rcvd_bytes == credited_units);
    const int32_t inflight = (int32_t)bytes - (int32_t)(credited_units * 256);
    assert(sender.cc.inflight == inflight);
    assert(apply_control(&config, &sender, &counts, &sack, clock_cycles) == 0);
    assert(counts.sacks == 2 && sender.cc.inflight == inflight);

    /* Neither the PSN before the first send nor the next PSN across wrap was sent. */
    sack.body.sack.rcvd_bytes++;
    sack.body.sack.ack_psn_offset = -1;
    assert(apply_control(&config, &sender, &counts, &sack, clock_cycles) == 0);
    sack.body.sack.ack_psn_offset = 1;
    assert(apply_control(&config, &sender, &counts, &sack, clock_cycles) == 0);
    assert(counts.sacks == 2 && sender.cc.prev_rcvd_bytes == credited_units && sender.cc.inflight == inflight);
}

int main(void)
{
    test_first_send_timeout(0);
    test_first_send_timeout(1);
    test_sack_after_transport_ack();
    puts("Requester first-send timeout and reordered Reliability SACK checks passed");
    return 0;
}
