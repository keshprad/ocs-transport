/* Exact-clock endpoint integration fixture. Include the implementation to inspect
 * private congestion state without adding a production diagnostics interface.
 * Frames and expected RTT values come from the independent Python peer. */
#include "../src/mrc_endpoint.c"
#include "mrc_connect.h"
#include <assert.h>

int main(int argc, char **argv)
{
    assert(argc == 8 || argc == 9);
    const unsigned local = (unsigned)strtoul(argv[2], NULL, 0);
    const unsigned peer = (unsigned)strtoul(argv[3], NULL, 0);
    const uint64_t elapsed = strtoull(argv[4], NULL, 0);
    const unsigned retransmissions = (unsigned)strtoul(argv[5], NULL, 0);
    const uint64_t expected = strtoull(argv[6], NULL, 0);
    const bool probe = strtoul(argv[7], NULL, 0) != 0;
    const uint64_t sent_at = argc == 9 ? strtoull(argv[8], NULL, 0) : 1000;
    uint8_t frame[2048];
    FILE *file = fopen(argv[1], "rb");
    assert(file);
    size_t length = fread(frame, 1, sizeof(frame), file);
    assert(!ferror(file) && feof(file));
    fclose(file);
    struct mrc_wire_packet packet;
    assert(mrc_wire_decode_srv6(frame, length, &packet, NULL) == 0);

    struct mrc_connect_offer offers[2] = {
        { .receive = { .qpn = 0x120345, .max_mpr = 1, .max_wimm = 1 },
          .pmtu = 1024, .p_key = 0xaf31, .supported = MRC_CONNECT_SERVICE_TIME },
        { .receive = { .qpn = 0x230456, .max_mpr = 1, .max_wimm = 1 },
          .pmtu = 1024, .p_key = 0xaf31, .supported = MRC_CONNECT_SERVICE_TIME },
    };
    offers[0].offered = local ? MRC_CONNECT_SERVICE_TIME : 0;
    offers[1].offered = peer ? MRC_CONNECT_SERVICE_TIME : 0;
    struct mrc_connect_qp connection;
    assert(mrc_connect_negotiate(&offers[0], &offers[1], &connection) == 0);
    struct mrc_endpoint_write work = { .packets = 1, .length = 13 };
    struct mrc_requester sender = {0};
    struct mrc_endpoint_config *config = &sender.transfer;
    *config = (struct mrc_endpoint_config){
        .sender = true, .packets = 1, .bytes = 13, .work_requests = 1,
        .uniform_wr_packets = 1, .work = &work, .initial_psn = 0xfffffd,
        .requestor_qpn = 0x120345, .responder_qpn = 0x230456,
        .window = 128, .hz = 1000000000, .exponential_retries = 4,
        .report_service_time = !!(connection.local_responder_features & MRC_CONNECT_SERVICE_TIME),
        .peer_service_time = !!(connection.peer_responder_features & MRC_CONNECT_SERVICE_TIME),
    };
    struct mrc_tx_packet slots[128];
    struct send_record metadata[128] = {0};
    sender.metadata = metadata;
    sender.next = 1;
    assert(mrc_tx_init(&sender.qp, config->initial_psn, 128, slots));
    assert(mrc_tx_track(&sender.qp, config->initial_psn, sent_at));
    mrc_tx_packet(&sender.qp, config->initial_psn)->retransmissions = retransmissions;
    assert(mrc_ev_init(&sender.ev, 2, 1, 1000, 1000));
    assert(mrc_mpr_init(&sender.mpr, false, 1));
    const uint32_t nominal = nominal_at(config, 0);
    assert(mrc_cc_init(&sender.cc, nominal, nominal * 128, 0, 20000));
    assert(mrc_cc_on_send(&sender.cc, nominal));
    metadata[0].counted_inflight = true;
    if (probe) {
        sender.probes[0] = (struct outstanding_probe){
            .sent_at = sent_at, .entropy = 0xc000, .id = 7, .pending = true,
        };
        sender.initial_probes = 1;
    }
    struct mrc_endpoint_counters counts = {0};
    assert(apply_control(config, &sender, &counts, &packet, sent_at + elapsed) == 0);
    if (sender.cc.base_rtt_ns != expected) {
        fprintf(stderr, "local=%u peer=%u opcode=%#x elapsed=%" PRIu64
                " retries=%u sent_at=%" PRIu64 " expected=%" PRIu64 " actual=%" PRIu64 "\n",
                local, peer, packet.bth.opcode, elapsed, retransmissions,
                sent_at, expected, sender.cc.base_rtt_ns);
        return 1;
    }
    /* A remote duration must never become or replace the stored send clock. */
    assert(mrc_tx_packet(&sender.qp, config->initial_psn)->sent_at == sent_at);
    if (probe)
        assert(sender.probes[0].sent_at == sent_at);
    if (packet.bth.opcode == MRC_RELIABILITY_SACK) {
        /* An unusable RTT sample must not discard valid reliability/byte
         * feedback. The independent fixture advertises one 256-byte unit. */
        assert(sender.cc.prev_rcvd_bytes == 1);
        assert(sender.cc.inflight == (int32_t)nominal - 256);
        assert(counts.sacks == 1 && counts.probe_replies == (unsigned)probe);
        if (!probe)
            assert(mrc_tx_packet(&sender.qp, config->initial_psn)->received);
    } else {
        assert(counts.nack_received_mask == (UINT32_C(1) << packet.body.nack.reason));
        assert(metadata[0].nacked && !metadata[0].counted_inflight);
        assert(sender.cc.inflight == 0);
    }
    return 0;
}
