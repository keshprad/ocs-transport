#include "mrc_qp.h"
#include "mrc_wr.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { PMTU = 8, KEY = 42, VA = 0x1000, WORK = 4, STAGE = 2, RECEIVE = 2 };

struct fixture {
    struct mrc_wr_responder responder;
    struct mrc_wr_slot work[WORK];
    struct mrc_wr_staged staged[STAGE];
    uint8_t memory[128];
    uint8_t staged_payload[STAGE * PMTU];
    uint64_t recv_queue[RECEIVE];
    struct mrc_rx_qp qp;
    uint8_t qp_slots[128];
};

static void init(struct fixture *fixture, uint32_t psn, uint32_t msn, uint16_t rqmsn,
                 size_t max_wimm)
{
    memset(fixture->memory, 0, sizeof(fixture->memory));
    struct mrc_wr_config config = {
        .region = {
            .memory = fixture->memory, .address = VA,
            .length = sizeof(fixture->memory), .r_key = KEY,
        },
        .pmtu = PMTU, .initial_psn = psn, .initial_msn = msn, .initial_rqmsn = rqmsn,
        .max_wimm_inflight = max_wimm, .work = fixture->work, .work_capacity = WORK,
        .staged = fixture->staged, .staged_payload = fixture->staged_payload,
        .staged_capacity = STAGE, .recv_queue = fixture->recv_queue, .recv_capacity = RECEIVE,
    };
    assert(mrc_wr_init(&fixture->responder, &config));
    assert(mrc_rx_init(&fixture->qp, psn, sizeof(fixture->qp_slots), fixture->qp_slots));
}

static enum mrc_wr_result offer(struct fixture *fixture, uint8_t opcode, uint32_t psn,
                                uint16_t msn, uint16_t rqmsn, uint64_t va, uint32_t dmalen,
                                uint32_t immediate, const char *data, size_t length)
{
    assert(mrc_rx_classify(&fixture->qp, psn) == MRC_RX_NEW);
    struct mrc_wire_write write = {
        .msn = msn, .rqmsn = rqmsn, .va = va, .r_key = KEY,
        .dma_len = dmalen, .imm_data = immediate,
        .data = (const uint8_t *)data, .data_len = length,
    };
    enum mrc_wr_result result = mrc_wr_receive(&fixture->responder, psn, opcode, &write);
    if (result == MRC_WR_OK) {
        bool sack_due;
        mrc_rx_record(&fixture->qp, psn, length, 0, 4096, &sack_due);
    }
    return result;
}

static enum mrc_wr_result advance(struct fixture *fixture, struct mrc_wr_completion *completion,
                                  bool *produced)
{
    return mrc_wr_advance(&fixture->responder, fixture->qp.cack_psn, completion, produced);
}

static void test_out_of_order_and_wrap(void)
{
    struct fixture f;
    struct mrc_wr_completion completion;
    bool produced;
    init(&f, MRC_PSN_MASK - 1, 0xffff, 0xffff, 2);
    assert(mrc_wr_post_receive(&f.responder, 101));
    assert(mrc_wr_post_receive(&f.responder, 202));
    assert(!mrc_wr_post_receive(&f.responder, 303));

    /* The later WRITEIMMs place immediately but cannot generate completions. */
    assert(offer(&f, MRC_WRITE_ONLY_IMM, 2, 1, 0, VA + 40, 3, 8, "xyz", 3) == MRC_WR_OK);
    assert(offer(&f, MRC_WRITE_ONLY_IMM, 1, 0, 0xffff, VA + 32, 2, 7, "uv", 2) == MRC_WR_OK);
    assert(memcmp(f.memory + 32, "uv", 2) == 0);
    assert(memcmp(f.memory + 40, "xyz", 3) == 0);
    assert(advance(&f, &completion, &produced) == MRC_WR_OK && !produced);
    assert(f.responder.recv_posted == 2);

    /* A lone MIDDLE must be staged until FIRST or LAST establishes geometry. */
    assert(offer(&f, MRC_WRITE_MIDDLE, MRC_PSN_MASK, 0xffff, 11, VA + 8, 17, 0,
                 "ijklmnop", 8) == MRC_WR_OK);
    assert(f.memory[8] == 0);
    assert(offer(&f, MRC_WRITE_LAST, 0, 0xffff, 20, VA + 16, 17, 0, "q", 1) == MRC_WR_OK);
    assert(memcmp(f.memory + 8, "ijklmnopq", 9) == 0);
    assert(advance(&f, &completion, &produced) == MRC_WR_OK && !produced);

    assert(offer(&f, MRC_WRITE_FIRST, MRC_PSN_MASK - 1, 0xffff, 99, VA, 17, 0,
                 "abcdefgh", 8) == MRC_WR_OK);
    assert(memcmp(f.memory, "abcdefghijklmnopq", 17) == 0);
    assert(advance(&f, &completion, &produced) == MRC_WR_OK && produced);
    assert(completion.wr_id == 101 && completion.msn == 0x10000 && completion.last_psn == 1);
    assert(completion.immediate == 7 && completion.byte_len == 2);
    assert(advance(&f, &completion, &produced) == MRC_WR_OK && produced);
    assert(completion.wr_id == 202 && completion.msn == 0x10001 && completion.last_psn == 2);
    assert(completion.immediate == 8 && completion.byte_len == 3);
    assert(advance(&f, &completion, &produced) == MRC_WR_OK && !produced);
    assert(mrc_wr_ack_msn(&f.responder) == 0x10001);
    assert(f.responder.wimm_inflight == 0);
    assert(mrc_rx_classify(&f.qp, 2) == MRC_RX_DUPLICATE);
}

static void test_reject_malformed_without_placement(void)
{
    struct fixture f;
    init(&f, 20, 1, 1, 1);
    assert(offer(&f, MRC_WRITE_MIDDLE, 21, 1, 1, VA + 9, 17, 0,
                 "ijklmnop", 8) == MRC_WR_OK);
    assert(offer(&f, MRC_WRITE_FIRST, 20, 1, 1, VA, 17, 0, "abcdefgh", 8) == MRC_WR_INVALID);
    for (size_t i = 0; i < sizeof(f.memory); i++)
        assert(f.memory[i] == 0);

    init(&f, 20, 1, 1, 1);
    assert(offer(&f, MRC_WRITE_FIRST, 20, 1, 1, VA + 112, 17, 0,
                 "abcdefgh", 8) == MRC_WR_REMOTE_ACCESS);
    assert(f.memory[112] == 0);

    init(&f, 20, 1, 1, 1);
    assert(offer(&f, MRC_WRITE_FIRST, 20, 1, 1, VA, 17, 0, "short", 5) == MRC_WR_INVALID);
    assert(f.memory[0] == 0);

    init(&f, 20, 1, 1, 1);
    assert(offer(&f, MRC_WRITE_ONLY, 20, 1, 0, VA + 128, 0, 0, NULL, 0) == MRC_WR_OK);
    struct mrc_wr_completion completion;
    bool produced;
    assert(advance(&f, &completion, &produced) == MRC_WR_OK && !produced);
    assert(mrc_wr_ack_msn(&f.responder) == 1);

    init(&f, 20, 1, 1, 1);
    struct mrc_wire_write bad_key = {
        .msn = 1, .va = VA, .r_key = KEY + 1,
        .dma_len = 1, .data = (const uint8_t *)"n", .data_len = 1,
    };
    assert(mrc_wr_receive(&f.responder, 20, MRC_WRITE_ONLY, &bad_key) == MRC_WR_REMOTE_ACCESS);
    assert(f.memory[0] == 0);
}

static void test_imm_resource_errors(void)
{
    struct fixture f;
    struct mrc_wr_completion completion;
    bool produced;

    init(&f, 20, 1, 1, 1);
    assert(offer(&f, MRC_WRITE_ONLY_IMM, 21, 2, 2, VA + 12, 1, 1, "x", 1) == MRC_WR_OK);
    assert(offer(&f, MRC_WRITE_ONLY_IMM, 20, 1, 1, VA, 1, 2, "y", 1) == MRC_WR_IMM_LIMIT);
    assert(f.memory[0] == 0 && f.memory[12] == 'x');
    assert(advance(&f, &completion, &produced) == MRC_WR_QP_ERROR && !produced);

    init(&f, 20, 1, 1, 1);
    assert(offer(&f, MRC_WRITE_ONLY_IMM, 20, 1, 1, VA, 1, 1, "x", 1) == MRC_WR_OK);
    assert(advance(&f, &completion, &produced) == MRC_WR_RQ_EMPTY && !produced);
    assert(!mrc_wr_post_receive(&f.responder, 3));

    init(&f, 20, 1, 1, 1);
    assert(mrc_wr_post_receive(&f.responder, 101));
    assert(offer(&f, MRC_WRITE_LAST_IMM, 21, 1, 1, VA + 8, 9, 7, "i", 1) == MRC_WR_OK);
    assert(offer(&f, MRC_WRITE_FIRST, 20, 1, 2, VA, 9, 0, "abcdefgh", 8) == MRC_WR_INVALID);
    assert(f.memory[0] == 0 && f.memory[8] == 'i');
    assert(f.responder.recv_posted == 1);
}

static void test_work_and_staging_bounds(void)
{
    struct fixture f;
    struct mrc_wr_completion completion;
    bool produced;
    init(&f, 20, 1, 1, 0);

    assert(offer(&f, MRC_WRITE_ONLY, 26, 5, 0, VA + 32, 1, 0, "x", 1) == MRC_WR_NO_RESOURCE);
    assert(f.memory[32] == 0);
    assert(offer(&f, MRC_WRITE_MIDDLE, 21, 1, 0, VA + 8, 33, 0,
                 "ijklmnop", 8) == MRC_WR_OK);
    assert(offer(&f, MRC_WRITE_MIDDLE, 22, 1, 0, VA + 16, 33, 0,
                 "qrstuvwx", 8) == MRC_WR_OK);
    assert(offer(&f, MRC_WRITE_MIDDLE, 23, 1, 0, VA + 24, 33, 0,
                 "yzABCDEF", 8) == MRC_WR_NO_RESOURCE);
    assert(offer(&f, MRC_WRITE_FIRST, 20, 1, 0, VA, 33, 0,
                 "abcdefgh", 8) == MRC_WR_OK);
    assert(offer(&f, MRC_WRITE_MIDDLE, 23, 1, 0, VA + 24, 33, 0,
                 "yzABCDEF", 8) == MRC_WR_OK);
    assert(offer(&f, MRC_WRITE_LAST, 24, 1, 0, VA + 32, 33, 0, "G", 1) == MRC_WR_OK);
    assert(memcmp(f.memory, "abcdefghijklmnopqrstuvwxyzABCDEFG", 33) == 0);
    assert(advance(&f, &completion, &produced) == MRC_WR_OK && !produced);
    assert(offer(&f, MRC_WRITE_ONLY, 28, 5, 0, VA + 96, 1, 0, "z", 1) == MRC_WR_OK);
}

int main(void)
{
    test_out_of_order_and_wrap();
    test_reject_malformed_without_placement();
    test_imm_resource_errors();
    test_work_and_staging_bounds();
    puts("MRC responder work request tests passed");
}
