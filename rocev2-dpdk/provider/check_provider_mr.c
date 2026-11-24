#define _GNU_SOURCE
#include "../src/roce_mr_ipc.h"
#include "../src/roce_mr_server.h"

#include <assert.h>
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
    return 0;
}
