# Kernel memory registration

`roce_mr.ko` provides `/dev/roce_mr`. Build it against the running
kernel headers with `make -C rocev2-dpdk/kernel` from the repository
root. The device defaults to mode `0600`. Deployment can grant
access to the application account explicitly.

1. The application populates its page-aligned memory and calls
   `ROCE_MR_REGISTER`. The module pins only pages in the caller's
   address space and charges them against the registering address
   space's `RLIMIT_MEMLOCK` as a long-term pin.
2. The returned descriptor refers to that one region. The application
   passes it to the worker over a Unix socket with `SCM_RIGHTS`. The
   worker checks the ABI version, length and write permission via
   `ROCE_MR_GET_INFO` before mapping it with `MAP_SHARED`, offset zero
   and the registered length.
3. The application keeps the region alive through work completion.
   The kernel retains the pins until every mapping and descriptor of
   the region is released. Read-only registrations cannot be upgraded
   to a writable mapping.

The mapped alias uses special PTEs to give the worker CPU access to
anonymous folios. Linux GUP rejects that alias for a second pin. The
descriptor handoff uses the Unix socket. Generic Netlink registration
events and uverbs integration are follow-up milestones.
[Linux GUP design](https://docs.kernel.org/core-api/pin_user_pages.html)

On kernels built with `CONFIG_DMA_SHARED_BUFFER`,
`ioctl(mapping_fd, ROCE_MR_EXPORT_DMABUF, 0)` returns a close-on-exec
DMA-BUF descriptor over the same pinned pages. The
DMA-BUF descriptor and each importer retain the registration independently
of the CPU alias. DMA mappings require a coherent device and direct access
to the pages without bounce buffers. Read-only registrations allow only
device reads (`DMA_TO_DEVICE`). The application and the device consumer must
coordinate access and retain the registration through work completion.
[Linux DMA-BUF API](https://docs.kernel.org/driver-api/dma-buf.html)

Linux RDMA core requests bidirectional DMA for `ibv_reg_dmabuf_mr`.
Register writable memory with `ROCE_MR_ACCESS_LOCAL_WRITE` for that path.
The DPDK 23.11.7 mlx5 PMD still needs DMA-BUF import support to use these
exports for packet scatter/gather.
[Linux RDMA DMA-BUF importer](https://github.com/torvalds/linux/blob/v6.17/drivers/infiniband/core/umem_dmabuf.c)

Once the module is loaded and the device is available to the test
account, run `make -C rocev2-dpdk/kernel check`. The test transfers
a mapping descriptor between two processes, verifies visibility in
both, and checks that closing the descriptor and releasing the
application mapping leaves the worker mapping live.

With `libibverbs` development files and access to an RDMA device, run
`make -C rocev2-dpdk/kernel check-dma RDMA_DEVICE=rdmap1s0f0`.
This test imports a writable export after releasing both the application
VMA and CPU mapping descriptor, then verifies rejection of a read-only
export by the bidirectional RDMA importer.
