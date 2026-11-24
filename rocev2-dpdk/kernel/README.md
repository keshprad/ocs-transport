# Kernel memory registration

`roce_mr.ko` provides `/dev/roce_mr`. Build it against the running
kernel headers with `make -C rocev2-dpdk/kernel` from the repository
root. The device defaults to mode `0600`. Deployment can grant
access to the application account explicitly.

1. The application populates its page-aligned memory and calls
   `ROCE_MR_REGISTER`. The module pins only pages in the caller's
   address space and charges them against the registering address
   space's `RLIMIT_MEMLOCK` as a long-term pin.
2. The returned descriptor refers to that one region. The libibverbs
   provider exports a one-time Generic Netlink ticket to the worker.
   The direct registration tool can pass the descriptor over a Unix
   socket with `SCM_RIGHTS`. The worker checks the ABI version, length
   and write permission via `ROCE_MR_GET_INFO` before mapping it with
   `MAP_SHARED`, offset zero and the registered length.
3. The application keeps the region alive through work completion.
   The kernel retains the pins until every mapping and descriptor of
   the region is released. Read-only registrations cannot be upgraded
   to a writable mapping.

The mapped alias uses special PTEs to give the worker CPU access to
anonymous folios. Linux GUP rejects that alias for a second pin. The
worker's registration service stops granting leases when the app's
registration socket closes. A lease retains its mapping and reserves
the key until the worker releases it.
[Linux GUP design](https://docs.kernel.org/core-api/pin_user_pages.html),
[Native libibverbs provider](../provider/README.md)

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
The DPDK 23.11.7 mlx5 patch imports these exports for packet scatter/gather.
[DPDK worker setup](../README.md#native-verbs-memory-registration)
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

## Generic Netlink handoff

The worker binds a `NETLINK_GENERIC` socket in the same network namespace
as the application. It communicates the resulting Netlink port ID and a
registration cookie over the connected Unix management socket. The app
calls `ROCE_MR_EXPORT_TICKET` on its mapping descriptor with that port ID,
cookie, and its connected management descriptor. The kernel takes the
worker process identity from the management peer credentials and checks
that the two processes share an effective UID and user namespace.

Family `roce_mr` version `1` sends a unicast `ROCE_MR_GENL_CMD_TICKET`
notification with a 16-byte ticket and the native-endian 64-bit cookie.
The worker checks that the sender is the kernel and uses the cookie to
associate the notification with its pending registration. The worker
passes the ticket to `ROCE_MR_IMPORT_TICKET` on `/dev/roce_mr` and receives
a fresh close-on-exec mapping descriptor with the same access permissions.

Only the designated process can import the ticket. A successful import
consumes it, and a failed import can be retried while it remains valid.
Each exporting file can have one pending ticket. The ticket expires after
30 seconds or when the last reference to the exporting file is dropped.
`make -C rocev2-dpdk/kernel check-ticket` verifies notification and cookie
delivery, cross-process authorization, descriptor exhaustion, replay
rejection, cancellation, and CPU mapping visibility.

## Verbs device discovery

Kernels with `CONFIG_INFINIBAND_USER_ACCESS` also build `roce_uverbs.ko`.
This module registers a software RDMA device through `ib_core` with no
associated NIC. The device appears as `roce_dpdk0` on a fresh host. Its
name starts with `roce_dpdk`, its `node_desc` is `RoCEv2 DPDK`, and its
uverbs ABI version is `1`. The module uses `RDMA_DRIVER_UNKNOWN` pending
an assigned driver ID. Stock libibverbs selects a matching userspace
provider before it presents the device to applications.

Standard uverbs commands provide a context, device and port queries,
and protection domains. The single Ethernet port reports `DOWN`, an
empty GID table, and no associated network device. Device queries report
zero CQ, QP and verbs MR capacity. RDMA core rejects unsupported transport
resource commands. The separate `/dev/roce_mr` interface handles pinned
memory and DMA-BUF export.

After installation with `ib_uverbs` available, use `rdma dev show` and
`rdma link show` to verify RDMA core registration and the down port.
The matching `/sys/class/infiniband_verbs/uverbsN/ibdev` should contain
`roce_dpdk0`, with `abi_version` equal to `1`. With the matching libibverbs
provider installed, `ibv_devinfo -d roce_dpdk0` opens the context and queries
the device and port. The [provider's verbs probe](../provider/README.md)
checks protection-domain lifetimes and rejection of CQ and MR creation.
