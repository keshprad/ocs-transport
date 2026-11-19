#include "mrc_qp.h"
#include "mrc_wr.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
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

static void init_responder(struct fixture *fixture, uint32_t psn, uint32_t msn, uint16_t rqmsn,
                           size_t max_wimm, struct mrc_provider_pd *pd)
{
    memset(fixture->memory, 0, sizeof(fixture->memory));
    struct mrc_wr_config config = {
        .region = {
            .memory = fixture->memory, .address = VA,
            .length = sizeof(fixture->memory), .r_key = KEY,
        },
        .pd = pd, .pmtu = PMTU, .initial_psn = psn, .initial_msn = msn, .initial_rqmsn = rqmsn,
        .max_wimm_inflight = max_wimm, .work = fixture->work, .work_capacity = WORK,
        .staged = fixture->staged, .staged_payload = fixture->staged_payload,
        .staged_capacity = STAGE, .recv_queue = fixture->recv_queue, .recv_capacity = RECEIVE,
    };
    assert(mrc_wr_init(&fixture->responder, &config));
    assert(mrc_rx_init(&fixture->qp, psn, sizeof(fixture->qp_slots), fixture->qp_slots));
}

static void init(struct fixture *fixture, uint32_t psn, uint32_t msn, uint16_t rqmsn,
                 size_t max_wimm)
{
    init_responder(fixture, psn, msn, rqmsn, max_wimm, NULL);
}

static enum mrc_wr_result offer_record(struct fixture *fixture, uint8_t opcode, uint32_t psn,
                                       const struct mrc_wire_write *write)
{
    assert(mrc_rx_classify(&fixture->qp, psn) == MRC_RX_NEW);
    enum mrc_wr_result result = mrc_wr_receive(&fixture->responder, psn, opcode, write, false);
    if (result == MRC_WR_OK) {
        bool sack_due;
        mrc_rx_record(&fixture->qp, psn, write->data_len, 0, 4096, &sack_due);
    }
    return result;
}

static enum mrc_wr_result offer(struct fixture *fixture, uint8_t opcode, uint32_t psn,
                                uint16_t msn, uint16_t rqmsn, uint64_t va, uint32_t dmalen,
                                uint32_t immediate, const char *data, size_t length)
{
    const struct mrc_wire_write write = {
        .msn = msn, .rqmsn = rqmsn, .va = va, .r_key = KEY,
        .dma_len = dmalen, .imm_data = immediate,
        .data = (const uint8_t *)data, .data_len = length,
    };
    return offer_record(fixture, opcode, psn, &write);
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
    assert(mrc_wr_receive(&f.responder, 20, MRC_WRITE_ONLY, &bad_key, false) == MRC_WR_REMOTE_ACCESS);
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
                 "yzABCDEF", 8) == MRC_WR_NO_PKT_BUFFER);
    assert(offer(&f, MRC_WRITE_FIRST, 20, 1, 0, VA, 33, 0,
                 "abcdefgh", 8) == MRC_WR_OK);
    assert(offer(&f, MRC_WRITE_MIDDLE, 23, 1, 0, VA + 24, 33, 0,
                 "yzABCDEF", 8) == MRC_WR_OK);
    assert(offer(&f, MRC_WRITE_LAST, 24, 1, 0, VA + 32, 33, 0, "G", 1) == MRC_WR_OK);
    assert(memcmp(f.memory, "abcdefghijklmnopqrstuvwxyzABCDEFG", 33) == 0);
    assert(advance(&f, &completion, &produced) == MRC_WR_OK && !produced);
    assert(offer(&f, MRC_WRITE_ONLY, 28, 5, 0, VA + 96, 1, 0, "z", 1) == MRC_WR_OK);
}

struct deregistration {
    struct mrc_provider_pd *pd;
    uint32_t key;
    int result;
};

static void *try_deregister(void *argument)
{
    struct deregistration *request = argument;
    request->result = mrc_provider_mr_deregister(request->pd, request->key);
    return NULL;
}

static struct mrc_provider_mr_keys register_write(struct mrc_provider_pd *pd,
                                                     uint8_t *memory, size_t length)
{
    struct mrc_provider_mr_keys keys;
    unsigned int flags = MRC_PROVIDER_MR_LOCAL_WRITE | MRC_PROVIDER_MR_REMOTE_WRITE;
    assert(mrc_provider_mr_register(pd, memory, length, VA, flags, &keys) == 0);
    return keys;
}

static void test_provider_ordered_leases(void)
{
    struct mrc_provider_pd *pd = NULL;
    uint8_t first[24] = {0}, second[24] = {0};
    assert(mrc_provider_pd_create(2, &pd) == 0);
    struct mrc_provider_mr_keys a = register_write(pd, first, sizeof(first));
    struct mrc_provider_mr_keys b = register_write(pd, second, sizeof(second));
    struct fixture f;
    init_responder(&f, 20, 1, 1, 1, pd);
    assert(mrc_wr_post_receive(&f.responder, 101));

    const struct mrc_wire_write later = {
        .msn = 2, .rqmsn = 1, .va = VA + 8, .r_key = b.rkey,
        .dma_len = 3, .imm_data = 19, .data = (const uint8_t *)"xyz", .data_len = 3,
    };
    assert(offer_record(&f, MRC_WRITE_ONLY_IMM, 23, &later) == MRC_WR_OK);
    assert(memcmp(second + 8, "xyz", 3) == 0 && first[8] == 0 && f.memory[8] == 0);

    struct mrc_wr_completion completion;
    bool produced;
    assert(advance(&f, &completion, &produced) == MRC_WR_OK && !produced);
    struct deregistration request = { .pd = pd, .key = b.lkey };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, try_deregister, &request) == 0);
    assert(pthread_join(thread, NULL) == 0 && request.result == -EBUSY);

    struct mrc_wire_write earlier = {
        .msn = 1, .va = VA + 8, .r_key = a.rkey, .dma_len = 17,
        .data = (const uint8_t *)"ijklmnop", .data_len = 8,
    };
    assert(offer_record(&f, MRC_WRITE_MIDDLE, 21, &earlier) == MRC_WR_OK);
    assert(first[8] == 0);
    earlier.va = VA + 16;
    earlier.data = (const uint8_t *)"q";
    earlier.data_len = 1;
    assert(offer_record(&f, MRC_WRITE_LAST, 22, &earlier) == MRC_WR_OK);
    assert(memcmp(first + 8, "ijklmnopq", 9) == 0);
    assert(mrc_provider_mr_deregister(pd, a.lkey) == -EBUSY);

    earlier.va = VA;
    earlier.data = (const uint8_t *)"abcdefgh";
    earlier.data_len = 8;
    assert(offer_record(&f, MRC_WRITE_FIRST, 20, &earlier) == MRC_WR_OK);
    assert(memcmp(first, "abcdefghijklmnopq", 17) == 0);
    assert(advance(&f, &completion, &produced) == MRC_WR_OK && produced);
    assert(completion.wr_id == 101 && completion.msn == 2 && completion.immediate == 19);
    assert(mrc_provider_mr_deregister(pd, a.lkey) == 0);
    assert(mrc_provider_mr_deregister(pd, b.lkey) == 0);
    assert(mrc_provider_pd_destroy(pd) == -EBUSY);
    mrc_wr_destroy(&f.responder);
    mrc_wr_destroy(&f.responder);
    assert(mrc_provider_pd_destroy(pd) == 0);
}

static void test_provider_rejects_staged_key_or_geometry(void)
{
    struct mrc_provider_pd *pd = NULL;
    uint8_t memory[24] = {0};
    assert(mrc_provider_pd_create(1, &pd) == 0);
    struct mrc_provider_mr_keys a = register_write(pd, memory, sizeof(memory));
    struct fixture f;
    init_responder(&f, 20, 1, 1, 1, pd);

    struct mrc_wire_write write = {
        .msn = 1, .va = VA + 8, .r_key = a.rkey, .dma_len = 17,
        .data = (const uint8_t *)"abcdefgh", .data_len = 8,
    };
    assert(offer_record(&f, MRC_WRITE_MIDDLE, 21, &write) == MRC_WR_OK);
    assert(memory[8] == 0);
    assert(mrc_provider_mr_deregister(pd, a.lkey) == 0);
    struct mrc_provider_mr_keys b = register_write(pd, memory, sizeof(memory));
    assert(b.rkey != a.rkey);
    write.va = VA;
    assert(offer_record(&f, MRC_WRITE_FIRST, 20, &write) == MRC_WR_REMOTE_ACCESS);
    assert(memory[0] == 0 && memory[8] == 0);
    mrc_wr_destroy(&f.responder);

    init_responder(&f, 20, 1, 1, 1, pd);
    write.r_key = b.rkey;
    write.va = VA + 9;
    assert(offer_record(&f, MRC_WRITE_MIDDLE, 21, &write) == MRC_WR_OK);
    write.va = VA;
    assert(offer_record(&f, MRC_WRITE_FIRST, 20, &write) == MRC_WR_INVALID);
    assert(memory[0] == 0 && memory[9] == 0);
    mrc_wr_destroy(&f.responder);

    init_responder(&f, 20, 1, 1, 1, pd);
    write.va = VA + 8;
    assert(offer_record(&f, MRC_WRITE_FIRST, 20, &write) == MRC_WR_REMOTE_ACCESS);
    assert(memory[8] == 0);
    mrc_wr_destroy(&f.responder);
    assert(mrc_provider_mr_deregister(pd, b.lkey) == 0);
    assert(mrc_provider_pd_destroy(pd) == 0);
}

static void test_provider_releases_on_error_and_destroy(void)
{
    struct mrc_provider_pd *pd = NULL;
    uint8_t first[24] = {0}, second[24] = {0};
    assert(mrc_provider_pd_create(2, &pd) == 0);
    struct mrc_provider_mr_keys a = register_write(pd, first, sizeof(first));
    struct mrc_provider_mr_keys b = register_write(pd, second, sizeof(second));
    struct fixture f;
    init_responder(&f, 20, 1, 1, 1, pd);

    struct mrc_wire_write write = {
        .msn = 1, .va = VA, .r_key = a.rkey, .dma_len = 17,
        .data = (const uint8_t *)"abcdefgh", .data_len = 8,
    };
    assert(offer_record(&f, MRC_WRITE_FIRST, 20, &write) == MRC_WR_OK);
    assert(mrc_provider_mr_deregister(pd, a.lkey) == -EBUSY);
    write.va = VA + 8;
    write.r_key = b.rkey;
    assert(offer_record(&f, MRC_WRITE_MIDDLE, 21, &write) == MRC_WR_REMOTE_ACCESS);
    assert(first[8] == 0 && second[8] == 0);
    assert(mrc_provider_mr_deregister(pd, a.lkey) == 0);
    mrc_wr_destroy(&f.responder);

    init_responder(&f, 20, 1, 1, 1, pd);
    write.va = VA;
    assert(offer_record(&f, MRC_WRITE_FIRST, 20, &write) == MRC_WR_OK);
    assert(mrc_provider_mr_deregister(pd, b.lkey) == -EBUSY);
    mrc_wr_destroy(&f.responder);
    assert(mrc_provider_mr_deregister(pd, b.lkey) == 0);
    assert(mrc_provider_pd_destroy(pd) == 0);
}

int main(void)
{
    test_out_of_order_and_wrap();
    test_reject_malformed_without_placement();
    test_imm_resource_errors();
    test_work_and_staging_bounds();
    test_provider_ordered_leases();
    test_provider_rejects_staged_key_or_geometry();
    test_provider_releases_on_error_and_destroy();
    puts("MRC responder work request tests passed");
}
