# Native libibverbs provider

`roce_provider.c` builds a libibverbs plugin for `roce_dpdk0`.
It uses the standard uverbs context, device queries, port queries
and protection-domain commands. With `ROCE_DPDK_MR_SOCKET` set before
opening the device, the provider also supports `ibv_reg_mr()` and
`ibv_dereg_mr()` through the worker's registration socket. An MR-enabled
context can create CQs and one RC QP before connection negotiation. The
provider retains the QP's PD and CQs at creation and takes its MR
reference when binding the worker's queue mapping. The lab requester
enables native posting with `--verbs-queues`.
The provider supports RC QP setup and query, signaled one-SGE
RDMA WRITE posting and CQ polling. The supplied PD must own the source
MR for the lane. The QP retains both that MR and its CQs until destroy.

Multiple bound QPs can share one polling CQ. Completions carry the
originating QPN and remain ordered within each send queue. The CQ reserves
space for all accepted signaled WRs across its QPs and returns the space
when the application polls their completions. When all slots are reserved,
posting returns `ENOMEM` with `bad_wr` pointing to the first unaccepted WR.
The provider releases a CQ only after its last associated QP is destroyed.

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
unaligned ranges and deregistration while a lease is active. The same test
uses a simulated worker to check shared CQ capacity, completion identity
and teardown with more than one attached QP.
Set `ROCE_DPDK_MR_REQUIRE_NETLINK=1` to require the kernel's ticket
handoff in these checks.

```sh
ROCE_DPDK_MR_REQUIRE_NETLINK=1 \
    make -C rocev2-dpdk/provider RDMA_CORE_BUILD="$RDMA_CORE_BUILD" check-mr
ROCE_DPDK_MR_REQUIRE_NETLINK=1 python3 rocev2-dpdk/tests/check_app_memif.py \
    --verbs-provider "$PROVIDER_DIR/libroce_dpdk"
```

The memif check runs native copy and scatter/gather with one QP using
standard librdmacm event and connection calls. It covers the CM-owned
and external QP flows. The application checks peer addresses, private
data, and both negotiated QPNs, polls every WRITE completion, and
observes disconnect. The CM-owned runs post 521 WRITEs per QP to
verify queue reuse after two full laps. With four available CPUs it
also runs four QPs across two workers using the TCP lab setup.
[librdmacm adapter](../README.md#in-band-connection-setup)

[Kernel setup](../kernel/README.md#verbs-device-discovery)
