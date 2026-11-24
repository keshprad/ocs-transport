#define _GNU_SOURCE
#include "../src/roce_mr_ipc.h"
#include "../src/roce_mr_server.h"
#include "../src/roce_native_connection.h"
#include "../src/roce_native_queue.h"

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct mr_keys {
    uint32_t lkey, rkey;
    uint64_t iova;
    size_t length;
};

static void send_value(int fd, const void *data, size_t length)
{
    assert(write(fd, data, length) == (ssize_t)length);
}

static void recv_value(int fd, void *data, size_t length)
{
    assert(recv(fd, data, length, MSG_TRUNC) == (ssize_t)length);
}

static void expect(int fd, char command)
{
    char received;
    recv_value(fd, &received, 1);
    assert(received == command);
}

static void share_keys(int fd, const struct ibv_mr *mr, uint64_t iova)
{
    const struct mr_keys keys = { mr->lkey, mr->rkey, iova, mr->length };
    send_value(fd, &keys, sizeof(keys));
}

static void check_early_qp(struct ibv_pd *pd, struct ibv_mr *mr, uint64_t iova)
{
    struct ibv_context *context = pd->context;
    struct ibv_pd *other = ibv_alloc_pd(context);
    struct ibv_cq *cq = ibv_create_cq(context, 1, NULL, NULL, 0);
    assert(other && cq);
    struct ibv_qp_init_attr init = {
        .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 1, .max_send_sge = 1 },
    };
    struct ibv_qp *qp = ibv_create_qp(other, &init);
    assert(qp && ibv_dealloc_pd(other) == EBUSY && ibv_destroy_cq(cq) == EBUSY);
    assert(!ibv_destroy_qp(qp));
    qp = ibv_create_qp(pd, &init);
    assert(qp && qp->qp_num > 1 && qp->qp_num < ROCE_PSN_MASK);
    uint32_t qpn = qp->qp_num;
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT, .port_num = 1, .qp_access_flags = IBV_ACCESS_REMOTE_WRITE,
    };
    assert(!ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS));
    assert(!ibv_query_qp(qp, &attr, IBV_QP_STATE, &init) && attr.qp_state == IBV_QPS_INIT);
    struct ibv_wc wc;
    assert(!ibv_poll_cq(cq, 1, &wc));

    void *provider = dlopen("libroce_dpdk-rdmav34.so", RTLD_NOLOAD | RTLD_NOW);
    assert(provider);
    int (*bind_queues)(struct ibv_context *, int) = dlsym(provider, "roce_dpdk_bind_queues");
    assert(bind_queues);
    int fd = memfd_create("roce-provider-qp-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    assert(fd >= 0 && !ftruncate(fd, sizeof(struct roce_native_work)));
    struct roce_native_work *work = mmap(NULL, sizeof(*work), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(work != MAP_FAILED);
    work->config = (struct roce_native_queue_config) {
        .version = ROCE_NATIVE_QUEUE_VERSION, .qps = 1, .workers = 1, .writes = 1, .write_size = 32,
        .local_qpn = qpn == 2 ? 3 : 2, .peer_qpn = 4000, .local_psn = 5000, .peer_psn = 6000,
        .pmtu = 1024, .local_lkey = mr->lkey, .local_ipv4 = 0xc0000201, .peer_ipv4 = 0xc0000202,
        .local_address = iova, .flags = ROCE_NATIVE_QUEUE_RECEIVE_ONLY,
    };
    assert(roce_doorbell_init(&work->doorbells[0]));
    assert(!fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL));
    assert(bind_queues(context, fd) == EINVAL);
    work->config.local_qpn = qpn;
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    struct ibv_mr *wrong_pd = ibv_reg_mr_iova(other, mr->addr, mr->length, iova, access);
    assert(wrong_pd && wrong_pd->lkey != mr->lkey);
    work->config.local_lkey = wrong_pd->lkey;
    assert(bind_queues(context, fd) == EACCES);
    assert(!ibv_dereg_mr(wrong_pd) && !ibv_dealloc_pd(other));
    work->config.local_lkey = mr->lkey;

    attr = (struct ibv_qp_attr) {
        .qp_state = IBV_QPS_RTR, .path_mtu = IBV_MTU_1024,
        .dest_qp_num = work->config.peer_qpn, .rq_psn = work->config.peer_psn,
        .ah_attr = { .port_num = 1, .is_global = 1, .grh.dgid.raw = { [10] = 0xff, 0xff, 192, 0, 2, 2 } },
    };
    int rtr = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
              IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    assert(ibv_modify_qp(qp, &attr, rtr) == EAGAIN);
    assert(!bind_queues(context, fd) && qp->qp_num == qpn);
    assert(ibv_dereg_mr(mr) == EBUSY);
    assert(!ibv_modify_qp(qp, &attr, rtr));
    attr = (struct ibv_qp_attr) { .qp_state = IBV_QPS_RTS, .sq_psn = work->config.local_psn };
    int rts = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
              IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    assert(!ibv_modify_qp(qp, &attr, rts));
    struct ibv_sge sge = { .addr = iova, .length = work->config.write_size, .lkey = mr->lkey };
    struct ibv_send_wr wr = { .sg_list = &sge, .num_sge = 1, .opcode = IBV_WR_RDMA_WRITE,
                              .send_flags = IBV_SEND_SIGNALED };
    struct ibv_send_wr *bad = NULL;
    assert(ibv_post_send(qp, &wr, &bad) == EOPNOTSUPP && bad == &wr);
    assert(!atomic_load(&work->qps[0].posted) && !ibv_poll_cq(cq, 1, &wc));
    assert(!ibv_destroy_qp(qp) && !ibv_destroy_cq(cq));
    assert(!munmap(work, sizeof(*work)) && !close(fd) && !dlclose(provider));
}

static void check_wc(const struct ibv_wc *wc, const struct ibv_qp *qp, uint64_t id,
                     enum ibv_wc_status status)
{
    assert(wc->qp_num == qp->qp_num && wc->wr_id == id && wc->status == status);
    if (status == IBV_WC_SUCCESS)
        assert(wc->opcode == IBV_WC_RDMA_WRITE && wc->byte_len == 32);
}

static struct roce_native_work *map_test_work(const struct roce_native_queue_config *config, int *fd)
{
    size_t size = roce_native_work_size(config->version);
    *fd = memfd_create("roce-provider-work", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    assert(*fd >= 0 && size && !ftruncate(*fd, (off_t)size));
    struct roce_native_work *work = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
    assert(work != MAP_FAILED);
    work->config = *config;
    assert(roce_doorbell_init(&work->doorbells[0]));
    assert(!fcntl(*fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL));
    return work;
}

static void check_two_sge_queue(struct ibv_pd *pd, const struct ibv_mr *mr, uint64_t iova)
{
    struct ibv_context *context = pd->context;
    const struct roce_native_queue_config config = {
        .version = ROCE_NATIVE_SGE_QUEUE_VERSION, .qps = 1, .workers = 1, .writes = 2,
        .write_size = 32, .local_qpn = 3100, .peer_qpn = 4200, .local_psn = 5001, .peer_psn = 6001,
        .pmtu = 1024, .local_lkey = mr->lkey, .remote_rkey = 42,
        .local_ipv4 = 0xc0000201, .peer_ipv4 = 0xc0000202, .local_address = iova, .remote_address = 0x100000,
    };
    int fd;
    struct roce_native_work *work = map_test_work(&config, &fd);
    work->config.version = ROCE_NATIVE_QUEUE_VERSION;
    assert(roce_native_bind_work(context, fd) == EINVAL);
    work->config.version = config.version;
    assert(!roce_native_bind_work(context, fd));

    struct ibv_device_attr limits;
    assert(!ibv_query_device(context, &limits) && limits.max_sge == ROCE_NATIVE_MAX_SEND_SGES);
    struct ibv_cq *cq = ibv_create_cq(context, 2, NULL, NULL, 0);
    assert(cq);
    struct ibv_qp_init_attr init = {
        .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 2, .max_send_sge = ROCE_NATIVE_MAX_SEND_SGES + 1 },
    };
    assert(!ibv_create_qp(pd, &init) && errno == EOPNOTSUPP);
    init.cap.max_send_sge = ROCE_NATIVE_MAX_SEND_SGES;
    struct ibv_qp *qp = ibv_create_qp(pd, &init);
    assert(qp && !roce_native_init_qp(qp, false) && !roce_native_connect_qp(qp, &config, 0));
    struct ibv_sge segments[] = {
        {.addr = iova + 40, .length = 13, .lkey = mr->lkey},
        {.addr = iova + 1, .length = 19, .lkey = mr->lkey},
        {.addr = iova + 16, .length = 32, .lkey = mr->lkey},
    };
    struct ibv_send_wr writes[] = {
        {.wr_id = 1, .sg_list = segments, .num_sge = 2, .opcode = IBV_WR_RDMA_WRITE,
         .send_flags = IBV_SEND_SIGNALED, .wr.rdma = { .remote_addr = config.remote_address, .rkey = 42 }},
        {.wr_id = 2, .sg_list = &segments[2], .num_sge = 1, .opcode = IBV_WR_RDMA_WRITE,
         .send_flags = IBV_SEND_SIGNALED, .wr.rdma = { .remote_addr = config.remote_address + 32, .rkey = 42 }},
    };
    struct ibv_send_wr *bad;
    segments[1].lkey ^= 1;
    assert(ibv_post_send(qp, writes, &bad) == EINVAL && bad == writes);
    segments[1].lkey = mr->lkey;
    segments[1].addr = iova + 64;
    assert(ibv_post_send(qp, writes, &bad) == EINVAL && bad == writes);
    assert(!atomic_load(&work->qps[0].posted));
    segments[1].addr = iova + 1;
    writes[0].next = &writes[1];
    assert(!ibv_post_send(qp, writes, &bad) && !bad);
    const struct roce_native_sge *tail = ((struct roce_native_sge_work *)work)->second[0];
    assert(tail[0].address == segments[1].addr && tail[0].length == segments[1].length && tail[0].lkey == mr->lkey);
    assert(!tail[1].length && work->qps[0].entries[0].length == 32 && work->qps[0].entries[1].length == 32);
    struct ibv_wc wc[2];
    assert(!ibv_poll_cq(cq, 2, wc));
    atomic_store_explicit(&work->qps[0].retired, 2, memory_order_release);
    assert(ibv_poll_cq(cq, 2, wc) == 2);
    check_wc(&wc[0], qp, 1, IBV_WC_SUCCESS);
    check_wc(&wc[1], qp, 2, IBV_WC_SUCCESS);
    assert(!ibv_destroy_qp(qp) && !ibv_destroy_cq(cq));
    assert(!munmap(work, roce_native_work_size(config.version)) && !close(fd));
}

static void check_multi_mr_queue(int broker, struct ibv_pd *pd, const struct ibv_mr *anchor, uint64_t iova)
{
    size_t length = (size_t)sysconf(_SC_PAGESIZE);
    void *data = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(data != MAP_FAILED);
    struct ibv_pd *foreign_pd = ibv_alloc_pd(pd->context);
    assert(foreign_pd);
    struct ibv_mr *secondary = ibv_reg_mr(pd, data, length, 0);
    struct ibv_mr *foreign = ibv_reg_mr(foreign_pd, data, length, 0);
    assert(secondary && foreign);
    share_keys(broker, anchor, iova);
    share_keys(broker, secondary, (uintptr_t)data);
    share_keys(broker, foreign, (uintptr_t)data);
    expect(broker, 'M');

    const struct roce_native_queue_config config = {
        .version = ROCE_NATIVE_SGE_QUEUE_VERSION, .qps = 1, .workers = 1, .writes = 1,
        .write_size = 32, .local_qpn = 3100, .peer_qpn = 4200, .local_psn = 5001, .peer_psn = 6001,
        .pmtu = 1024, .local_lkey = anchor->lkey, .remote_rkey = 42, .flags = ROCE_NATIVE_QUEUE_MULTI_MR,
        .local_ipv4 = 0xc0000201, .peer_ipv4 = 0xc0000202, .local_address = iova, .remote_address = 0x100000,
    };
    int fd;
    struct roce_native_work *work = map_test_work(&config, &fd);
    assert(!roce_native_bind_work(pd->context, fd));
    struct ibv_cq *cq = ibv_create_cq(pd->context, 1, NULL, NULL, 0);
    assert(cq);
    struct ibv_qp_init_attr init = {
        .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 1, .max_send_sge = 2 },
    };
    struct ibv_qp *qp = ibv_create_qp(pd, &init);
    assert(qp && !roce_native_init_qp(qp, false) && !roce_native_connect_qp(qp, &config, 0));
    struct ibv_sge segments[] = {
        { .addr = (uintptr_t)data, .length = 13, .lkey = foreign->lkey },
        { .addr = iova, .length = 19, .lkey = anchor->lkey },
    };
    struct ibv_send_wr write = {
        .wr_id = 1, .sg_list = segments, .num_sge = 2, .opcode = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED, .wr.rdma = { .remote_addr = config.remote_address, .rkey = 42 },
    };
    struct ibv_send_wr *bad;
    assert(ibv_post_send(qp, &write, &bad) == EINVAL && bad == &write);
    segments[0].lkey = secondary->lkey;
    segments[0].addr += length - 1;
    assert(ibv_post_send(qp, &write, &bad) == EINVAL && bad == &write);
    segments[0].addr = (uintptr_t)data;
    assert(!ibv_post_send(qp, &write, &bad) && !bad && atomic_load(&work->qps[0].posted) == 1);
    assert(work->qps[0].entries[0].lkey == secondary->lkey);
    assert(((struct roce_native_sge_work *)work)->second[0][0].lkey == anchor->lkey);
    assert(ibv_dereg_mr(secondary) == EBUSY);
    send_value(broker, "M", 1);
    expect(broker, 'R');
    assert(ibv_dereg_mr(secondary) == EBUSY);
    atomic_store_explicit(&work->qps[0].retired, 1, memory_order_release);
    struct ibv_wc wc;
    assert(ibv_poll_cq(cq, 1, &wc) == 1);
    check_wc(&wc, qp, 1, IBV_WC_SUCCESS);
    assert(!ibv_dereg_mr(secondary) && !ibv_dereg_mr(foreign) && !ibv_dealloc_pd(foreign_pd));
    assert(!ibv_destroy_qp(qp) && !ibv_destroy_cq(cq));
    assert(!munmap(work, roce_native_work_size(config.version)) && !close(fd) && !munmap(data, length));
}

static void check_shared_cq(void *buffer, size_t length, uint64_t iova)
{
    enum { QPS = 3, WRITES = 3, WRITE_SIZE = 32 };
    struct ibv_context *context = roce_native_open_device();
    assert(context);
    struct ibv_pd *pd = ibv_alloc_pd(context);
    assert(pd);
    struct ibv_mr *mr = ibv_reg_mr_iova(pd, buffer, length, iova, 0);
    assert(mr);
    struct roce_native_queue_config config = {
        .version = ROCE_NATIVE_QUEUE_VERSION, .qps = QPS, .workers = 1, .writes = WRITES,
        .write_size = WRITE_SIZE, .local_qpn = 3000, .peer_qpn = 4000, .local_psn = 5000, .peer_psn = 6000,
        .pmtu = 1024, .local_lkey = mr->lkey, .remote_rkey = 42,
        .local_ipv4 = 0xc0000201, .peer_ipv4 = 0xc0000202, .local_address = iova, .remote_address = 0x100000,
    };
    int fd;
    struct roce_native_work *work = map_test_work(&config, &fd);
    assert(!roce_native_bind_work(context, fd));
    struct ibv_device_attr limits;
    assert(!ibv_query_device(context, &limits) && limits.max_cqe == QPS * ROCE_NATIVE_QUEUE_DEPTH);
    struct ibv_cq *large = ibv_create_cq(context, limits.max_cqe, NULL, NULL, 0);
    assert(large && !ibv_destroy_cq(large));

    struct ibv_cq *send_cq = ibv_create_cq(context, 4, NULL, NULL, 0);
    struct ibv_cq *recv_cq = ibv_create_cq(context, 1, NULL, NULL, 0);
    struct ibv_cq *spare_cq = ibv_create_cq(context, 1, NULL, NULL, 0);
    assert(send_cq && recv_cq && spare_cq);
    struct ibv_qp *qps[QPS];
    struct ibv_send_wr writes[QPS][WRITES];
    struct ibv_sge segments[QPS][WRITES];
    for (unsigned q = 0; q < QPS; ++q) {
        struct ibv_qp_init_attr init = {
            .send_cq = send_cq, .recv_cq = q ? recv_cq : send_cq, .qp_type = IBV_QPT_RC,
            .cap = { .max_send_wr = WRITES, .max_send_sge = 1 },
            .sq_sig_all = q == 2,
        };
        qps[q] = ibv_create_qp(pd, &init);
        assert(qps[q] && !roce_native_init_qp(qps[q], false));
        assert(!roce_native_connect_qp(qps[q], &work->config, q));
        for (unsigned i = 0; i < WRITES; ++i) {
            uint64_t offset = (q * WRITES + i) * WRITE_SIZE;
            segments[q][i] = (struct ibv_sge) { .addr = iova + offset, .length = WRITE_SIZE, .lkey = mr->lkey };
            writes[q][i] = (struct ibv_send_wr) {
                .wr_id = i + 1, .next = i + 1 < WRITES ? &writes[q][i + 1] : NULL,
                .sg_list = &segments[q][i], .num_sge = 1, .opcode = IBV_WR_RDMA_WRITE,
                .send_flags = !q && i == 1 ? IBV_SEND_SIGNALED : 0,
                .wr.rdma = { .remote_addr = work->config.remote_address + offset, .rkey = work->config.remote_rkey },
            };
        }
    }

    struct ibv_send_wr *bad;
    config.local_qpn += QPS;
    config.qps = 2;
    config.writes = 1;
    int next_fd;
    struct roce_native_work *next = map_test_work(&config, &next_fd);
    assert(roce_native_bind_work(context, next_fd) == EBUSY);

    segments[0][0].addr = iova + QPS * WRITES * WRITE_SIZE - WRITE_SIZE + 1;
    assert(ibv_post_send(qps[0], writes[0], &bad) == EINVAL && bad == &writes[0][0]);
    segments[0][0].addr = iova + 2 * WRITE_SIZE;
    writes[0][0].wr.rdma.remote_addr = work->config.remote_address + WRITES * WRITE_SIZE;
    assert(ibv_post_send(qps[0], writes[0], &bad) == EINVAL && bad == &writes[0][0]);
    assert(atomic_load(&work->qps[0].posted) == 0);
    writes[0][0].wr.rdma.remote_addr = work->config.remote_address + 2 * WRITE_SIZE;
    segments[0][2].addr = iova;
    writes[0][2].wr.rdma.remote_addr = work->config.remote_address;

    writes[0][1].next = NULL;
    assert(!ibv_post_send(qps[0], writes[0], &bad) && !bad);
    assert(work->qps[0].entries[0].local_address == segments[0][0].addr &&
           work->qps[0].entries[0].remote_address == writes[0][0].wr.rdma.remote_addr);
    assert(ibv_post_send(qps[1], writes[1], &bad) == ENOMEM && bad == &writes[1][2]);
    assert(atomic_load(&work->qps[0].posted) == 2 && atomic_load(&work->qps[1].posted) == 2);
    assert(ibv_post_send(qps[0], &writes[0][2], &bad) == ENOMEM && bad == &writes[0][2]);
    assert(ibv_destroy_qp(qps[0]) == EBUSY && ibv_destroy_cq(send_cq) == EBUSY &&
           ibv_destroy_cq(recv_cq) == EBUSY && ibv_dereg_mr(mr) == EBUSY);
    struct ibv_wc wc[2];
    assert(!ibv_poll_cq(send_cq, 2, wc));

    /* Reclaim a successful unsignaled prefix while returning zero WCs. */
    atomic_store_explicit(&work->qps[0].retired, 1, memory_order_release);
    assert(!ibv_poll_cq(send_cq, 1, wc));
    writes[2][0].next = NULL;
    assert(!ibv_post_send(qps[2], writes[2], &bad) && !bad);
    assert(ibv_post_send(qps[0], &writes[0][2], &bad) == ENOMEM && bad == &writes[0][2]);

    atomic_store_explicit(&work->qps[0].retired, 2, memory_order_release);
    work->qps[1].status[0] = ROCE_NATIVE_WC_REMOTE_ACCESS;
    work->qps[1].status[1] = ROCE_NATIVE_WC_FLUSHED;
    atomic_store_explicit(&work->qps[1].retired, 2, memory_order_release);
    assert(ibv_destroy_qp(qps[1]) == EBUSY);
    assert(ibv_poll_cq(send_cq, 1, wc) == 1);
    check_wc(&wc[0], qps[0], 2, IBV_WC_SUCCESS);

    /* Destroy may reclaim a fully retired tail that has no visible WC. */
    assert(!ibv_post_send(qps[0], &writes[0][2], &bad) && !bad);
    assert(ibv_destroy_qp(qps[0]) == EBUSY);
    atomic_store_explicit(&work->qps[0].retired, 3, memory_order_release);
    assert(!ibv_destroy_qp(qps[0]) && ibv_destroy_cq(send_cq) == EBUSY && ibv_dereg_mr(mr) == EBUSY);
    writes[2][1].next = NULL;
    assert(!ibv_post_send(qps[2], &writes[2][1], &bad) && !bad);
    atomic_store_explicit(&work->qps[2].retired, 2, memory_order_release);

    assert(ibv_poll_cq(send_cq, 1, wc) == 1);
    check_wc(&wc[0], qps[1], 1, IBV_WC_REM_ACCESS_ERR);
    assert(ibv_poll_cq(send_cq, 2, wc) == 2);
    check_wc(&wc[0], qps[2], 1, IBV_WC_SUCCESS);
    check_wc(&wc[1], qps[2], 2, IBV_WC_SUCCESS);
    assert(!ibv_poll_cq(recv_cq, 1, wc) && ibv_poll_cq(send_cq, 1, wc) == 1);
    check_wc(&wc[0], qps[1], 2, IBV_WC_WR_FLUSH_ERR);
    assert(!ibv_poll_cq(send_cq, 2, wc));
    assert(!ibv_destroy_qp(qps[1]) && ibv_destroy_cq(recv_cq) == EBUSY);
    assert(!ibv_destroy_qp(qps[2]));
    assert(!ibv_create_cq(context, 1, NULL, NULL, 0) && errno == ENOSPC);
    assert(!ibv_destroy_cq(spare_cq));
    assert(!roce_native_bind_work(context, next_fd));

    struct ibv_qp_init_attr next_init = {
        .send_cq = send_cq, .recv_cq = recv_cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 1, .max_send_sge = 1 }, .sq_sig_all = 1,
    };
    struct ibv_qp *next_qp = ibv_create_qp(pd, &next_init);
    assert(next_qp && next_qp->qp_num == config.local_qpn);
    assert(!roce_native_init_qp(next_qp, false) && !roce_native_connect_qp(next_qp, &config, 0));
    writes[0][2].next = NULL;
    assert(!ibv_post_send(next_qp, &writes[0][2], &bad) && !bad);
    assert(atomic_load(&work->qps[0].posted) == 3 && atomic_load(&next->qps[0].posted) == 1);
    atomic_store_explicit(&next->qps[0].retired, 1, memory_order_release);
    assert(ibv_poll_cq(send_cq, 1, wc) == 1);
    check_wc(&wc[0], next_qp, 3, IBV_WC_SUCCESS);
    assert(!ibv_destroy_qp(next_qp));
    assert(roce_native_bind_work(context, next_fd) == EBUSY);

    /* A fresh descriptor can replace a binding with unused slots. */
    config.local_qpn += config.qps;
    config.qps = 1;
    int final_fd;
    struct roce_native_work *final = map_test_work(&config, &final_fd);
    assert(!roce_native_bind_work(context, final_fd));
    struct ibv_qp *final_qp = ibv_create_qp(pd, &next_init);
    assert(final_qp && final_qp->qp_num == config.local_qpn);
    assert(!ibv_destroy_qp(final_qp) && !ibv_destroy_cq(send_cq) && !ibv_destroy_cq(recv_cq));
    assert(!ibv_dereg_mr(mr) && !ibv_dealloc_pd(pd) && !ibv_close_device(context));
    assert(!munmap(final, sizeof(*final)) && !close(final_fd));
    assert(!munmap(next, sizeof(*next)) && !close(next_fd));
    assert(!munmap(work, sizeof(*work)) && !close(fd));
}

static void check_scoped_write_client(int fd, struct ibv_pd *pd, const struct ibv_mr *anchor, uint64_t anchor_iova)
{
    size_t length = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *bytes = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(bytes != MAP_FAILED);
    struct ibv_pd *foreign_pd = ibv_alloc_pd(pd->context);
    assert(foreign_pd);
    uint64_t iova = UINT64_C(0x2100000);
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    struct ibv_mr *related = ibv_reg_mr_iova(pd, bytes, length, iova, access);
    struct ibv_mr *foreign = ibv_reg_mr_iova(foreign_pd, bytes, length, iova, access);
    struct ibv_mr *readonly = ibv_reg_mr_iova(pd, bytes, length, iova, 0);
    assert(related && foreign && readonly && !readonly->rkey);
    share_keys(fd, anchor, anchor_iova);
    share_keys(fd, related, iova);
    share_keys(fd, foreign, iova);
    share_keys(fd, readonly, iova);

    expect(fd, 'B');
    assert(ibv_dereg_mr(related) == EBUSY);
    assert(bytes[length - 2] == 0x36 && bytes[length - 1] == 0x47);
    send_value(fd, "B", 1);
    expect(fd, 'R');
    assert(!ibv_dereg_mr(related) && !ibv_dereg_mr(foreign) && !ibv_dereg_mr(readonly));
    assert(!ibv_dealloc_pd(foreign_pd) && !munmap(bytes, length));
    send_value(fd, "R", 1);
}

static void client(int fd)
{
    assert(!setenv("ROCE_DPDK_MR_PD_SCOPE", "1", 1));
    struct ibv_device **devices = ibv_get_device_list(NULL);
    assert(devices);
    struct ibv_context *context = NULL;
    for (size_t i = 0; devices[i]; ++i)
        if (!strcmp(ibv_get_device_name(devices[i]), "roce_dpdk0")) {
            context = ibv_open_device(devices[i]);
            break;
        }
    ibv_free_device_list(devices);
    assert(context);
    struct ibv_device_attr attr;
    assert(ibv_query_device(context, &attr) == 0 && attr.max_mr == ROCE_MR_IPC_MAX_REGISTRATIONS);
    struct ibv_pd *pd = ibv_alloc_pd(context);
    assert(pd);
    long page = sysconf(_SC_PAGESIZE);
    assert(page > 0);
    uint8_t *pages = mmap(NULL, 2 * (size_t)page, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(pages != MAP_FAILED);
    uint8_t *buffer = pages + 13;
    size_t length = (size_t)page + 17;
    memset(buffer, 0xa5, length);
    uint64_t iova = UINT64_C(0x200000d);
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    check_shared_cq(buffer, length, iova);
    assert(!ibv_reg_mr_iova(pd, buffer, length, iova + 1, access) && errno == EINVAL);
    assert(!ibv_reg_mr(pd, buffer, length, IBV_ACCESS_REMOTE_WRITE) && errno == EINVAL);
    struct ibv_mr *mr = ibv_reg_mr_iova(pd, buffer, length, iova, access);
    assert(mr && mr->addr == buffer && mr->lkey && mr->rkey && mr->lkey != mr->rkey);
    check_early_qp(pd, mr, iova);
    check_early_qp(pd, mr, iova);
    check_two_sge_queue(pd, mr, iova);
    check_multi_mr_queue(fd, pd, mr, iova);
    check_scoped_write_client(fd, pd, mr, iova);
    share_keys(fd, mr, iova);

    expect(fd, 'B');
    assert(ibv_dereg_mr(mr) == EBUSY && ibv_dealloc_pd(pd) == EBUSY);
    assert(buffer[0] == 0x3b && buffer[length - 1] == 0x4c);
    send_value(fd, "B", 1);
    expect(fd, 'R');
    assert(ibv_dereg_mr(mr) == 0);
    send_value(fd, "R", 1);

    mr = ibv_reg_mr(pd, buffer + 1, length - 1, 0);
    assert(mr && mr->lkey && !mr->rkey);
    share_keys(fd, mr, (uintptr_t)(buffer + 1));
    expect(fd, 'X');
    _exit(0);
}

static void check_scoped_write_worker(struct roce_mr_server *server, int fd, pid_t child)
{
    struct mr_keys primary, related, foreign, readonly;
    recv_value(fd, &primary, sizeof(primary));
    recv_value(fd, &related, sizeof(related));
    recv_value(fd, &foreign, sizeof(foreign));
    recv_value(fd, &readonly, sizeof(readonly));

    struct roce_mr_lease anchor = {0};
    assert(!roce_mr_acquire(server, child, primary.lkey, ROCE_MR_IPC_REMOTE_WRITE, &anchor));
    struct roce_mr_destination destination = { .anchor = &anchor };
    assert(roce_mr_resolve_write(&destination, primary.rkey, primary.iova + primary.length - 1, 1) ==
           anchor.buffer + primary.length - 1);
    roce_mr_release_write(&destination);
    assert(!roce_mr_resolve_write(&destination, primary.rkey, primary.iova - 1, 1));
    assert(!roce_mr_resolve_write(&destination, primary.rkey, primary.iova + primary.length - 1, 2));
    assert(!roce_mr_resolve_write(&destination, related.rkey, UINT64_MAX, 2));
    assert(!roce_mr_resolve_write(&destination, related.rkey, related.iova + related.length - 1, 2));
    assert(!roce_mr_resolve_write(&destination, foreign.rkey, foreign.iova, 1));
    assert(!roce_mr_resolve_write(&destination, readonly.rkey, readonly.iova, 1));
    assert(!roce_mr_resolve_write(&destination, readonly.lkey, readonly.iova, 1));
    assert(!destination.active.server);

    uint8_t *target = roce_mr_resolve_write(&destination, related.rkey, related.iova + related.length - 2, 2);
    assert(target);
    assert(!roce_mr_resolve_write(&destination, primary.rkey, primary.iova, 1));
    target[0] = 0x36;
    target[1] = 0x47;
    send_value(fd, "B", 1);
    expect(fd, 'B');
    roce_mr_release_write(&destination);

    send_value(fd, "R", 1);
    expect(fd, 'R');
    assert(!roce_mr_resolve_write(&destination, related.rkey, related.iova, 1));
    assert(roce_mr_resolve_write(&destination, primary.rkey, primary.iova, 1) == anchor.buffer);
    roce_mr_release_write(&destination);
    roce_mr_release(&anchor);
}

static void check(struct roce_mr_server *server, int fd, pid_t child)
{
    struct mr_keys related[3];
    for (size_t i = 0; i < sizeof(related) / sizeof(*related); ++i)
        recv_value(fd, &related[i], sizeof(related[i]));
    struct roce_mr_lease anchor = {0}, secondary = {0}, denied = {0};
    assert(!roce_mr_acquire(server, child, related[0].lkey, 0, &anchor) && anchor.scope);
    assert(roce_mr_acquire_related(&anchor, related[2].lkey, 0, &denied) == EACCES);
    assert(roce_mr_acquire_related(&anchor, related[1].lkey, ROCE_MR_IPC_WRITE, &denied) == EACCES);
    assert(!roce_mr_acquire_related(&anchor, related[1].lkey, 0, &secondary));
    assert(secondary.scope == anchor.scope && secondary.iova == related[1].iova && secondary.lkey != anchor.lkey);
    send_value(fd, "M", 1);
    expect(fd, 'M');
    roce_mr_release(&secondary);
    roce_mr_release(&anchor);
    send_value(fd, "R", 1);

    check_scoped_write_worker(server, fd, child);

    struct mr_keys keys;
    struct roce_mr_lease lease = {0};
    recv_value(fd, &keys, sizeof(keys));
    assert(roce_mr_acquire(server, getpid(), keys.lkey, 0, &lease) == ENOENT);
    assert(roce_mr_acquire(server, child, keys.lkey, ROCE_MR_IPC_REMOTE_WRITE, &lease) == 0);
    assert(lease.iova == keys.iova && lease.length == keys.length && lease.rkey == keys.rkey);
    int dmabuf = roce_mr_export_dmabuf(&lease);
    assert(dmabuf >= 0 && fcntl(dmabuf, F_GETFD) == FD_CLOEXEC);
    assert(lseek(dmabuf, 0, SEEK_END) == (off_t)lease.mapped_length);
    assert(close(dmabuf) == 0);
    assert(lease.buffer[0] == 0xa5 && lease.buffer[lease.length - 1] == 0xa5);
    lease.buffer[0] = 0x3b;
    lease.buffer[lease.length - 1] = 0x4c;
    assert(roce_mr_server_stop(server) == EBUSY);
    send_value(fd, "B", 1);
    expect(fd, 'B');
    roce_mr_release(&lease);
    send_value(fd, "R", 1);
    expect(fd, 'R');
    assert(roce_mr_acquire(server, child, keys.lkey, 0, &lease) == ENOENT);

    recv_value(fd, &keys, sizeof(keys));
    assert(roce_mr_acquire(server, child, keys.lkey, ROCE_MR_IPC_WRITE, &lease) == EACCES);
    assert(roce_mr_acquire(server, child, keys.lkey, 0, &lease) == 0);
    send_value(fd, "X", 1);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    struct roce_mr_lease late = {0};
    int error;
    while ((error = roce_mr_acquire(server, child, keys.lkey, 0, &late)) == 0) {
        roce_mr_release(&late);
        usleep(1000);
    }
    assert(error == ENOENT);
    assert(lease.buffer[0] == 0xa5 && lease.buffer[lease.length - 1] == 0x4c);
    assert(roce_mr_server_stop(server) == EBUSY);
    assert(roce_mr_wait_deregistered(server, keys.lkey, 0) == ETIMEDOUT);
    roce_mr_release(&lease);
    assert(!roce_mr_wait_deregistered(server, keys.lkey, 0));
}

int main(int argc, char **argv)
{
    alarm(15);
    if (argc == 3 && !strcmp(argv[1], "--client")) {
        client(atoi(argv[2]));
        return 1;
    }
    char directory[] = "/tmp/roce-verbs-mr.XXXXXX";
    assert(mkdtemp(directory));
    char path[sizeof(directory) + sizeof("/broker")];
    snprintf(path, sizeof(path), "%s/broker", directory);
    assert(setenv("ROCE_DPDK_MR_SOCKET", path, 1) == 0);
    struct roce_mr_server *server = roce_mr_server_start(path);
    assert(server);
    int channel[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, channel) == 0);
    char client_fd[24];
    snprintf(client_fd, sizeof(client_fd), "%d", channel[1]);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(channel[0]);
        execl("/proc/self/exe", "check_provider_mr", "--client", client_fd, (char *)NULL);
        _exit(127);
    }
    close(channel[1]);
    check(server, channel[0], child);
    close(channel[0]);
    assert(roce_mr_server_stop(server) == 0);
    assert(rmdir(directory) == 0);
    puts("libibverbs MR registration, access, deregistration and worker leases verified");
    puts("Scoped remote WRITE placement, range protection and destination leases verified");
    puts("Early QP lifetime, protected binding and receive-only queues verified");
    puts("Shared CQ credits, QP completion identity and resource lifetimes verified");
    puts("Selective WRITE signaling and unsignaled error completions verified");
    puts("Reused the verbs context, PD, MR and CQs across native queue sessions");
    return 0;
}
