#define _GNU_SOURCE
#include "../src/roce_mr_ipc.h"
#include "../src/roce_mr_server.h"
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

static void client(int fd)
{
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
    assert(!ibv_reg_mr_iova(pd, buffer, length, iova + 1, access) && errno == EINVAL);
    assert(!ibv_reg_mr(pd, buffer, length, IBV_ACCESS_REMOTE_WRITE) && errno == EINVAL);
    struct ibv_mr *mr = ibv_reg_mr_iova(pd, buffer, length, iova, access);
    assert(mr && mr->addr == buffer && mr->lkey && mr->rkey && mr->lkey != mr->rkey);
    check_early_qp(pd, mr, iova);
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

static void check(struct roce_mr_server *server, int fd, pid_t child)
{
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
    roce_mr_release(&lease);
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
    puts("Early QP lifetime, protected binding and receive-only queues verified");
    return 0;
}
