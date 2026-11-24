#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <infiniband/driver.h>

#include "../kernel/roce_mr_uapi.h"
#include "../src/roce_mr_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

_Static_assert(IBVERBS_PABI_VERSION == 34, "Use libibverbs provider ABI 34");

struct roce_verbs_context {
    struct verbs_context verbs;
    char *mr_socket;
};

struct roce_verbs_pd {
    struct ibv_pd ibv;
    atomic_size_t mr_count;
};

struct roce_verbs_mr {
    struct verbs_mr verbs;
    int socket;
    bool dereg_pending;
};

static int query_device(struct ibv_context *context, const struct ibv_query_device_ex_input *input,
                        struct ibv_device_attr_ex *attributes, size_t length)
{
    struct ib_uverbs_ex_query_device_resp response = {0};
    size_t capacity = sizeof(response);
    int error = ibv_cmd_query_device_any(context, input, attributes, length, &response, &capacity);
    if (error)
        return error;
    memset(attributes->orig_attr.fw_ver, 0, sizeof(attributes->orig_attr.fw_ver));
    const struct roce_verbs_context *provider =
        container_of(verbs_get_ctx(context), const struct roce_verbs_context, verbs);
    long page_size = sysconf(_SC_PAGESIZE);
    if (provider->mr_socket && page_size > 0) {
        attributes->orig_attr.max_mr = ROCE_MR_IPC_MAX_REGISTRATIONS;
        attributes->orig_attr.max_mr_size = (uint64_t)INT_MAX * (uint64_t)page_size;
    }
    return 0;
}

static int query_port(struct ibv_context *context, uint8_t port, struct ibv_port_attr *attributes)
{
    struct ibv_query_port command = {0};
    return ibv_cmd_query_port(context, port, attributes, &command, sizeof(command));
}

static struct ibv_pd *alloc_pd(struct ibv_context *context)
{
    struct ibv_alloc_pd command = {0};
    struct ib_uverbs_alloc_pd_resp response = {0};
    struct roce_verbs_pd *pd = calloc(1, sizeof(*pd));
    if (!pd)
        return NULL;
    atomic_init(&pd->mr_count, 0);
    int error = ibv_cmd_alloc_pd(context, &pd->ibv, &command, sizeof(command), &response, sizeof(response));
    if (error) {
        free(pd);
        errno = error;
        return NULL;
    }
    return &pd->ibv;
}

static int dealloc_pd(struct ibv_pd *ibv)
{
    struct roce_verbs_pd *pd = container_of(ibv, struct roce_verbs_pd, ibv);
    if (atomic_load_explicit(&pd->mr_count, memory_order_acquire))
        return EBUSY;

    int error = ibv_cmd_dealloc_pd(ibv);
    if (!error)
        free(pd);
    return error;
}

static int mr_registration(void *address, size_t length, uint64_t iova, int access,
                           struct roce_mr_register *pin, struct roce_mr_request *request)
{
    if (access & ~(IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE))
        return EOPNOTSUPP;
    if ((access & IBV_ACCESS_REMOTE_WRITE) && !(access & IBV_ACCESS_LOCAL_WRITE))
        return EINVAL;

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || !address || !length)
        return EINVAL;

    uintptr_t start = (uintptr_t)address;
    size_t page = (size_t)page_size;
    size_t offset = start % page;
    if (iova % page != offset || length > SIZE_MAX - offset)
        return EINVAL;

    size_t span = offset + length;
    size_t pad = (page - span % page) % page;
    uintptr_t base = start - offset;
    if (span > SIZE_MAX - pad || span + pad > UINTPTR_MAX - base || length - 1 > UINT64_MAX - iova)
        return EINVAL;

    *pin = (struct roce_mr_register) {
        .address = base,
        .length = span + pad,
        .access = (access & IBV_ACCESS_LOCAL_WRITE) ? ROCE_MR_ACCESS_LOCAL_WRITE : 0,
    };
    *request = (struct roce_mr_request) {
        .version = ROCE_MR_IPC_VERSION,
        .access = ((access & IBV_ACCESS_LOCAL_WRITE) ? ROCE_MR_IPC_WRITE : 0) |
                  ((access & IBV_ACCESS_REMOTE_WRITE) ? ROCE_MR_IPC_REMOTE_WRITE : 0),
        .iova = iova,
        .offset = offset,
        .length = length,
        .mapped_length = pin->length,
    };
    return 0;
}

static int pin_mapping(const struct roce_mr_register *request)
{
    int device = open(ROCE_MR_DEVICE, O_RDWR | O_CLOEXEC);
    if (device < 0)
        return -1;

    int fd = ioctl(device, ROCE_MR_REGISTER, request);
    int error = errno;
    close(device);
    errno = error;
    return fd;
}

static struct ibv_mr *reg_mr(struct ibv_pd *ibv, void *address, size_t length, uint64_t iova, int access)
{
    struct roce_verbs_context *context =
        container_of(verbs_get_ctx(ibv->context), struct roce_verbs_context, verbs);
    struct roce_verbs_pd *pd = container_of(ibv, struct roce_verbs_pd, ibv);
    struct roce_mr_register pin;
    struct roce_mr_request request;
    if (!context->mr_socket) {
        errno = EOPNOTSUPP;
        return NULL;
    }

    int error = mr_registration(address, length, iova, access, &pin, &request);
    if (error) {
        errno = error;
        return NULL;
    }

    struct roce_verbs_mr *mr = calloc(1, sizeof(*mr));
    if (!mr)
        return NULL;
    atomic_fetch_add_explicit(&pd->mr_count, 1, memory_order_relaxed);

    int mapping = pin_mapping(&pin);
    if (mapping < 0) {
        error = errno;
        goto free_mr;
    }

    mr->socket = roce_mr_ipc_connect(context->mr_socket);
    if (mr->socket < 0) {
        error = errno;
        close(mapping);
        goto free_mr;
    }
    error = roce_mr_ipc_send_registration(mr->socket, &request, mapping);
    close(mapping);
    if (error)
        goto close_socket;

    struct roce_mr_reply reply;
    error = roce_mr_ipc_read_reply(mr->socket, &reply);
    if (!error)
        error = reply.error;
    if (error)
        goto close_socket;

    mr->verbs.mr_type = IBV_MR_TYPE_MR;
    mr->verbs.access = access;
    mr->verbs.ibv_mr.lkey = reply.lkey;
    mr->verbs.ibv_mr.rkey = reply.rkey;
    return &mr->verbs.ibv_mr;

close_socket:
    close(mr->socket);
free_mr:
    atomic_fetch_sub_explicit(&pd->mr_count, 1, memory_order_release);
    free(mr);
    errno = error;
    return NULL;
}

static int dereg_mr(struct verbs_mr *verbs)
{
    struct roce_verbs_mr *mr = container_of(verbs, struct roce_verbs_mr, verbs);
    struct roce_verbs_pd *pd = container_of(verbs->ibv_mr.pd, struct roce_verbs_pd, ibv);
    /* A receive timeout leaves the original request pending on this channel. */
    if (!mr->dereg_pending) {
        const char command = ROCE_MR_IPC_DEREGISTER;
        ssize_t sent;
        do {
            sent = send(mr->socket, &command, sizeof(command), MSG_NOSIGNAL);
        } while (sent < 0 && errno == EINTR);
        if (sent < 0)
            return errno;
        if (sent != sizeof(command))
            return EIO;
        mr->dereg_pending = true;
    }

    struct roce_mr_reply reply;
    int error = roce_mr_ipc_read_reply(mr->socket, &reply);
    if (error)
        return error;
    mr->dereg_pending = false;
    if (reply.error)
        return reply.error;

    close(mr->socket);
    atomic_fetch_sub_explicit(&pd->mr_count, 1, memory_order_release);
    free(mr);
    return 0;
}

static void free_context(struct ibv_context *ibv)
{
    struct roce_verbs_context *context = container_of(verbs_get_ctx(ibv), struct roce_verbs_context, verbs);
    verbs_uninit_context(&context->verbs);
    free(context->mr_socket);
    free(context);
}

static const struct verbs_context_ops context_ops = {
    .query_device_ex = query_device,
    .query_port = query_port,
    .alloc_pd = alloc_pd,
    .dealloc_pd = dealloc_pd,
    .reg_mr = reg_mr,
    .dereg_mr = dereg_mr,
    .free_context = free_context,
};

static struct verbs_context *alloc_context(struct ibv_device *device, int command_fd,
                                           void *private_data)
{
    (void)private_data;
    struct roce_verbs_context *context =
        verbs_init_and_alloc_context(device, command_fd, context, verbs, RDMA_DRIVER_UNKNOWN);
    if (!context)
        return NULL;

    struct ibv_get_context command = {0};
    struct ib_uverbs_get_context_resp response = {0};
    int error = ibv_cmd_get_context(&context->verbs, &command, sizeof(command), &response, sizeof(response));
    if (error) {
        free_context(&context->verbs.context);
        errno = error;
        return NULL;
    }
    const char *socket = secure_getenv("ROCE_DPDK_MR_SOCKET");
    if (socket && *socket) {
        context->mr_socket = strdup(socket);
        if (!context->mr_socket) {
            error = errno;
            free_context(&context->verbs.context);
            errno = error;
            return NULL;
        }
    }
    verbs_set_ops(&context->verbs, &context_ops);
    return &context->verbs;
}

static bool match_device(struct verbs_sysfs_dev *device)
{
    char description[64];
    return device->match &&
           ibv_read_sysfs_file(device->ibdev_path, "node_desc", description, sizeof(description)) > 0 &&
           strcmp(description, "RoCEv2 DPDK") == 0;
}

static struct verbs_device *alloc_device(struct verbs_sysfs_dev *device)
{
    (void)device;
    return calloc(1, sizeof(struct verbs_device));
}

static void free_device(struct verbs_device *device)
{
    free(device);
}

static const struct verbs_match_ent match[] = {
    VERBS_NAME_MATCH("roce_dpdk", NULL),
    {},
};

static const struct verbs_device_ops device_ops = {
    .name = "roce_dpdk",
    .match_min_abi_version = 1,
    .match_max_abi_version = 1,
    .match_table = match,
    .match_device = match_device,
    .alloc_context = alloc_context,
    .alloc_device = alloc_device,
    .uninit_device = free_device,
};

PROVIDER_DRIVER(roce_dpdk, device_ops);
