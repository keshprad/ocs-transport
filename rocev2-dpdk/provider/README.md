# Native libibverbs provider

`roce_provider.c` builds a libibverbs plugin for `roce_dpdk0`.
It uses the standard uverbs context, device queries, port queries
and protection-domain commands. With `ROCE_DPDK_MR_SOCKET` set before
opening the device, the provider also supports `ibv_reg_mr()` and
`ibv_dereg_mr()` through the worker's registration socket. The CQ and
QP operations are available after the negotiated queue mapping is bound
to the context. The lab requester enables this with `--verbs-queues`.
The provider supports RC QP setup and query, signaled one-SGE
RDMA WRITE posting and CQ polling. The supplied PD must own the source
MR for the lane. The QP retains both that MR and its CQs until destroy.

The provider uses libibverbs private ABI 34, the ABI of rdma-core v50
on both tested Sparks. Build the `ibverbs` target from the official
rdma-core v50.0 source to generate the private headers. The provider
links against the installed libibverbs. [rdma-core v50.0](https://github.com/linux-rdma/rdma-core/tree/v50.0)

```sh
cmake -S "$RDMA_CORE_SRC" -B "$RDMA_CORE_BUILD" \
    -DNO_MAN_PAGES=1 -DNO_PYVERBS=1 -DENABLE_RESOLVE_NEIGH=0
cmake --build "$RDMA_CORE_BUILD" --target ibverbs
make -C rocev2-dpdk/provider RDMA_CORE_BUILD="$RDMA_CORE_BUILD"
```

After loading `roce_uverbs.ko` and granting the test account access
to its uverbs node, run the check from the repository root. The
Makefile sets `RDMAV_DRIVERS` for the check process. A privileged
system-wide provider install is optional.

```sh
make -C rocev2-dpdk/provider RDMA_CORE_BUILD="$RDMA_CORE_BUILD" check
```

The discovery probe checks the device without a worker connection.
With both kernel modules loaded, run the separate MR lifecycle check.
It starts a worker-side registration listener and checks MR permissions,
unaligned ranges and deregistration while a lease is active.
Set `ROCE_DPDK_MR_REQUIRE_NETLINK=1` to require the kernel's ticket
handoff in these checks.

```sh
ROCE_DPDK_MR_REQUIRE_NETLINK=1 \
    make -C rocev2-dpdk/provider RDMA_CORE_BUILD="$RDMA_CORE_BUILD" check-mr
ROCE_DPDK_MR_REQUIRE_NETLINK=1 python3 rocev2-dpdk/tests/check_app_memif.py \
    --verbs-provider "$PROVIDER_DIR/libroce_dpdk"
```

The memif check runs native copy and scatter/gather with one QP using
standard librdmacm event and connection calls. The application checks
peer addresses and private data during establishment, polls native
WRITE completions, and observes disconnect. With four available CPUs
it also runs four QPs across two workers using the TCP lab setup.
The lab client supplies the local queue mapping after connection
negotiation. The optional [librdmacm adapter](../README.md#in-band-connection-setup)
uses the worker-assigned QPN for connect and accept.

[Kernel setup](../kernel/README.md#verbs-device-discovery)
