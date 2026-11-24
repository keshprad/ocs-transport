#include <infiniband/driver.h>

#include <stdlib.h>
#include <string.h>

_Static_assert(IBVERBS_PABI_VERSION == 34, "Use libibverbs provider ABI 34");

struct roce_verbs_context {
    struct verbs_context verbs;
};

static int query_device(struct ibv_context *context, const struct ibv_query_device_ex_input *input,
                        struct ibv_device_attr_ex *attributes, size_t length)
{
    struct ib_uverbs_ex_query_device_resp response = {0};
    size_t capacity = sizeof(response);
    int error = ibv_cmd_query_device_any(context, input, attributes, length, &response, &capacity);
    if (!error)
        memset(attributes->orig_attr.fw_ver, 0, sizeof(attributes->orig_attr.fw_ver));
    return error;
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
    struct ibv_pd *pd = calloc(1, sizeof(*pd));
    if (!pd)
        return NULL;
    int error = ibv_cmd_alloc_pd(context, pd, &command, sizeof(command), &response, sizeof(response));
    if (error) {
        free(pd);
        errno = error;
        return NULL;
    }
    return pd;
}

static int dealloc_pd(struct ibv_pd *pd)
{
    int error = ibv_cmd_dealloc_pd(pd);
    if (!error)
        free(pd);
    return error;
}

static void free_context(struct ibv_context *ibv)
{
    struct roce_verbs_context *context = container_of(verbs_get_ctx(ibv), struct roce_verbs_context, verbs);
    verbs_uninit_context(&context->verbs);
    free(context);
}

static const struct verbs_context_ops context_ops = {
    .query_device_ex = query_device,
    .query_port = query_port,
    .alloc_pd = alloc_pd,
    .dealloc_pd = dealloc_pd,
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
