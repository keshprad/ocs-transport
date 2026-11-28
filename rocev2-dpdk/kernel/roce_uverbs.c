// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/random.h>
#include <rdma/ib_verbs.h>

#define ROCE_UVERBS_ABI_VERSION 1
#define ROCE_UVERBS_PORT 1
#define ROCE_UVERBS_MAX_PD 4096

struct roce_uverbs_device {
	struct ib_device ibdev;
	atomic_t pd_count;
};

static struct roce_uverbs_device *roce_device;

static int roce_query_device(struct ib_device *ibdev, struct ib_device_attr *attr,
			     struct ib_udata *udata)
{
	*attr = (struct ib_device_attr) {
		.sys_image_guid = ibdev->node_guid,
		.max_pd = ROCE_UVERBS_MAX_PD,
	};
	return 0;
}

static int roce_query_port(struct ib_device *ibdev, u32 port_num,
			   struct ib_port_attr *attr)
{
	if (port_num != ROCE_UVERBS_PORT)
		return -EINVAL;
	*attr = (struct ib_port_attr) {
		.state = IB_PORT_DOWN,
		.phys_state = IB_PORT_PHYS_STATE_DISABLED,
		.max_mtu = IB_MTU_256,
		.active_mtu = IB_MTU_256,
	};
	return 0;
}

static int roce_get_port_immutable(struct ib_device *ibdev, u32 port_num,
				   struct ib_port_immutable *immutable)
{
	if (port_num != ROCE_UVERBS_PORT)
		return -EINVAL;
	/* The discovery device has no transport capabilities or address tables. */
	*immutable = (struct ib_port_immutable) {};
	return 0;
}

static enum rdma_link_layer roce_get_link_layer(struct ib_device *ibdev, u32 port_num)
{
	return IB_LINK_LAYER_ETHERNET;
}

static int roce_alloc_ucontext(struct ib_ucontext *context, struct ib_udata *udata)
{
	return 0;
}

static void roce_dealloc_ucontext(struct ib_ucontext *context)
{
}

static int roce_alloc_pd(struct ib_pd *pd, struct ib_udata *udata)
{
	struct roce_uverbs_device *device = container_of(pd->device, struct roce_uverbs_device,
							ibdev);

	if (!atomic_add_unless(&device->pd_count, 1, ROCE_UVERBS_MAX_PD))
		return -ENOSPC;
	return 0;
}

static int roce_dealloc_pd(struct ib_pd *pd, struct ib_udata *udata)
{
	struct roce_uverbs_device *device = container_of(pd->device, struct roce_uverbs_device,
							ibdev);

	atomic_dec(&device->pd_count);
	return 0;
}

static const struct ib_device_ops roce_device_ops = {
	.owner = THIS_MODULE,
	.driver_id = RDMA_DRIVER_UNKNOWN,
	.uverbs_abi_ver = ROCE_UVERBS_ABI_VERSION,
	.query_device = roce_query_device,
	.query_port = roce_query_port,
	.get_port_immutable = roce_get_port_immutable,
	.get_link_layer = roce_get_link_layer,
	.alloc_ucontext = roce_alloc_ucontext,
	.dealloc_ucontext = roce_dealloc_ucontext,
	.alloc_pd = roce_alloc_pd,
	.dealloc_pd = roce_dealloc_pd,
	.size_ib_pd = sizeof(struct ib_pd),
	.size_ib_ucontext = sizeof(struct ib_ucontext),
};

static int __init roce_uverbs_init(void)
{
	struct roce_uverbs_device *device;
	struct ib_device *ibdev;
	u8 guid[sizeof(ibdev->node_guid)];
	int ret;

	device = ib_alloc_device(roce_uverbs_device, ibdev);
	if (!device)
		return -ENOMEM;

	ibdev = &device->ibdev;
	ibdev->node_type = RDMA_NODE_IB_CA;
	ibdev->phys_port_cnt = ROCE_UVERBS_PORT;
	strscpy(ibdev->node_desc, "RoCEv2 DPDK", sizeof(ibdev->node_desc));
	get_random_bytes(guid, sizeof(guid));
	guid[0] = (guid[0] & 0xfe) | 0x02;
	memcpy(&ibdev->node_guid, guid, sizeof(guid));
	atomic_set(&device->pd_count, 0);
	ib_set_device_ops(ibdev, &roce_device_ops);
	ret = ib_register_device(ibdev, "roce_dpdk%d", NULL);
	if (ret) {
		ib_dealloc_device(ibdev);
		return ret;
	}
	roce_device = device;
	return 0;
}

static void __exit roce_uverbs_exit(void)
{
	ib_unregister_device(&roce_device->ibdev);
	ib_dealloc_device(&roce_device->ibdev);
}

module_init(roce_uverbs_init);
module_exit(roce_uverbs_exit);

MODULE_DESCRIPTION("RoCEv2 DPDK software RDMA device discovery");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: ib_uverbs");
