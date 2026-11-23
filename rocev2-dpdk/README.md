# RoCEv2 over DPDK

`roce_write` originates and responds to host-memory IPv4 RoCEv2 Reliable
Connected WRITEs on one DPDK Ethernet port. `roce_verbs_write` exercises the
same test with a native verbs QP. Each process transfers one message. The
sender reports success after its transport completion and the receiver's
byte-for-byte verification.

The test uses one directly connected Ethernet path, the default P_Key, an
untagged IPv4 data path, and one nonempty plain RDMA WRITE. Both peers exchange
QP numbers, initial PSNs, PMTU, and buffer attributes over a private TCP
connection. Run it on a trusted test network. The control protocol exposes the
test memory region to the peer and assumes both processes are cooperating.

[CONFORMANCE.md](CONFORMANCE.md) records the specification mapping, verification
commands, Spark hardware evidence, and implementation notes from BURST.

## Build and verify

Build with a C compiler, pkg-config, DPDK with the desired PMD, and
libibverbs development files. For a non-system DPDK install, select its
`libdpdk.pc` with `PKG_CONFIG_PATH` and its libraries with `LD_LIBRARY_PATH`.

```sh
make
make check
make check-memif
make check-peer
```

`make check` needs only a C compiler and Python 3. It exercises the wire codec
against independently encoded frames and drives the RC state machine through
loss, duplicate, access, timeout, and PSN-wrap cases. `make check-memif` needs
DPDK's memif PMD and two accessible CPUs. It runs two copies of `roce_write`,
using TCP on loopback and Ethernet frames over memif.
`make check-peer` uses the same prerequisites. It runs the production
endpoint against an independent Python RC peer, with the MRC memif relay
carrying raw frames. It checks retry after a sequence NAK and a lost final
ACK, rejects a corrupt iCRC, and verifies that a duplicate cannot overwrite
completed data.

## Spark wire interoperability

Use a dedicated data function and a separate IP path for TCP setup. The
native peer needs a matching IPv4-mapped RoCEv2 GID, an active Ethernet RDMA
port, and a reachable neighbor for the DPDK data IP. On the DPDK host, select an
mlx5-enabled DPDK runtime and a function that delivers UDP destination 4791 to
its Ethernet RX queue. See the [mlx5 privilege requirements](https://doc.dpdk.org/guides-23.11/platform/mlx5.html#run-as-non-root)
and [NVIDIA's UDP 4791 steering guidance](https://docs.nvidia.com/networking/display/bfswtroubleshooting/mlnx-dpdk.pdf).
Keep native RoCE enabled on the verbs peer. The commands use the current
link MTU and require it to fit the requested RoCE PMTU.

Example parameters for the Spark-04 to Spark-02 lab link at PMTU 1024.
Start the receiver and wait for `control listener ready`.

```sh
# Spark-02, native receiver
./roce_verbs_write rx 8195 10.61.0.13 28732 10.61.0.1 1024 rocep1s0f1 1 3

# Spark-04, DPDK sender
./roce_write -l 0 --iova-mode=va --no-huge -a 0000:01:00.0 -- \
  tx 8195 10.61.0.13 28732 10.61.0.2 1024 0000:01:00.0
```

Reverse the roles to verify both directions.

```sh
# Spark-04, DPDK receiver
./roce_write -l 0 --iova-mode=va --no-huge -a 0000:01:00.0 -- \
  rx 8195 10.61.0.14 28733 10.61.0.2 1024 0000:01:00.0

# Spark-02, native sender
./roce_verbs_write tx 8195 10.61.0.14 28733 10.61.0.1 1024 rocep1s0f1 1 3
```

A pass requires successful sender transport completion and successful payload
verification by the receiver. Repeat at 3 bytes for WRITE Only and 8195 bytes
for an unaligned multi-packet WRITE. The DPDK endpoint reports port counters
when a run fails after control setup. The native executable uses libibverbs
directly, so it also serves as a native-to-native control.
