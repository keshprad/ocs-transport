#include "mrc_abi.h"
#include "mrc_port_netlink.h"

#include <errno.h>
#include <mrc_ctl.h>

static int port_error(int error)
{
    errno = error;
    return -1;
}

static int port_ifindex(struct mrc_context *context, uint8_t port, uint32_t *ifindex)
{
    struct ibv_device_attr device;
    int error = ibv_query_device(context->verbs, &device);
    if (error)
        return error > 0 ? error : (errno ? errno : EIO);
    if (port > device.phys_port_cnt)
        return EINVAL;

    struct ibv_port_attr attr;
    error = ibv_query_port(context->verbs, port, &attr);
    if (error)
        return error > 0 ? error : (errno ? errno : EIO);
    if (attr.link_layer != IBV_LINK_LAYER_ETHERNET)
        return EOPNOTSUPP;

    int index = ibv_get_device_index(context->verbs->device);
    if (index < 0)
        return errno ? errno : EIO;
    return -mrc_port_netlink_rdma_index((uint32_t)index, port, ifindex);
}

MRC_PUBLIC int mrc_ctl_query_port(struct mrc_context *context, uint8_t port_num,
                                   struct mrc_ctl_port_attr *attr, int attr_mask)
{
    if (!context || !port_num || !attr || (attr_mask & ~MRC_CTL_PORT_ADMIN_STATE))
        return port_error(EINVAL);
    uint32_t ifindex;
    int error = port_ifindex(context, port_num, &ifindex);
    if (error)
        return port_error(error);
    if (!(attr_mask & MRC_CTL_PORT_ADMIN_STATE))
        return 0;

    bool enabled;
    error = -mrc_port_netlink_query(ifindex, &enabled);
    if (error)
        return port_error(error);
    attr->port_admin_state = enabled ? MRC_CTL_PORT_ADMIN_ENABLED : MRC_CTL_PORT_ADMIN_DISABLED;
    return 0;
}

MRC_PUBLIC int mrc_ctl_modify_port(struct mrc_context *context, uint8_t port_num,
                                    struct mrc_ctl_port_attr *attr, int attr_mask)
{
    if (!context || !port_num || !attr || (attr_mask & ~MRC_CTL_PORT_ADMIN_STATE) ||
        ((attr_mask & MRC_CTL_PORT_ADMIN_STATE) &&
         attr->port_admin_state != MRC_CTL_PORT_ADMIN_ENABLED &&
         attr->port_admin_state != MRC_CTL_PORT_ADMIN_DISABLED))
        return port_error(EINVAL);
    if (!mrc_abi_controller_capable())
        return port_error(EPERM);
    uint32_t ifindex;
    int error = port_ifindex(context, port_num, &ifindex);
    if (!error && (attr_mask & MRC_CTL_PORT_ADMIN_STATE))
        error = -mrc_port_netlink_set(ifindex, attr->port_admin_state == MRC_CTL_PORT_ADMIN_ENABLED);
    return error ? port_error(error) : 0;
}
