#define _GNU_SOURCE

#include "roce_mr_uapi.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static struct ibv_context *open_rdma_device(const char *name)
{
	struct ibv_device **devices = ibv_get_device_list(NULL);
	struct ibv_context *context = NULL;

	if (!devices)
		err(1, "list RDMA devices");
	for (struct ibv_device **device = devices; *device; ++device) {
		if (strcmp(name, ibv_get_device_name(*device)))
			continue;
		context = ibv_open_device(*device);
		if (!context)
			err(1, "open RDMA device %s", name);
		break;
	}
	ibv_free_device_list(devices);
	if (!context)
		errx(1, "RDMA device %s is unavailable", name);
	return context;
}

static int export_after_owner_release(int device, size_t length, uint32_t access)
{
	unsigned char *buffer = mmap(NULL, length, PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	struct roce_mr_register request = {
		.address = (uintptr_t)buffer,
		.length = length,
		.access = access,
	};
	int mapping, dmabuf, flags;

	if (buffer == MAP_FAILED)
		err(1, "map application memory");
	memset(buffer, 0xa5, length);
	mapping = ioctl(device, ROCE_MR_REGISTER, &request);
	if (mapping < 0)
		err(1, "register application memory");
	dmabuf = ioctl(mapping, ROCE_MR_EXPORT_DMABUF, 0);
	if (dmabuf < 0)
		err(1, "export application memory as DMA-BUF");
	if (fcntl(dmabuf, F_GETFD) != FD_CLOEXEC)
		errx(1, "DMA-BUF export did not return CLOEXEC");
	flags = fcntl(dmabuf, F_GETFL);
	if (flags < 0 || (flags & O_ACCMODE) != (access ? O_RDWR : O_RDONLY))
		errx(1, "DMA-BUF export changed the region's access mode");
	if (lseek(dmabuf, 0, SEEK_END) != (off_t)length)
		errx(1, "DMA-BUF export changed the region's size");
	if (close(mapping) || munmap(buffer, length))
		err(1, "release application memory and mapping descriptor");
	return dmabuf;
}

int main(int argc, char **argv)
{
	long page_size = sysconf(_SC_PAGESIZE);
	struct ibv_context *context;
	struct ibv_pd *pd;
	struct ibv_mr *mr;
	int device, dmabuf, error;
	size_t length;

	if (argc != 2)
		errx(1, "usage: %s RDMA_DEVICE", argv[0]);
	if (page_size <= 0)
		errx(1, "could not determine host page size");
	length = 2 * (size_t)page_size;
	context = open_rdma_device(argv[1]);
	pd = ibv_alloc_pd(context);
	if (!pd)
		err(1, "allocate protection domain");
	device = open("/dev/roce_mr", O_RDWR | O_CLOEXEC);
	if (device < 0)
		err(1, "open /dev/roce_mr");

	dmabuf = export_after_owner_release(device, length, ROCE_MR_ACCESS_LOCAL_WRITE);
	mr = ibv_reg_dmabuf_mr(pd, 0, length, (uint64_t)page_size, dmabuf,
			      IBV_ACCESS_LOCAL_WRITE);
	if (!mr)
		err(1, "register writable DMA-BUF with %s", argv[1]);
	if (close(dmabuf))
		err(1, "close DMA-BUF descriptor while verbs still owns the import");
	error = ibv_dereg_mr(mr);
	if (error)
		errx(1, "deregister writable DMA-BUF: %s", strerror(error));

	/* Linux RDMA core requests DMA_BIDIRECTIONAL even with verbs access zero. */
	dmabuf = export_after_owner_release(device, length, 0);
	mr = ibv_reg_dmabuf_mr(pd, 0, length, (uint64_t)page_size, dmabuf, 0);
	if (mr)
		errx(1, "verbs accepted bidirectional DMA for a read-only export");
	if (errno != EACCES)
		err(1, "read-only DMA-BUF failed for an unexpected reason");
	if (close(dmabuf) || close(device))
		err(1, "close memory registration descriptors");

	error = ibv_dealloc_pd(pd);
	if (error)
		errx(1, "release protection domain: %s", strerror(error));
	error = ibv_close_device(context);
	if (error)
		errx(1, "close RDMA device: %s", strerror(error));
	puts("DMA-BUF verbs import and direction checks passed");
	return 0;
}
