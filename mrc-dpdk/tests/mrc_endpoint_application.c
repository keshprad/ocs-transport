#include "dpdk_io.h"
#include "mrc_endpoint.h"

#include <assert.h>
#include <stdio.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_mbuf.h>
#include <rte_pause.h>

static void activate(struct mrc_provider_qp *qp)
{
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_RESET, MRC_PROVIDER_QP_INIT) == 0);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_INIT, MRC_PROVIDER_QP_RTR) == 0);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_RTR, MRC_PROVIDER_QP_RTS) == 0);
}

static void wait_for_write(struct mrc_requester *requester, struct mrc_provider_cq *cq,
                           const struct mrc_provider_write *write)
{
    struct mrc_provider_wc wc;
    int completions;
    do {
        assert(mrc_requester_progress(requester) == 0);
        completions = mrc_provider_cq_poll(cq, &wc, 1);
        rte_pause();
    } while (!completions);
    assert(completions == 1 && wc.status == MRC_PROVIDER_WC_SUCCESS &&
           wc.opcode == MRC_PROVIDER_WC_WRITE && wc.wr_id == write->wr_id &&
           wc.byte_length == write->length);
}

int main(int argc, char **argv)
{
    assert(rte_eal_init(argc, argv) >= 0);
    const uint64_t local_iova = 0x200000;
    const uint64_t remote_address = 0x100000000;
    const uint32_t remote_key = 0x6d726301;
    uint8_t first_data[13], second_data1[512], second_data2[511], second_data3[2];
    const size_t bytes = sizeof(first_data) + sizeof(second_data1) +
                         sizeof(second_data2) + sizeof(second_data3);
    uint8_t *buffers[] = {first_data, second_data1, second_data2, second_data3};
    const size_t lengths[] = {sizeof(first_data), sizeof(second_data1),
                              sizeof(second_data2), sizeof(second_data3)};

    struct mrc_provider_pd *pd;
    struct mrc_provider_cq *cq;
    struct mrc_provider_qp *qp;
    struct mrc_provider_sge sources[4];
    assert(mrc_provider_pd_create(4, &pd) == 0);
    size_t offset = 0;
    for (size_t i = 0; i < 4; i++) {
        uint64_t iova = local_iova + 0x1000 * i;
        struct mrc_provider_mr_keys keys;
        for (size_t j = 0; j < lengths[i]; j++, offset++)
            buffers[i][j] = (uint8_t)(offset ^ (offset >> 8));
        assert(mrc_provider_mr_register(pd, buffers[i], lengths[i], iova, 0, &keys) == 0);
        sources[i] = (struct mrc_provider_sge){ .iova = iova, .length = lengths[i], .lkey = keys.lkey };
    }
    assert(mrc_provider_cq_create(2, &cq) == 0);
    const struct mrc_provider_qp_config qp_config = {
        .qpn = 0x101, .pd = pd, .send_cq = cq, .send_capacity = 2,
    };
    assert(mrc_provider_qp_create(&qp_config, &qp) == 0);
    activate(qp);
    struct mrc_provider_write first = {
        .wr_id = 0x1234, .local_iova = local_iova, .remote_address = remote_address,
        .length = sizeof(first_data), .lkey = sources[0].lkey, .rkey = remote_key, .signaled = true,
    };
    struct mrc_provider_write second = {
        .wr_id = 0xaabb, .remote_address = remote_address + first.length,
        .length = bytes - first.length, .rkey = remote_key, .immediate = 84, .with_immediate = true,
        .signaled = true,
    };

    const struct mrc_provider_write third = {
        .wr_id = 0x5678, .remote_address = remote_address + bytes,
        .rkey = remote_key, .immediate = 85, .with_immediate = true, .signaled = true,
    };

    /* Reuse the provider QP so the first transfer starts at provider sequence 2. */
    uint64_t sequence;
    struct mrc_provider_wc wc[2];
    assert(mrc_provider_qp_post_write(qp, &first, &sequence) == 0 && sequence == 1);
    assert(mrc_provider_qp_reset(qp) == 0);
    activate(qp);
    /* The application supplies connection and port attributes. The requester
     * reads the WR sizes, source buffers and remote addresses from the SQ. */
    struct mrc_endpoint_config transfer = {
        .sender = true, .continuous_send = true, .pool = mrc_dpdk_packet_pool(),
        .requestor_qpn = 0x101, .responder_qpn = 0x102,
        .ip_version = MRC_WIRE_IP4, .local_ip = 0xc0000201, .peer_ip = 0xc0000202,
        .partition_key = 0xffff, .peer_partition_key = 0xffff, .udp_port = 4971,
        .window = 128, .mpr_max = 1, .max_wimm_inflight = 1, .initial_psn = 0xffffff,
        .timeout_exponent = 16, .linear_retries = 3, .exponential_retries = 4,
        .drop_after = UINT32_MAX, .restore_after = UINT32_MAX, .trim_after = UINT32_MAX,
        .hz = rte_get_timer_hz(),
        .paths = {
            { .port = 0, .peer.addr_bytes = {2, 0, 0, 0, 0, 0x10} },
            { .port = 1, .peer.addr_bytes = {2, 0, 0, 0, 0, 0x11} },
        },
    };
    assert(transfer.pool);
    transfer.initial_rto = mrc_endpoint_cycles(&transfer, 67108864);
    transfer.idle_timeout = mrc_endpoint_cycles(&transfer, 2000000000);
    for (unsigned int rail = 0; rail < PATH_COUNT; rail++)
        assert(mrc_dpdk_open_port((uint16_t)rail, transfer.pool, &transfer.paths[rail].local) == 0);

    struct mrc_requester *requester;
    struct mrc_endpoint_counters counts = {0};
    assert(mrc_requester_create(&transfer, qp, &counts, &requester) == 0);
    assert(mrc_requester_progress(requester) == 0);
    assert(mrc_provider_cq_poll(cq, wc, 2) == 0);
    assert(mrc_provider_qp_post_write(qp, &first, &sequence) == 0 && sequence == 2);
    wait_for_write(requester, cq, &first);

    /* The first packet crosses both SGE boundaries. The second packet
     * starts one byte into the final registration. */
    assert(mrc_provider_qp_post_writev(qp, &second, sources + 1, 3, &sequence) == 0 && sequence == 3);
    assert(mrc_requester_progress(requester) == 0);
    assert(mrc_provider_qp_post_write(qp, &third, &sequence) == 0 && sequence == 4);
    wait_for_write(requester, cq, &second);
    wait_for_write(requester, cq, &third);
    assert(mrc_requester_progress(requester) == 0 && mrc_provider_cq_poll(cq, wc, 2) == 0);
    for (size_t i = 0; i < 4; i++)
        assert(mrc_provider_mr_deregister(pd, sources[i].lkey) == 0);

    mrc_requester_destroy(requester);
    for (unsigned int rail = 0; rail < PATH_COUNT; rail++)
        mrc_dpdk_close_port((uint16_t)rail);
    rte_mempool_free(transfer.pool);
    assert(mrc_provider_qp_destroy(qp) == 0);
    assert(mrc_provider_cq_destroy(cq) == 0);
    assert(mrc_provider_pd_destroy(pd) == 0);
    rte_eal_cleanup();
    printf("{\"bytes\":%zu,\"write_completions\":3}\n", bytes);
    return 0;
}
