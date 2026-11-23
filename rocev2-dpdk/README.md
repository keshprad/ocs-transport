# RoCEv2 over DPDK

`roce_write` originates and responds to host-memory IPv4 RoCEv2 Reliable
Connected WRITEs on one DPDK Ethernet port. `roce_verbs_write` exercises the
same test with a native verbs QP. By default each process transfers one
message on one QP. The sender reports success after its transport completion
and the receiver's byte-for-byte verification.

The test uses one directly connected Ethernet path, the default P_Key, an
untagged IPv4 data path, and nonempty plain RDMA WRITEs. Both peers exchange
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

## Bandwidth probes

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

Add `--qps N` at both DPDK endpoints for 1–16 independent QPs. Each QP
uses its own PSNs, retry state, and slice of the buffer. The total
verified payload is `BYTES × writes × qps`. For example, compare 1 GiB
over one QP using `--writes 1024` with the same payload over 16 QPs
using `--writes 64 --qps 16`, both with `BYTES=1048576`.
By default one polling core rotates TX bursts across QPs.
`--tx-burst` counts packets per PMD submission. Use `--workers N` on
both DPDK endpoints and enable at least N EAL lcores, including the
main lcore. For example, use `-l 5,6,7,8` before the EAL separator and
`--qps 16 --workers 4` after it. Each worker owns one RX/TX queue pair
and a disjoint range of QPs. Memif connects matching queue indices. On
physical devices, DPDK flow rules steer each QP by its peer IP addresses
and UDP ports, so the PMD must support that pattern and queue action.

The multi-worker rate uses aggregate verified bytes and ends at the
last local queue completion. Reported CPU time sums the polling threads
until each exits or observes endpoint completion.

The requester accepts `--tx-burst N` (1–32, default 32) and
`--send-window N` (1–512 packets per QP, default 32).
`--queue-window N` optionally caps the combined outstanding packet
count across the QPs on each worker. Use `--tx-burst 1`
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

In a local DPDK memif A/B test, enabling a 256-mbuf cache on each polling
core raised the median single-QP requester rate from 22.16 to 40.16 Gbit/s
over three 1 GiB runs per variant. With the cache enabled in the endpoint,
a separate run of three 1 GiB trials per QP count measured 42.20 Gbit/s
on one QP and 43.54 Gbit/s on 16 QPs. Both experiments pinned RX to CPU 5
and TX to CPU 9 on Spark-04 and used PMTU 1024, 1 MiB WRITEs, 16 packets
per TX burst, and a 256-packet window per QP. Every transfer verified with
zero retries and used approximately one polling core per endpoint.

Three further 4 GiB memif trials per configuration used 16 QPs and
256 WRITEs per QP with the same PMTU, burst, and window. RX used the
first N CPUs of 5, 6, 15, and 16 on Spark-04. TX used the first N of
9, 8, 19, and 18. All nine transfers verified with zero retries.

| Polling workers per endpoint | Median requester payload Gbit/s | Median requester polling cores used |
| --- | --- | --- |
| 1 | 45.40 | 1.00 |
| 2 | 73.11 | 2.00 |
| 4 | 134.99 | 3.85 |

The memif timings compare datapath changes on one host. Measure the
physical DPDK link before selecting a default window or burst size. For
the near-line-rate target, compare sustained software payload rate with
the matched native payload rate on the same paths and MTU. Use 95% of
the native control as the initial gate. Report single-QP and batched
multi-QP rates separately, following [BURST §5.1 and Appendix A.7](https://www.usenix.org/system/files/nsdi26-shen.pdf).

### Physical single-rail sweep

Run the first-rail sweep from Spark-04 after the pinned services are
installed on Spark-04 and Spark-02 and both data functions are in raw
Ethernet mode. TCP setup uses the second rail. Each sample transfers
16 GiB in 1 MiB WRITEs at PMTU 1024. The four profiles use one QP or
16 QPs with one, two, or four workers. Each profile runs three samples
by default.

```sh
python3 tests/check_spark_bandwidth.py \
  --destination "/opt/ocs-roce-bw/$REVISION" --sha256 "$BINARY_SHA256"
```

The runner verifies the binary and service arguments on both hosts.
It records the active link configuration, both endpoints' verified
bandwidth and CPU time, the requester's retry count, and the systemd
invocation logs.

On November 23, 2026, the `3a6a1ca` build completed this sweep from
Spark-04 to Spark-02 on the first 100 Gbit/s rail at Ethernet MTU 1500.
The receiver verified all 16 GiB in every trial. Each worker used a
3.9 GHz core.

| QPs | Workers per endpoint | Median TX Gbit/s | Median RX Gbit/s | Median TX / RX polling cores | Requester retries per trial |
| --- | --- | --- | --- | --- | --- |
| 1 | 1 | 29.189 | 29.190 | 1.000 / 1.000 | 0, 0, 0 |
| 16 | 1 | 23.096 | 23.096 | 1.000 / 1.000 | 30,899, 32,424, 35,889 |
| 16 | 2 | 42.205 | 42.195 | 2.000 / 2.000 | 39,942, 35,759, 34,747 |
| 16 | 4 | 90.506 | 90.515 | 3.978 / 4.000 | 61, 73, 147 |

### Physical dual-rail sweep

The native runner accepts a local perftest 6.30 binary and checks the
RoCEv2 GIDs and current Ethernet MTU on both Sparks. It measures each
rail separately and both rails simultaneously, with separate one-QP
and batched 16-QP controls.

```sh
python3 tests/check_spark_native_bandwidth.py --perftest "$PERFTEST_BIN"
```

With the pinned aggregate services installed and all four data
functions in raw Ethernet mode, add `--aggregate` to the DPDK runner.
Each rail uses 16 QPs and four polling cores per endpoint and verifies
16 GiB. The lab TCP connections use loopback SSH forwards over the
management link. The requester option `--start-gate PORT` synchronizes
release across both sender processes on Spark-04. The aggregate metric
uses 32 GiB over the interval from the first sender start to the last
sender completion. The report records overlap and start skew.
