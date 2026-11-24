# Native libibverbs provider

`roce_provider.c` builds a libibverbs plugin for `roce_dpdk0`.
It uses the standard uverbs context, device queries, port queries
and protection-domain commands. With `ROCE_DPDK_MR_SOCKET` set before
opening the device, the provider also supports `ibv_reg_mr()` and
`ibv_dereg_mr()` through the worker's registration socket. The CQ and
QP operations are pending.

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

```sh
make -C rocev2-dpdk/provider RDMA_CORE_BUILD="$RDMA_CORE_BUILD" check-mr
python3 rocev2-dpdk/tests/check_app_memif.py \
    --verbs-provider "$PROVIDER_DIR/libroce_dpdk"
```

[Kernel setup](../kernel/README.md#verbs-device-discovery)
