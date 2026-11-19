# DPDK multipath transport

`mrc_write` is the in-progress OCP MRC 1.0 endpoint. See the
[conformance status and verification](CONFORMANCE.md).

## OCP WRITE benchmark

From this directory, `make` builds `mrc_write` and the original prototype.
`make check` runs the portable format and state tests. With the memif PMD
available, `python3 tests/check_mrc_write.py` exercises the OCP endpoint
without using the physical NICs.

The arguments after EAL are
`tx|rx BYTES_PER_WR LOCAL_IP PEER_IP PORT0 PEER_MAC0 PORT1 PEER_MAC1`.
Ports may be DPDK port IDs or names, including PCI BDFs. Both peers use
matching QP and memory attributes. Run `mrc_write` with valid EAL parameters
and no benchmark arguments for the option list. Use `--work-requests N` for
multiple WRs and `--immediate N` to give the first WR an immediate value.
Subsequent immediate values increment modulo 32 bits. The default receiver
posts one receive credit per WRITEIMM. `--max-wimm-inflight N` sets the agreed
credit limit and `--mpr N` sets the static window in units of 128 packets.

### Spark double ring

Run the [revision gate](CONFORMANCE.md#verification) immediately before
starting the test. Build on each selected host from its clean test worktree.
On the direct Spark-04 to Spark-02 pair, use the following route mapping:

| End | Local IP | Peer IP | Path 0 PCI / peer MAC | Path 1 PCI / peer MAC |
| --- | --- | --- | --- | --- |
| Spark-04 | 10.61.0.2 | 10.61.0.1 | `0000:01:00.0` / `4c:bb:47:82:4e:db` | `0002:01:00.0` / `4c:bb:47:82:4e:df` |
| Spark-02 | 10.61.0.1 | 10.61.0.2 | `0000:01:00.1` / `4c:bb:47:2e:7e:65` | `0002:01:00.1` / `4c:bb:47:2e:7e:69` |

Use EAL `-a` for each selected PCI address and `--iova-mode=va --no-huge`
for the private mlx5 runtime. Start Spark-02 as `rx` and Spark-04 as `tx`.
The configured IP pair identifies the QP across both circuits, while each
circuit uses its corresponding peer MAC. First run with both paths healthy.
Use `--drop-path-after N` on the sender for an in-process path-1 blackhole
that leaves the Polatis configuration untouched.

## Original multipath prototype

`mrc_bench` is the original prototype. It sends one host-memory buffer over two DPDK Ethernet ports.
It stripes 1,400-byte segments across the ports, places segments by sequence
number, and sends selective ACKs as 64-segment bitmaps. When a segment's ACK
times out, the sender retries it on the other path when available and temporarily
avoids the timed-out path for new segments. The receiver verifies the entire
buffer before reporting success.

This benchmark uses a private frame format with the experimental Ethernet
EtherType `0x88b5`. It requires two paths that can deliver Ethernet frames to
the peer MACs on untagged Ethernet. It implements a single unidirectional transfer with a fixed
send window and an ACK timer. The OCP MRC wire format, RDMA verbs, SRv6, path
probes, and congestion control are outside the scope of this prototype.

## Build and run on two hosts

Install a C compiler, Make, pkg-config, and DPDK development libraries with the
PMD for the target NIC. The build was checked with DPDK 23.11.7 on Linux AArch64. Run the commands below from the `mrc-dpdk` directory.

```sh
make
```

On each host, identify the two data interfaces and their local MAC and PCI
addresses with `ip link` and `ethtool -i`. Match the two DPDK port IDs to
the corresponding peer MACs. The commands below assume DPDK port IDs 0 and 1.
Use the kernel-bound `mlx5_core` driver for ConnectX-7. Use the benchmark's
ports as data interfaces during the run, since the mlx5 PMD's default receive
steering can take over unicast traffic on the same MAC. Make sure the selected
ports have an Ethernet MTU of at least 1,420 bytes. The endpoint logs its DPDK
port IDs and device names at startup. See the [mlx5 PMD documentation](https://doc.dpdk.org/guides/nics/mlx5.html).

Start the receiver with the sender's MAC addresses.

```sh
sudo ./mrc_bench -l 2 -a <RX_PCI_0> -a <RX_PCI_1> -- \
    rx 4194304 1 0 <TX_MAC_0> 1 <TX_MAC_1>
```

Start the sender with the receiver's MAC addresses and the same byte count and
transfer ID.

```sh
sudo ./mrc_bench -l 2 -a <TX_PCI_0> -a <TX_PCI_1> -- \
    tx 4194304 1 0 <RX_MAC_0> 1 <RX_MAC_1>
```

Use an ID unique to the run on those links, and a byte count between 1 and
1,073,741,824. Use the same `--rto-us` at both endpoints if changing the
default of 10,000 µs. `--timeout-ms` is the no-progress timeout and defaults
to 30,000 ms. Add `--drop-path-after 512` to the sender to start silently
dropping path 1 frames after 512 first-send segments have been scheduled.
Both endpoints emit JSON counters. Sender goodput covers transmission through
the final ACK. Use `--trace FILE.csv` to record packet events for comparison.
Tracing adds per-packet work, so leave it off for timing runs.

The current Spark setup exposes two logical paths over one QSFP cable.
Use separate circuits to test a physical interruption that leaves a
surviving path. See [the existing RoCEv2 baseline](../summary.md).

## Behavioral parity with the emulator

Use an upstream [`srv6-mrc-emulator`](https://github.com/segmentrouting/srv6-mrc-emulator)
checkout. The comparison was developed against
`ccd0f5eb711743ae27a83c236445d33e633ee18d`. The test runs two endpoints on
two real DPDK `net_memif` virtual links. It requires Linux, two usable CPU
cores, Python 3, and the memif PMD. It needs no physical NICs or hugepages.

```sh
python3 tests/check_dpdk_mrc_parity.py --emulator /path/to/srv6-mrc-emulator
```

- The healthy trace must match the emulator's two-plane EV-spray scheduler
  packet for packet.
- Both receive traces are replayed through the emulator's reorder tracker. The
  duplicate count, reorder count, maximum reorder distance, and per-path counts
  must agree exactly.
- During the blackhole case, every dropped segment must be retried on the
  survivor. After the emulator's EV policy demotes the same failed path, its
  choices must agree with the prototype's steady-state path selection.

The sender's timeout and the emulator's loss-window detection run independently,
so this check compares the resulting path choice. The memif run measures
behavior on one host. It does not measure Spark link throughput or physical
failover timing.
