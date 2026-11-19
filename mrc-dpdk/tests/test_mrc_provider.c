#include "mrc_provider.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

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

int main(void)
{
    test_pds_and_keys();
    test_bounds_and_permissions();
    test_cq_fifo_and_notifications();
    test_cq_overflow();
    puts("MRC provider resource tests passed");
    return 0;
}
