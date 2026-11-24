# Native libibverbs provider

`roce_provider.c` builds a libibverbs plugin for `roce_dpdk0`.
It uses the standard uverbs context, device queries, port queries
and protection-domain commands. The kernel device currently advertises
zero CQ, QP and verbs MR capacity. The service-backed implementations
of those resources are pending.

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

The probe checks enumeration, context and PD lifetimes, Ethernet
port attributes, and rejection of CQ and MR commands.
[Kernel setup](../kernel/README.md#verbs-device-discovery)
