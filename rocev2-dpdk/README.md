# RoCEv2 over DPDK

`roce_write` originates and responds to host-memory IPv4 RoCEv2 Reliable
Connected WRITEs on one DPDK Ethernet port. `roce_verbs_write` exercises the
same test with a native verbs QP. By default each process transfers one message. The
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
On Spark-04, with the dedicated data link idle, disable native RoCE on the
DPDK function so UDP 4791 reaches its Ethernet RX queue. Reloading the
function briefly interrupts that link.

```sh
sudo devlink dev param set pci/0000:01:00.0 name enable_roce value false cmode driverinit
sudo devlink dev reload pci/0000:01:00.0 action driver_reinit
```

Confirm that `enp1s0f0np0` is back up with `10.61.0.2/30`. Start the
receiver and wait for `control listener ready`.

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

For a temporary test, restore the prior Spark-04 native RoCE setting by
repeating the devlink commands with `value true` once the services are idle.

## Single-QP bandwidth probes

For an initial DPDK-to-DPDK comparison, run `roce_write` at both ends with
the same message size and `--report-bw`. Both data functions need to
deliver UDP 4791 to DPDK. Leave the TCP control path on a separate rail.
For the direct Spark-04 to Spark-02 first rail, the receiver uses
`0000:01:00.1`, data address `10.61.0.1`, and control address
`10.61.0.13`. Use an idle lab link and start the receiver before the sender.

Both endpoints report payload bytes, elapsed time, and polling-thread CPU
time. The timed interval begins after control setup and ends when the
requester processes the final ACK or the responder places the final
segment. The executables report bandwidth after the receiver verifies
its buffer. For a longer DPDK-to-DPDK run, append `--writes N` at both
ends. `BYTES` then gives the size of each WRITE, and each host allocates a
buffer of `BYTES × N`. The sender pipelines successive WRITEs on the same
QP, and the receiver verifies every byte across all destination regions
after the transfer. For example, use `BYTES=1048576` and `--writes 4096`
to send 4 GiB in 1 MiB WRITEs. The transfer has one setup exchange and
one timed interval.

The requester accepts `--tx-burst N` (1–32, default 32) and
`--send-window N` (1–512 packets, default 32). Use `--tx-burst 1`
for a single-packet TX submission comparison. A build with Arm CRC
instructions enabled uses the hardware IEEE CRC32 operation for iCRC.
Use `CPPFLAGS=-DOCS_ROCE_PORTABLE_CRC` for a clean build of the
portable comparison.

### Initial performance controls

On November 23, 2026, both Spark-04/Spark-02 lab rails reported 100 Gbit/s
and Ethernet MTU 1500. The still-native second rail delivered 92.58 Gbit/s
with one QP and 92.34 Gbit/s with 16 QPs and post lists of 16. These
unidirectional perftest 6.30 trials used 1 MiB WRITEs, PMTU 1024,
transmit depth 128, CQ moderation 16 and a four-second measurement
between two-second margins.

The table shows median requester payload rates for three byte-verified
64 MiB single-QP WRITEs at PMTU 1024 over local DPDK memif.

| iCRC | TX burst | Window (packets) | Payload Gbit/s |
| --- | --- | --- | --- |
| Portable | 1 | 32 | 1.376 |
| Arm CRC instructions | 1 | 32 | 12.454 |
| Arm CRC instructions | 32 | 32 | 10.274 |
| Arm CRC instructions | 32 | 256 | 19.380 |

The pipelined mode verified 4,096 1 MiB WRITEs at 17.73 Gbit/s over memif
at PMTU 1024, with zero retries and approximately one polling core per
endpoint. A separate PMTU 256 stress run verified 4,097 WRITEs and
16,781,312 packets with zero retries, exercising both offsets above
4 GiB and a full cycle of 24-bit PSNs.

The memif timings compare datapath changes on one host. Measure the
physical DPDK link before selecting a default window or burst size. For
the near-line-rate target, compare sustained software payload rate with
the matched native payload rate on the same paths and MTU. Use 95% of
the native control as the initial gate. Report single-QP and batched
multi-QP rates separately, following [BURST §5.1 and Appendix A.7](https://www.usenix.org/system/files/nsdi26-shen.pdf).
