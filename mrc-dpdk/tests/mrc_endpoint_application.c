#include "dpdk_io.h"
#include "mrc_endpoint.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_pause.h>

static bool fail_rail_two_link_query;

int __real_rte_eth_link_get_nowait(uint16_t port, struct rte_eth_link *link);

int __wrap_rte_eth_link_get_nowait(uint16_t port, struct rte_eth_link *link)
{
    if (port == 1 && fail_rail_two_link_query) {
        fail_rail_two_link_query = false;
        return -EIO;
    }
    return __real_rte_eth_link_get_nowait(port, link);
}

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

static struct mrc_endpoint_config requester_config(void)
{
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
    return transfer;
}

static void close_ports(struct rte_mempool *pool)
{
    for (unsigned int rail = 0; rail < PATH_COUNT; rail++)
        mrc_dpdk_close_port((uint16_t)rail);
    rte_mempool_free(pool);
}

static void check_requester_cancel(const struct mrc_endpoint_config *transfer)
{
    struct mrc_provider_pd *pd;
    struct mrc_provider_cq *cq;
    struct mrc_provider_qp *qp;
    uint8_t data[13] = {0};
    struct mrc_provider_mr_keys keys;
    assert(mrc_provider_pd_create(1, &pd) == 0);
    assert(mrc_provider_cq_create(1, &cq) == 0);
    assert(mrc_provider_mr_register(pd, data, sizeof(data), 0x9000, 0, &keys) == 0);
    const struct mrc_provider_qp_config config = {.qpn = 0x101, .pd = pd, .send_cq = cq, .send_capacity = 1};
    assert(mrc_provider_qp_create(&config, &qp) == 0);
    activate(qp);
    const struct mrc_provider_write write = {.wr_id = 0xcafe, .local_iova = 0x9000,
        .lkey = keys.lkey, .length = sizeof(data), .remote_address = 0x100000000, .rkey = 1};
    uint64_t sequence;
    assert(mrc_provider_qp_post_write(qp, &write, &sequence) == 0);
    struct mrc_requester *requester;
    struct mrc_endpoint_counters counters = {0};
    struct mrc_endpoint_config invalid = *transfer;
    invalid.mpr_max = 0;
    assert(mrc_requester_create(&invalid, qp, &counters, &requester) == -EINVAL);
    struct mrc_provider_qp_attr attr;
    assert(mrc_provider_qp_query(qp, &attr) == 0 && attr.state == MRC_PROVIDER_QP_RTS && attr.send_pending == 1);
    assert(mrc_provider_mr_deregister(pd, keys.lkey) == -EBUSY);
    assert(mrc_requester_create(transfer, qp, &counters, &requester) == 0);
    mrc_requester_destroy(requester);
    assert(mrc_provider_qp_query(qp, &attr) == 0 && attr.state == MRC_PROVIDER_QP_ERROR && !attr.send_pending);
    struct mrc_provider_wc wc;
    assert(mrc_provider_cq_poll(cq, &wc, 1) == 1 && wc.wr_id == write.wr_id && wc.status == MRC_PROVIDER_WC_FLUSHED);
    assert(mrc_provider_cq_poll(cq, &wc, 1) == 0);
    assert(mrc_provider_mr_deregister(pd, keys.lkey) == 0);
    assert(mrc_provider_qp_destroy(qp) == 0);
    assert(mrc_provider_cq_destroy(cq) == 0);
    assert(mrc_provider_pd_destroy(pd) == 0);
}

static void check_responder_cancel(const struct mrc_endpoint_config *source)
{
    struct mrc_provider_cq *cq;
    struct mrc_provider_qp *qp;
    assert(mrc_provider_cq_create(1, &cq) == 0);
    const struct mrc_provider_qp_config config = {.qpn = 0x102, .recv_cq = cq, .recv_capacity = 1};
    assert(mrc_provider_qp_create(&config, &qp) == 0);
    activate(qp);
    assert(mrc_provider_qp_post_recv(qp, 0xbeef) == 0);
    uint8_t data[13] = {0};
    struct mrc_endpoint_config transfer = *source;
    transfer.sender = false;
    transfer.data = data;
    transfer.bytes = sizeof(data);
    transfer.remote_address = 0x100000000;
    transfer.remote_key = 1;
    transfer.rx_work_capacity = 1;
    struct mrc_endpoint_counters counters = {0};
    struct mrc_responder *responder;
    transfer.mpr_max = 0;
    assert(mrc_responder_create(&transfer, qp, &counters, &responder) == -EINVAL);
    struct mrc_provider_qp_attr attr;
    assert(mrc_provider_qp_query(qp, &attr) == 0 && attr.state == MRC_PROVIDER_QP_RTS && attr.recv_pending == 1);
    transfer.mpr_max = 1;
    assert(mrc_responder_create(&transfer, qp, &counters, &responder) == 0);
    mrc_responder_destroy(responder);
    assert(mrc_provider_qp_query(qp, &attr) == 0 && attr.state == MRC_PROVIDER_QP_ERROR && !attr.recv_pending);
    struct mrc_provider_wc wc;
    assert(mrc_provider_cq_poll(cq, &wc, 1) == 1 && wc.wr_id == 0xbeef && wc.status == MRC_PROVIDER_WC_FLUSHED);
    assert(mrc_provider_cq_poll(cq, &wc, 1) == 0);
    assert(mrc_provider_qp_destroy(qp) == 0);
    assert(mrc_provider_cq_destroy(cq) == 0);
}

static void run_application(void)
{
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
    struct mrc_endpoint_config transfer = requester_config();
    check_requester_cancel(&transfer);
    check_responder_cancel(&transfer);

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
    close_ports(transfer.pool);
    assert(mrc_provider_qp_destroy(qp) == 0);
    assert(mrc_provider_cq_destroy(cq) == 0);
    assert(mrc_provider_pd_destroy(pd) == 0);
    printf("{\"bytes\":%zu,\"write_completions\":3}\n", bytes);
}

static void wait_for_ports(struct mrc_requester *requester, const struct mrc_endpoint_node *node,
                           uint32_t expected)
{
    uint64_t deadline = rte_get_timer_cycles() + 2 * rte_get_timer_hz();
    do {
        assert(rte_get_timer_cycles() < deadline);
        assert(mrc_requester_progress(requester) == 0);
        rte_pause();
    } while (node->local_ports != expected || node->peer_ports != expected ||
             rte_get_timer_cycles() >= node->peer_valid_until);
    assert(node->local_down == (3u & ~expected));
}

static void check_link_query_error(struct mrc_requester *requester, const struct mrc_endpoint_node *node)
{
    fail_rail_two_link_query = true;
    uint64_t deadline = rte_get_timer_cycles() + rte_get_timer_hz();
    while (fail_rail_two_link_query) {
        assert(rte_get_timer_cycles() < deadline);
        assert(mrc_requester_progress(requester) == 0);
        rte_pause();
    }
    assert(node->local_ports == 1 && node->local_down == 2);
}

static void run_link_transition(void)
{
    enum { WRITE_BYTES = 4096, ROUNDS = 3 };
    const uint64_t local_iova = 0x200000;
    const uint64_t remote_address = 0x100000000;
    const uint32_t remote_key = 0x6d726301;
    uint8_t data[WRITE_BYTES];
    struct mrc_provider_mr_keys keys;
    struct mrc_provider_pd *pd;
    struct mrc_provider_cq *cq;
    struct mrc_provider_qp *qp;
    assert(mrc_provider_pd_create(1, &pd) == 0);
    assert(mrc_provider_mr_register(pd, data, sizeof(data), local_iova, 0, &keys) == 0);
    assert(mrc_provider_cq_create(1, &cq) == 0);
    const struct mrc_provider_qp_config qp_config = {
        .qpn = 0x101, .pd = pd, .send_cq = cq, .send_capacity = 1,
    };
    assert(mrc_provider_qp_create(&qp_config, &qp) == 0);
    activate(qp);

    struct mrc_endpoint_config transfer = requester_config();
    struct mrc_endpoint_node node = {0};
    transfer.node = &node;
    struct mrc_endpoint_counters counts = {0};
    struct mrc_requester *requester;
    assert(mrc_requester_create(&transfer, qp, &counts, &requester) == 0);

    uint32_t rail_packets[ROUNDS][PATH_COUNT];
    for (unsigned int round = 0; round < ROUNDS; round++) {
        unsigned int ports = round == 1 ? 1 : 3;
        if (round == 1)
            assert(rte_eth_dev_stop(transfer.paths[1].port) == 0);
        if (round == 2)
            assert(rte_eth_dev_start(transfer.paths[1].port) == 0);
        wait_for_ports(requester, &node, ports);
        if (round == 1)
            check_link_query_error(requester, &node);

        for (unsigned int i = 0; i < sizeof(data); i++) {
            unsigned int offset = round * WRITE_BYTES + i;
            data[i] = (uint8_t)(offset ^ (offset >> 8));
        }
        const struct mrc_provider_write write = {
            .wr_id = round + 1, .local_iova = local_iova, .lkey = keys.lkey,
            .remote_address = remote_address + round * WRITE_BYTES, .rkey = remote_key,
            .length = sizeof(data), .signaled = true,
        };
        const uint32_t before[PATH_COUNT] = { counts.sent[0], counts.sent[1] };
        uint64_t sequence;
        assert(mrc_provider_qp_post_write(qp, &write, &sequence) == 0 && sequence == round + 1);
        wait_for_write(requester, cq, &write);
        for (unsigned int rail = 0; rail < PATH_COUNT; rail++)
            rail_packets[round][rail] = counts.sent[rail] - before[rail];
        assert(rail_packets[round][0] == (ports == 1 ? 4 : 2));
        assert(rail_packets[round][1] == (ports == 1 ? 0 : 2));
    }
    assert(!counts.retries[0] && !counts.retries[1]);
    mrc_requester_destroy(requester);
    close_ports(transfer.pool);
    assert(mrc_provider_mr_deregister(pd, keys.lkey) == 0);
    assert(mrc_provider_qp_destroy(qp) == 0);
    assert(mrc_provider_cq_destroy(cq) == 0);
    assert(mrc_provider_pd_destroy(pd) == 0);
    printf("{\"bytes\":%u,\"write_completions\":%u,\"sent_by_round\":[[%u,%u],[%u,%u],[%u,%u]]}\n",
           ROUNDS * WRITE_BYTES, ROUNDS, rail_packets[0][0], rail_packets[0][1],
           rail_packets[1][0], rail_packets[1][1], rail_packets[2][0], rail_packets[2][1]);
}

static void check_jumbo_mbuf_room(void)
{
    const struct mrc_endpoint_config config = { .ip_version = MRC_WIRE_IP4, .pmtu = MRC_MAX_PMTU };
    const uint16_t frame_length = mrc_endpoint_frame_capacity(&config);
    struct rte_mempool *pool = mrc_dpdk_packet_pool_for_frame(frame_length);
    assert(pool);
    assert(rte_pktmbuf_data_room_size(pool) >= frame_length + RTE_ETHER_CRC_LEN + RTE_PKTMBUF_HEADROOM);
    rte_mempool_free(pool);
}

int main(int argc, char **argv)
{
    int consumed = rte_eal_init(argc, argv);
    assert(consumed >= 0);
    check_jumbo_mbuf_room();
    if (argc - consumed == 2 && strcmp(argv[consumed + 1], "link_transition") == 0)
        run_link_transition();
    else
        run_application();
    rte_eal_cleanup();
    return 0;
}
