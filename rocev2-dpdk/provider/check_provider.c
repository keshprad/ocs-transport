#include <err.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct ibv_device **devices = ibv_get_device_list(NULL);
    if (!devices)
        err(1, "enumerate RDMA devices");

    struct ibv_context *context = NULL;
    for (size_t i = 0; devices[i]; ++i) {
        if (strcmp(ibv_get_device_name(devices[i]), "roce_dpdk0") == 0) {
            context = ibv_open_device(devices[i]);
            if (!context)
                err(1, "open roce_dpdk0");
            break;
        }
    }
    ibv_free_device_list(devices);
    if (!context)
        errx(1, "roce_dpdk0 is missing from libibverbs discovery");

    struct ibv_device_attr attributes;
    struct ibv_port_attr port;
    if (ibv_query_device(context, &attributes) != 0 || ibv_query_port(context, 1, &port) != 0)
        errx(1, "query roce_dpdk0 and port 1");
    if (attributes.phys_port_cnt != 1 || port.link_layer != IBV_LINK_LAYER_ETHERNET ||
        port.state != IBV_PORT_DOWN || attributes.max_qp || attributes.max_cq || attributes.max_mr)
        errx(1, "unexpected roce_dpdk0 port attributes");

    struct ibv_pd *pd = ibv_alloc_pd(context);
    if (!pd)
        err(1, "allocate roce_dpdk0 protection domain");
    if (ibv_create_cq(context, 1, NULL, NULL, 0) || errno != EOPNOTSUPP)
        errx(1, "roce_dpdk0 should report that completion queues are unavailable");
    char buffer[16];
    if (ibv_reg_mr(pd, buffer, sizeof(buffer), IBV_ACCESS_LOCAL_WRITE) || errno != EOPNOTSUPP)
        errx(1, "roce_dpdk0 should report that verbs memory registration is unavailable");
    if (ibv_dealloc_pd(pd) != 0 || ibv_close_device(context) != 0)
        errx(1, "release roce_dpdk0 resources");

    puts("libibverbs discovery, context, port query and PD checks passed");
    return 0;
}
