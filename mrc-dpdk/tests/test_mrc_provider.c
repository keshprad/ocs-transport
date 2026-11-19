#include "mrc_provider.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define READ_WRITE (MRC_PROVIDER_MR_LOCAL_WRITE | MRC_PROVIDER_MR_REMOTE_WRITE)

struct deregistration {
    struct mrc_provider_pd *pd;
    uint32_t lkey;
    int result;
};

static void *deregister(void *argument)
{
    struct deregistration *request = argument;
    request->result = mrc_provider_mr_deregister(request->pd, request->lkey);
    return NULL;
}

static void test_pds_and_keys(void)
{
    struct mrc_provider_pd *first = NULL, *second = NULL;
    assert(mrc_provider_pd_create(0, &first) == -EINVAL);
    assert(mrc_provider_pd_create(SIZE_MAX, &first) == -EOVERFLOW);
    assert(mrc_provider_pd_create(1, &first) == 0);
    assert(mrc_provider_pd_create(1, &second) == 0);
    assert(mrc_provider_pd_release(first) == -EINVAL);
    assert(mrc_provider_pd_retain(first) == 0);
    assert(mrc_provider_pd_destroy(first) == -EBUSY);
    assert(mrc_provider_pd_release(first) == 0);

    _Alignas(16) unsigned char buffer[64] = {0};
    struct mrc_provider_mr_keys a = {0}, b = {0};
    assert(mrc_provider_mr_register(first, buffer, sizeof(buffer), 0x1000,
                                    MRC_PROVIDER_MR_REMOTE_WRITE, &a) == -EINVAL);
    assert(mrc_provider_mr_register(first, buffer, sizeof(buffer), 0x1000, 0x100, &a) == -EINVAL);
    assert(mrc_provider_mr_register(first, buffer, sizeof(buffer), UINT64_MAX - 1,
                                    READ_WRITE, &a) == -EOVERFLOW);
    assert(mrc_provider_mr_register(first, buffer, sizeof(buffer), 0x1000, READ_WRITE, &a) == 0);
    assert(mrc_provider_pd_destroy(first) == -EBUSY);
    assert(mrc_provider_mr_register(first, buffer, sizeof(buffer), 0x1000, 0, &b) == -ENOSPC);
    assert(mrc_provider_mr_register(second, buffer, sizeof(buffer), 0x1000, READ_WRITE, &b) == 0);
    assert(a.lkey && a.rkey && b.lkey && b.rkey);
    assert(a.lkey != a.rkey && a.lkey != b.lkey && a.rkey != b.rkey);

    struct mrc_provider_mr_lease lease = {0};
    struct mrc_provider_mr_access access = {
        .operation = MRC_PROVIDER_MR_WRITE_REMOTE, .key = a.rkey,
        .iova = 0x1010, .length = 16, .alignment = 16,
    };
    assert(mrc_provider_mr_acquire(second, &access, &lease) == -EACCES);
    access.key = a.lkey;
    assert(mrc_provider_mr_acquire(first, &access, &lease) == -EACCES);
    access.key = a.rkey;
    access.operation = MRC_PROVIDER_MR_READ_LOCAL;
    assert(mrc_provider_mr_acquire(first, &access, &lease) == -EACCES);
    access.operation = MRC_PROVIDER_MR_WRITE_REMOTE;
    assert(mrc_provider_mr_acquire(first, &access, &lease) == 0);
    assert(lease.address == buffer + 16 && lease.length == 16);
    memset(lease.address, 0xab, lease.length);
    assert(buffer[15] == 0 && buffer[16] == 0xab && buffer[31] == 0xab && buffer[32] == 0);
    assert(mrc_provider_mr_acquire(first, &access, &lease) == -EBUSY);

    struct deregistration request = { .pd = first, .lkey = a.lkey };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, deregister, &request) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(request.result == -EBUSY);

    assert(mrc_provider_mr_release(&lease) == 0);
    assert(mrc_provider_mr_release(&lease) == -EINVAL);
    assert(mrc_provider_mr_deregister(first, a.rkey) == -ENOENT);
    assert(mrc_provider_mr_deregister(first, a.lkey) == 0);
    struct mrc_provider_mr_keys replacement;
    assert(mrc_provider_mr_register(first, buffer, sizeof(buffer), 0x1000,
                                    READ_WRITE, &replacement) == 0);
    assert(replacement.lkey != a.lkey && replacement.rkey != a.rkey);
    assert(mrc_provider_mr_acquire(first, &access, &lease) == -EACCES);
    assert(mrc_provider_mr_deregister(first, replacement.lkey) == 0);
    assert(mrc_provider_mr_deregister(second, b.lkey) == 0);
    assert(mrc_provider_pd_destroy(first) == 0);
    assert(mrc_provider_pd_destroy(second) == 0);
}

static void test_bounds_and_permissions(void)
{
    struct mrc_provider_pd *pd = NULL;
    assert(mrc_provider_pd_create(2, &pd) == 0);
    _Alignas(16) unsigned char buffer[64] = {0};
    struct mrc_provider_mr_keys read_only, write_only;
    assert(mrc_provider_mr_register(pd, buffer, sizeof(buffer), 0x2000, 0, &read_only) == 0);
    assert(mrc_provider_mr_register(pd, buffer + 1, sizeof(buffer) - 1, 0x4000,
                                    MRC_PROVIDER_MR_LOCAL_WRITE, &write_only) == 0);
    assert(read_only.rkey == 0 && write_only.rkey == 0);

    struct mrc_provider_mr_lease lease = {0};
    struct mrc_provider_mr_access access = {
        .operation = MRC_PROVIDER_MR_WRITE_LOCAL, .key = read_only.lkey,
        .iova = 0x2000, .length = 64,
    };
    assert(mrc_provider_mr_acquire(pd, &access, &lease) == -EACCES);
    access.operation = MRC_PROVIDER_MR_READ_LOCAL;
    assert(mrc_provider_mr_acquire(pd, &access, &lease) == 0);
    assert(lease.address == buffer && lease.length == sizeof(buffer));
    assert(mrc_provider_mr_release(&lease) == 0);

    access.iova--;
    access.length = 0;
    assert(mrc_provider_mr_acquire(pd, &access, &lease) == -EACCES);
    access.iova = 0x2040;
    access.length = 1;
    assert(mrc_provider_mr_acquire(pd, &access, &lease) == -EACCES);
    access.length = 0;
    assert(mrc_provider_mr_acquire(pd, &access, &lease) == 0);
    assert(lease.address == buffer + sizeof(buffer));
    assert(mrc_provider_mr_release(&lease) == 0);
    access.iova = UINT64_MAX;
    assert(mrc_provider_mr_acquire(pd, &access, &lease) == -EACCES);
    access.iova = 0x2001;
    access.length = SIZE_MAX;
    assert(mrc_provider_mr_acquire(pd, &access, &lease) == -EACCES);
    access.length = 1;
    access.alignment = 3;
    assert(mrc_provider_mr_acquire(pd, &access, &lease) == -EINVAL);
    access.alignment = 16;
    assert(mrc_provider_mr_acquire(pd, &access, &lease) == -EACCES);

    access.iova = 0x4000;
    access.key = write_only.lkey;
    access.operation = MRC_PROVIDER_MR_WRITE_LOCAL;
    assert(mrc_provider_mr_acquire(pd, &access, &lease) == -EACCES);
    access.alignment = 0;
    assert(mrc_provider_mr_acquire(pd, &access, &lease) == 0);
    assert(lease.address == buffer + 1);
    assert(mrc_provider_mr_release(&lease) == 0);
    assert(mrc_provider_mr_deregister(pd, read_only.lkey) == 0);
    assert(mrc_provider_mr_deregister(pd, write_only.lkey) == 0);
    assert(mrc_provider_pd_destroy(pd) == 0);
}

static void test_external_keys(void)
{
    unsigned char buffer[32];
    struct mrc_provider_pd *pd;
    assert(mrc_provider_pd_create(4, &pd) == 0);

    struct mrc_provider_mr_keys generated;
    assert(mrc_provider_mr_register(pd, buffer, 8, 0xab000, 0, &generated) == 0);
    struct mrc_provider_mr_keys bound = { .lkey = generated.lkey + 1, .rkey = 0xa0123 };
    assert(mrc_provider_mr_bind(pd, buffer + 8, 8, 0xac000, 0, &bound) == -EINVAL);
    assert(mrc_provider_mr_bind(pd, buffer + 8, 8, 0xac000, READ_WRITE, &bound) == 0);
    struct mrc_provider_mr_keys duplicate = bound;
    duplicate.rkey++;
    assert(mrc_provider_mr_bind(pd, buffer + 16, 8, 0xad000, READ_WRITE, &duplicate) == -EEXIST);
    duplicate = bound;
    duplicate.lkey++;
    assert(mrc_provider_mr_bind(pd, buffer + 16, 8, 0xad000, READ_WRITE, &duplicate) == -EEXIST);

    struct mrc_provider_mr_keys next;
    assert(mrc_provider_mr_register(pd, buffer + 16, 8, 0xad000, 0, &next) == 0);
    assert(next.lkey != bound.lkey);
    struct mrc_provider_mr_access access = {
        .iova = 0xac004, .length = 4, .key = bound.rkey, .operation = MRC_PROVIDER_MR_WRITE_REMOTE,
    };
    struct mrc_provider_mr_lease lease = {0};
    assert(mrc_provider_mr_acquire(pd, &access, &lease) == 0);
    assert(lease.address == buffer + 12 && lease.lkey == bound.lkey);
    assert(mrc_provider_mr_deregister(pd, bound.lkey) == -EBUSY);
    assert(mrc_provider_mr_release(&lease) == 0);
    assert(mrc_provider_mr_deregister(pd, bound.lkey) == 0);
    assert(mrc_provider_mr_acquire(pd, &access, &lease) == -EACCES);
    assert(mrc_provider_mr_deregister(pd, generated.lkey) == 0);
    assert(mrc_provider_mr_deregister(pd, next.lkey) == 0);
    assert(mrc_provider_pd_destroy(pd) == 0);
}

static struct mrc_provider_wc completion(uint64_t id)
{
    return (struct mrc_provider_wc){
        .wr_id = id, .qpn = 42, .opcode = MRC_PROVIDER_WC_WRITE,
        .status = MRC_PROVIDER_WC_SUCCESS, .byte_length = 64,
    };
}

static void test_cq_fifo_and_notifications(void)
{
    struct mrc_provider_cq *cq = NULL;
    assert(mrc_provider_cq_create(0, &cq) == -EINVAL);
    assert(mrc_provider_cq_create(SIZE_MAX, &cq) == -EOVERFLOW);
    assert(mrc_provider_cq_create(3, &cq) == 0);
    assert(mrc_provider_cq_release(cq) == -EINVAL);
    assert(mrc_provider_cq_retain(cq) == 0);
    assert(mrc_provider_cq_destroy(cq) == -EBUSY);
    assert(mrc_provider_cq_release(cq) == 0);

    enum mrc_provider_cq_event event;
    struct mrc_provider_wc entries[3], wc = completion(1);
    assert(mrc_provider_cq_poll(cq, NULL, 1) == -EINVAL);
    assert(mrc_provider_cq_poll(cq, NULL, 0) == 0);
    assert(mrc_provider_cq_poll(cq, entries, (size_t)INT_MAX + 1) == -EINVAL);
    assert(mrc_provider_cq_next_event(cq, &event) == -EAGAIN);
    assert(mrc_provider_cq_arm(cq, true) == 0);
    assert(mrc_provider_cq_push(cq, &wc, false) == 0);
    assert(mrc_provider_cq_next_event(cq, &event) == -EAGAIN);
    wc = completion(2);
    wc.opcode = MRC_PROVIDER_WC_RECV_WRITE_IMM;
    wc.with_immediate = true;
    wc.immediate = 0xf00d;
    assert(mrc_provider_cq_push(cq, &wc, true) == 0);
    wc = completion(3);
    assert(mrc_provider_cq_push(cq, &wc, true) == 0);
    assert(mrc_provider_cq_next_event(cq, &event) == 0 && event == MRC_PROVIDER_CQ_COMPLETION);
    assert(mrc_provider_cq_next_event(cq, &event) == -EAGAIN);

    assert(mrc_provider_cq_poll(cq, entries, 2) == 2);
    assert(entries[0].wr_id == 1 && entries[1].wr_id == 2);
    assert(entries[1].immediate == 0xf00d && entries[1].with_immediate &&
           entries[1].opcode == MRC_PROVIDER_WC_RECV_WRITE_IMM);
    assert(mrc_provider_cq_arm(cq, true) == 0);
    wc = completion(4);
    wc.status = MRC_PROVIDER_WC_LOCAL_PROTECTION;
    assert(mrc_provider_cq_push(cq, &wc, false) == 0);
    wc = completion(5);
    assert(mrc_provider_cq_push(cq, &wc, false) == 0);
    assert(mrc_provider_cq_poll(cq, entries, 3) == 3);
    assert(entries[0].wr_id == 3 && entries[1].wr_id == 4 && entries[2].wr_id == 5);
    assert(entries[1].status == MRC_PROVIDER_WC_LOCAL_PROTECTION);
    assert(mrc_provider_cq_poll(cq, entries, 3) == 0);
    assert(mrc_provider_cq_next_event(cq, &event) == 0 && event == MRC_PROVIDER_CQ_COMPLETION);
    assert(mrc_provider_cq_ack_events(cq, 3) == -EINVAL);
    assert(mrc_provider_cq_destroy(cq) == -EBUSY);
    assert(mrc_provider_cq_ack_events(cq, 2) == 0);
    assert(mrc_provider_cq_destroy(cq) == 0);
}

static void test_cq_overflow(void)
{
    struct mrc_provider_cq *cq = NULL;
    assert(mrc_provider_cq_create(1, &cq) == 0);
    struct mrc_provider_wc wc = completion(1), out;
    enum mrc_provider_cq_event event;
    assert(mrc_provider_cq_arm(cq, false) == 0);
    assert(mrc_provider_cq_push(cq, &wc, false) == 0);
    assert(mrc_provider_cq_push(cq, &wc, false) == -EOVERFLOW);
    assert(mrc_provider_cq_push(cq, &wc, false) == -EOVERFLOW);
    assert(mrc_provider_cq_poll(cq, &out, 1) == -EOVERFLOW);
    assert(mrc_provider_cq_arm(cq, false) == -EOVERFLOW);
    assert(mrc_provider_cq_destroy(cq) == -EBUSY);
    assert(mrc_provider_cq_next_event(cq, &event) == 0 && event == MRC_PROVIDER_CQ_COMPLETION);
    assert(mrc_provider_cq_next_event(cq, &event) == 0 && event == MRC_PROVIDER_CQ_OVERFLOW);
    assert(mrc_provider_cq_next_event(cq, &event) == -EAGAIN);
    assert(mrc_provider_cq_ack_events(cq, 2) == 0);
    assert(mrc_provider_cq_destroy(cq) == 0);
}

static void ready_to_send(struct mrc_provider_qp *qp)
{
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_RESET, MRC_PROVIDER_QP_INIT) == 0);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_INIT, MRC_PROVIDER_QP_RTR) == 0);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_RTR, MRC_PROVIDER_QP_RTS) == 0);
}

static void test_qp_lifecycle_and_ordered_flush(void)
{
    struct mrc_provider_pd *pd;
    struct mrc_provider_cq *scq, *rcq;
    assert(mrc_provider_pd_create(1, &pd) == 0);
    assert(mrc_provider_cq_create(4, &scq) == 0);
    assert(mrc_provider_cq_create(2, &rcq) == 0);
    struct mrc_provider_qp_config config = {
        .pd = pd, .send_cq = scq, .recv_cq = rcq, .qpn = 42, .send_capacity = 4, .recv_capacity = 2,
    };
    struct mrc_provider_qp *qp = NULL;
    config.qpn = 2;
    assert(mrc_provider_qp_create(&config, &qp) == -EINVAL && !qp);
    config.qpn = 42;
    assert(mrc_provider_qp_create(&config, &qp) == 0);
    assert(mrc_provider_pd_destroy(pd) == -EBUSY);
    assert(mrc_provider_cq_destroy(scq) == -EBUSY);
    assert(mrc_provider_cq_destroy(rcq) == -EBUSY);
    struct mrc_provider_qp_attr attr;
    assert(mrc_provider_qp_query(qp, &attr) == 0);
    assert(attr.state == MRC_PROVIDER_QP_RESET && attr.qpn == 42);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_RESET, MRC_PROVIDER_QP_RTS) == -EINVAL);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_RTR, MRC_PROVIDER_QP_RTS) == -EAGAIN);

    uint64_t sequence, id;
    assert(mrc_provider_qp_post_send(qp, 100, 64, false, &sequence) == -EINVAL);
    assert(mrc_provider_qp_post_recv(qp, 200) == -EINVAL);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_RESET, MRC_PROVIDER_QP_INIT) == 0);
    assert(mrc_provider_qp_post_recv(qp, 200) == 0);
    assert(mrc_provider_qp_post_recv(qp, 201) == 0);
    assert(mrc_provider_qp_post_recv(qp, 202) == -ENOSPC);
    assert(mrc_provider_qp_complete_recv(qp, 123, 0xf01, true, &id) == -EINVAL);
    assert(mrc_provider_qp_destroy(qp) == -EBUSY);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_INIT, MRC_PROVIDER_QP_RESET) == -EBUSY);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_INIT, MRC_PROVIDER_QP_RTR) == 0);
    assert(mrc_provider_cq_arm(rcq, true) == 0);
    assert(mrc_provider_qp_complete_recv(qp, 123, 0xf01, true, &id) == 0 && id == 200);
    struct mrc_provider_wc wc[4];
    assert(mrc_provider_cq_poll(rcq, wc, 4) == 1);
    assert(wc[0].status == MRC_PROVIDER_WC_SUCCESS && wc[0].qpn == 42 && wc[0].wr_id == 200);
    assert(wc[0].with_immediate && wc[0].immediate == 0xf01 && wc[0].byte_length == 123);
    enum mrc_provider_cq_event event;
    assert(mrc_provider_cq_next_event(rcq, &event) == 0 && event == MRC_PROVIDER_CQ_COMPLETION);
    assert(mrc_provider_cq_ack_events(rcq, 1) == 0);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_RTR, MRC_PROVIDER_QP_RTS) == 0);

    assert(mrc_provider_qp_post_send(qp, 110, 33, false, &sequence) == 0 && sequence == 1);
    assert(mrc_provider_qp_post_send(qp, 111, 55, true, &sequence) == 0 && sequence == 2);
    assert(mrc_provider_qp_complete_send(qp, 3) == -EINVAL);
    assert(mrc_provider_qp_complete_send(qp, 2) == 0);
    assert(mrc_provider_cq_poll(scq, wc, 4) == 1);
    assert(wc[0].status == MRC_PROVIDER_WC_SUCCESS && wc[0].wr_id == 111 && wc[0].byte_length == 55);
    assert(mrc_provider_qp_post_send(qp, 0, 77, true, &sequence) == 0 && sequence == 3);
    assert(mrc_provider_qp_post_send(qp, 111, 88, false, &sequence) == 0 && sequence == 4);
    assert(mrc_provider_qp_post_send(qp, 111, 99, true, &sequence) == 0 && sequence == 5);
    assert(mrc_provider_qp_fail(qp, 2, MRC_PROVIDER_WC_REMOTE_ACCESS) == -ENOENT);
    assert(mrc_provider_qp_query(qp, &attr) == 0);
    assert(attr.state == MRC_PROVIDER_QP_RTS && attr.send_pending == 3 && attr.recv_pending == 1);
    assert(mrc_provider_qp_fail(qp, 4, MRC_PROVIDER_WC_REMOTE_ACCESS) == 0);
    assert(mrc_provider_qp_query(qp, &attr) == 0);
    assert(attr.state == MRC_PROVIDER_QP_ERROR && attr.send_pending == 0 && attr.recv_pending == 0);
    assert(mrc_provider_qp_post_send(qp, 9, 9, true, &sequence) == -EINVAL);
    assert(mrc_provider_qp_post_recv(qp, 9) == -EINVAL);
    assert(mrc_provider_cq_poll(scq, wc, 4) == 3);
    assert(wc[0].wr_id == 0 && wc[0].status == MRC_PROVIDER_WC_FLUSHED);
    assert(wc[1].wr_id == 111 && wc[1].status == MRC_PROVIDER_WC_REMOTE_ACCESS);
    assert(wc[2].wr_id == 111 && wc[2].status == MRC_PROVIDER_WC_FLUSHED);
    assert(mrc_provider_cq_poll(rcq, wc, 4) == 1);
    assert(wc[0].wr_id == 201 && wc[0].status == MRC_PROVIDER_WC_FLUSHED);

    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_ERROR, MRC_PROVIDER_QP_RESET) == 0);
    ready_to_send(qp);
    assert(mrc_provider_qp_post_send(qp, 111, 0, true, &sequence) == 0 && sequence == 6);
    assert(mrc_provider_qp_complete_send(qp, 1) == -EINVAL);
    assert(mrc_provider_qp_complete_send(qp, 6) == 0);
    assert(mrc_provider_cq_poll(scq, wc, 4) == 1 && wc[0].status == MRC_PROVIDER_WC_SUCCESS);
    assert(mrc_provider_qp_destroy(qp) == 0);
    assert(mrc_provider_cq_destroy(scq) == 0);
    assert(mrc_provider_cq_destroy(rcq) == 0);
    assert(mrc_provider_pd_destroy(pd) == 0);
}

static void test_posted_write_buffer_lifetime(void)
{
    unsigned char buffer[32] = "0123456789abcdefghijklmnopqrst";
    struct mrc_provider_pd *pd;
    struct mrc_provider_cq *cq;
    assert(mrc_provider_pd_create(1, &pd) == 0);
    assert(mrc_provider_cq_create(3, &cq) == 0);
    struct mrc_provider_mr_keys keys;
    assert(mrc_provider_mr_register(pd, buffer, sizeof(buffer), 0x12000, 0, &keys) == 0);
    struct mrc_provider_qp *qp;
    const struct mrc_provider_qp_config config = {
        .pd = pd, .send_cq = cq, .send_capacity = 3, .qpn = 46, .sq_sig_all = true,
    };
    assert(mrc_provider_qp_create(&config, &qp) == 0);
    ready_to_send(qp);

    struct mrc_provider_write write = {
        .wr_id = 201, .local_iova = 0x12004, .remote_address = 0x1900,
        .length = 7, .lkey = keys.lkey, .rkey = 0xaabbccdd,
        .immediate = 0x9988, .with_immediate = true,
    };
    uint64_t first = 0, second, zero_length;
    write.local_buffer = buffer;
    assert(mrc_provider_qp_post_write(qp, &write, &first) == -EINVAL);
    write.local_buffer = NULL;
    write.local_iova = 0x1201f;
    assert(mrc_provider_qp_post_write(qp, &write, &first) == -EACCES);
    write.local_iova = 0x12004;
    write.lkey ^= UINT32_C(0x80000000);
    assert(mrc_provider_qp_post_write(qp, &write, &first) == -EACCES);
    write.lkey = keys.lkey;
    write.remote_address = UINT64_MAX;
    assert(mrc_provider_qp_post_write(qp, &write, &first) == -EOVERFLOW && first == 0);
    write.remote_address = 0x1900;
    assert(mrc_provider_qp_post_write(qp, &write, &first) == 0 && first == 1);

    write.wr_id = 202;
    write.local_iova = 0x12010;
    write.remote_address = 0x2911;
    write.length = 4;
    write.rkey = 0xbbccddaa;
    write.with_immediate = false;
    assert(mrc_provider_qp_post_write(qp, &write, &second) == 0 && second == 2);
    const struct mrc_provider_write empty = { .wr_id = 203, .remote_address = 0x800 };
    assert(mrc_provider_qp_post_write(qp, &empty, &zero_length) == 0 && zero_length == 3);
    assert(mrc_provider_qp_post_write(qp, &write, &zero_length) == -ENOSPC && zero_length == 3);
    assert(mrc_provider_mr_deregister(pd, keys.lkey) == -EBUSY);

    struct mrc_provider_write_view view;
    assert(mrc_provider_qp_get_write(qp, first, &view) == 0);
    assert(view.data == buffer + 4 && memcmp(view.data, "456789a", 7) == 0);
    assert(view.remote_address == 0x1900 && view.rkey == 0xaabbccdd && view.length == 7);
    assert(view.with_immediate && view.immediate == 0x9988);
    assert(mrc_provider_qp_get_write(qp, second, &view) == 0);
    assert(view.data == buffer + 16 && memcmp(view.data, "ghij", 4) == 0);
    assert(view.remote_address == 0x2911 && view.rkey == 0xbbccddaa && view.length == 4);
    assert(!view.with_immediate);
    assert(mrc_provider_qp_get_write(qp, zero_length, &view) == 0 && !view.data && !view.length);

    assert(mrc_provider_qp_complete_send(qp, first) == 0);
    assert(mrc_provider_qp_get_write(qp, first, &view) == -ENOENT);
    assert(mrc_provider_mr_deregister(pd, keys.lkey) == -EBUSY);
    assert(mrc_provider_qp_fail(qp, second, MRC_PROVIDER_WC_RETRY_EXCEEDED) == 0);
    assert(mrc_provider_qp_get_write(qp, second, &view) == -ENOENT);
    assert(mrc_provider_mr_deregister(pd, keys.lkey) == 0);

    struct mrc_provider_wc wc[3];
    assert(mrc_provider_cq_poll(cq, wc, 3) == 3);
    assert(wc[0].wr_id == 201 && wc[0].byte_length == 7 && wc[0].status == MRC_PROVIDER_WC_SUCCESS);
    assert(wc[1].wr_id == 202 && wc[1].status == MRC_PROVIDER_WC_RETRY_EXCEEDED);
    assert(wc[2].wr_id == 203 && wc[2].status == MRC_PROVIDER_WC_FLUSHED);
    assert(mrc_provider_qp_destroy(qp) == 0);
    assert(mrc_provider_cq_destroy(cq) == 0);
    assert(mrc_provider_pd_destroy(pd) == 0);
}

static void test_gathered_write_and_reset(void)
{
    struct mrc_provider_pd *pd;
    struct mrc_provider_cq *cq;
    assert(mrc_provider_pd_create(2, &pd) == 0);
    assert(mrc_provider_cq_create(2, &cq) == 0);
    unsigned char first[] = "abcdef";
    unsigned char second[] = "ghijkl";
    struct mrc_provider_mr_keys keys[2];
    assert(mrc_provider_mr_register(pd, first, sizeof(first), 0x12000, 0, &keys[0]) == 0);
    assert(mrc_provider_mr_register(pd, second, sizeof(second), 0x14000, 0, &keys[1]) == 0);

    struct mrc_provider_qp *qp;
    const struct mrc_provider_qp_config config = {
        .pd = pd, .send_cq = cq, .recv_cq = cq, .send_capacity = 2, .recv_capacity = 1,
        .qpn = 47, .sq_sig_all = true,
    };
    assert(mrc_provider_qp_create(&config, &qp) == 0);
    ready_to_send(qp);

    struct mrc_provider_sge sges[] = {
        { .iova = 0x12001, .length = 4, .lkey = keys[0].lkey },
        {0},
        { .iova = 0x14001, .length = 4, .lkey = keys[1].lkey },
    };
    const struct mrc_provider_write write = {
        .wr_id = 301, .remote_address = 0x1000, .length = 8, .rkey = 0x65432,
        .with_immediate = true, .immediate = 0x11223344,
    };
    uint64_t sequence = 0;
    assert(mrc_provider_qp_post_writev(qp, &write, sges, 2, &sequence) == -EINVAL);
    sges[2].iova = 0x14005;
    assert(mrc_provider_qp_post_writev(qp, &write, sges, 3, &sequence) == -EACCES && !sequence);
    assert(mrc_provider_mr_deregister(pd, keys[0].lkey) == 0);
    assert(mrc_provider_mr_register(pd, first, sizeof(first), 0x12000, 0, &keys[0]) == 0);
    sges[0].lkey = keys[0].lkey;
    sges[2].iova = 0x14001;

    assert(mrc_provider_qp_post_writev(qp, &write, sges, 3, &sequence) == 0 && sequence == 1);
    assert(mrc_provider_mr_deregister(pd, keys[0].lkey) == -EBUSY);
    assert(mrc_provider_mr_deregister(pd, keys[1].lkey) == -EBUSY);
    struct mrc_provider_write_view view;
    assert(mrc_provider_qp_get_write(qp, sequence, &view) == 0);
    assert(!view.data && view.length == 8 && view.with_immediate);
    assert(view.immediate == 0x11223344 && view.rkey == 0x65432);
    uint8_t scratch[5];
    assert(mrc_provider_write_slice(&view, 4, 2, scratch) == second + 1);
    assert(mrc_provider_write_slice(&view, 2, 5, scratch) == scratch);
    assert(memcmp(scratch, "dehij", sizeof(scratch)) == 0);
    assert(mrc_provider_write_slice(&view, 2, 5, NULL) == NULL);
    assert(mrc_provider_write_slice(&view, 6, 3, scratch) == NULL);
    assert(mrc_provider_write_slice(&view, 8, 0, scratch) == NULL);
    assert(mrc_provider_qp_complete_send(qp, sequence) == 0);

    assert(mrc_provider_qp_post_writev(qp, &write, sges, 3, &sequence) == 0 && sequence == 2);
    assert(mrc_provider_qp_fail(qp, sequence, MRC_PROVIDER_WC_RETRY_EXCEEDED) == 0);
    assert(mrc_provider_qp_transition(qp, MRC_PROVIDER_QP_ERROR, MRC_PROVIDER_QP_RESET) == 0);
    ready_to_send(qp);
    assert(mrc_provider_qp_post_writev(qp, &write, sges, 3, &sequence) == 0 && sequence == 3);
    assert(mrc_provider_qp_post_recv(qp, 302) == 0);
    assert(mrc_provider_qp_reset(qp) == 0);

    struct mrc_provider_qp_attr attr;
    assert(mrc_provider_qp_query(qp, &attr) == 0);
    assert(attr.state == MRC_PROVIDER_QP_RESET && !attr.send_pending && !attr.recv_pending);
    assert(attr.send_first_sequence == sequence + 1);
    assert(mrc_provider_qp_get_write(qp, sequence, &view) == -ENOENT);
    struct mrc_provider_wc wc[2];
    assert(mrc_provider_cq_poll(cq, wc, 2) == 2);
    assert(wc[0].wr_id == write.wr_id && wc[0].status == MRC_PROVIDER_WC_SUCCESS &&
           wc[0].byte_length == write.length);
    assert(wc[1].wr_id == write.wr_id && wc[1].status == MRC_PROVIDER_WC_RETRY_EXCEEDED);
    assert(mrc_provider_cq_poll(cq, wc, 2) == 0);
    assert(mrc_provider_mr_deregister(pd, keys[0].lkey) == 0);
    assert(mrc_provider_mr_deregister(pd, keys[1].lkey) == 0);
    assert(mrc_provider_qp_destroy(qp) == 0);
    assert(mrc_provider_cq_destroy(cq) == 0);
    assert(mrc_provider_pd_destroy(pd) == 0);
}

static void test_qp_cq_overrun_flushes_other_queue(void)
{
    struct mrc_provider_cq *send, *recv;
    assert(mrc_provider_cq_create(1, &send) == 0);
    assert(mrc_provider_cq_create(1, &recv) == 0);
    struct mrc_provider_qp_config config = {
        .send_cq = send, .recv_cq = recv, .qpn = 48, .send_capacity = 2, .recv_capacity = 1,
        .sq_sig_all = true,
    };
    struct mrc_provider_qp *qp;
    assert(mrc_provider_qp_create(&config, &qp) == 0);
    ready_to_send(qp);
    uint64_t sequence;
    assert(mrc_provider_qp_post_recv(qp, 201) == 0);
    assert(mrc_provider_qp_post_send(qp, 101, 1, false, &sequence) == 0);
    assert(mrc_provider_qp_post_send(qp, 102, 1, false, &sequence) == 0);
    assert(mrc_provider_qp_complete_send(qp, sequence) == -EOVERFLOW);
    struct mrc_provider_qp_attr attr;
    assert(mrc_provider_qp_query(qp, &attr) == 0);
    assert(attr.state == MRC_PROVIDER_QP_ERROR && !attr.send_pending && !attr.recv_pending);
    struct mrc_provider_wc wc;
    assert(mrc_provider_cq_poll(recv, &wc, 1) == 1);
    assert(wc.wr_id == 201 && wc.status == MRC_PROVIDER_WC_FLUSHED);
    enum mrc_provider_cq_event event;
    assert(mrc_provider_cq_next_event(send, &event) == 0 && event == MRC_PROVIDER_CQ_OVERFLOW);
    assert(mrc_provider_cq_ack_events(send, 1) == 0);
    assert(mrc_provider_qp_destroy(qp) == 0);
    assert(mrc_provider_cq_destroy(send) == 0);
    assert(mrc_provider_cq_destroy(recv) == 0);
}

struct cq_destroy_request {
    struct mrc_provider_cq *cq;
    int result;
};

static void *destroy_cq_wait(void *argument)
{
    struct cq_destroy_request *request = argument;
    request->result = mrc_provider_cq_destroy_wait(request->cq);
    return NULL;
}

static void test_completion_channel(void)
{
    struct mrc_provider_comp_channel *channel = NULL;
    struct mrc_provider_cq *send, *recv, *event_cq = NULL;
    void *event_context = NULL;
    int send_context, recv_context;
    assert(mrc_provider_comp_channel_create(&channel) == 0);
    int fd = mrc_provider_comp_channel_fd(channel);
    assert(fd >= 0);
    struct pollfd waiter = { .fd = fd, .events = POLLIN };
    assert(poll(&waiter, 1, 0) == 0);
    assert(mrc_provider_comp_channel_next(channel, &event_cq, &event_context) == -EAGAIN);
    assert(mrc_provider_cq_create_on_channel(2, channel, &send_context, &send) == 0);
    assert(mrc_provider_cq_create_on_channel(2, channel, &recv_context, &recv) == 0);
    assert(mrc_provider_comp_channel_destroy(channel) == -EBUSY);

    const struct mrc_provider_qp_config config = {
        .send_cq = send, .recv_cq = recv, .qpn = 42, .send_capacity = 2, .recv_capacity = 2,
    };
    struct mrc_provider_qp *qp;
    assert(mrc_provider_qp_create(&config, &qp) == 0);
    assert(mrc_provider_cq_destroy_wait(send) == -EBUSY);
    ready_to_send(qp);
    assert(mrc_provider_cq_arm(send, false) == 0);
    assert(mrc_provider_cq_arm(recv, true) == 0);
    uint64_t first, second, id;
    assert(mrc_provider_qp_post_send(qp, 100, 33, true, &first) == 0);
    assert(mrc_provider_qp_post_recv(qp, 200) == 0);
    assert(mrc_provider_qp_complete_recv(qp, 44, 0x01020304, false, &id) == 0 && id == 200);
    assert(poll(&waiter, 1, 0) == 0);
    assert(mrc_provider_qp_complete_send(qp, first) == 0);
    assert(mrc_provider_qp_post_recv(qp, 201) == 0);
    assert(mrc_provider_qp_complete_recv(qp, 55, 0x04030201, true, &id) == 0 && id == 201);

    assert(poll(&waiter, 1, 0) == 1 && (waiter.revents & POLLIN));
    enum mrc_provider_cq_event event;
    assert(mrc_provider_cq_next_event(send, &event) == -EAGAIN);
    struct mrc_provider_wc wc[2];
    /* Polling a CQ can consume the WC while its notification still waits. */
    assert(mrc_provider_cq_poll(send, wc, 2) == 1 && wc[0].wr_id == 100);
    assert(mrc_provider_comp_channel_next(channel, &event_cq, &event_context) == 0);
    assert(event_cq == send && event_context == &send_context);
    assert(mrc_provider_cq_arm(send, false) == 0);
    assert(mrc_provider_qp_post_send(qp, 101, 66, true, &second) == 0);
    assert(mrc_provider_qp_complete_send(qp, second) == 0);
    assert(mrc_provider_qp_destroy(qp) == 0);
    assert(mrc_provider_cq_destroy(send) == -EBUSY);

    struct cq_destroy_request destroy = { .cq = send, .result = -1 };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, destroy_cq_wait, &destroy) == 0);
    struct timespec delay = { .tv_nsec = 1000000 };
    for (unsigned int attempts = 0;; attempts++) {
        int result = mrc_provider_cq_arm(send, false);
        if (result == -EBUSY)
            break;
        assert(result == 0 && attempts < 2000);
        nanosleep(&delay, NULL);
    }

    /* The unread send event is gone, and the read send event pins the CQ. */
    assert(mrc_provider_comp_channel_next(channel, &event_cq, &event_context) == 0);
    assert(event_cq == recv && event_context == &recv_context);
    assert(mrc_provider_comp_channel_next(channel, &event_cq, &event_context) == -EAGAIN);
    assert(poll(&waiter, 1, 0) == 0);
    assert(mrc_provider_cq_ack_events(send, 1) == 0);
    assert(pthread_join(thread, NULL) == 0 && destroy.result == 0);

    assert(mrc_provider_cq_poll(recv, wc, 2) == 2);
    assert(wc[0].wr_id == 200 && wc[1].wr_id == 201);
    assert(wc[1].with_immediate && wc[1].immediate == 0x04030201);
    assert(mrc_provider_cq_ack_events(recv, 1) == 0);
    assert(mrc_provider_cq_destroy_wait(recv) == 0);
    assert(mrc_provider_comp_channel_destroy(channel) == 0);
}

int main(void)
{
    test_pds_and_keys();
    test_bounds_and_permissions();
    test_external_keys();
    test_cq_fifo_and_notifications();
    test_cq_overflow();
    test_qp_lifecycle_and_ordered_flush();
    test_posted_write_buffer_lifetime();
    test_gathered_write_and_reset();
    test_qp_cq_overrun_flushes_other_queue();
    test_completion_channel();
    puts("MRC provider resource tests passed");
    return 0;
}
