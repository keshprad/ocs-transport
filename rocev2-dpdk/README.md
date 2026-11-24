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

## Application-owned buffers

Append `--app-socket PATH` to the `roce_write` arguments on each
endpoint to run the DPDK worker separately from the application. Use
a socket path in a private directory owned by the endpoint UID. The
worker prints `application listener ready` when it can accept the
application. Start the client as the same UID.

```sh
./roce_app_write "$APP_SOCKET"
make check-app
```

The application allocates and initializes its buffer, then sends a
mapping descriptor to the worker over the Unix socket. The receiving
application verifies the bytes in its own mapping before returning the
verification response. `make check-app` exercises the four-process
path over memif.

The requester application posts batches through a process-shared
multi-producer doorbell queue per worker. Workers admit each batch
while polling the data path and publish cumulative completions per QP.
Use `--app-post-batch N` on the requester to exercise smaller live
posts. Copy TX can wait for each batch's acknowledgement before the next
post. Scatter/gather producers post batches as long as there is room in
the doorbell ring or native send queue. The worker publishes completions
after returning the TX references.
With native scatter/gather and `--app-post-batch 1`, each producer waits
for its verbs WC before posting its next WRITE.

With the registration module loaded and accessible to the client, use
`./roce_app_write "$APP_SOCKET" --kernel-mr /dev/roce_mr` to register a
page-aligned application allocation. The [kernel test](kernel/README.md)
covers the descriptor and pin lifetime independently.

For scatter/gather transmit, add `--scatter-gather` to the application-backed
requester. The worker builds the headers, padding and iCRC in its own buffers
and references the application payload as a separate TX segment. The CPU still
reads the payload to compute the iCRC. The receiving worker copies into the
application mapping before acknowledging the WRITE.

For software jumbo tests, connect two DPDK `net_tap` interfaces with a
private Linux bridge. Pass `--tap-mtu 4200` and PMTU `4096` on each worker.
The TAP PMD transmits the mbuf chain with `writev()` using the mapped
application payload. [DPDK TAP PMD](https://doc.dpdk.org/guides-23.11/nics/tap.html)

On a physical port this mode currently supports mlx5 in IOVA-as-VA mode
with either memfd backing or a registered MR and a DMA-BUF-capable PMD.
The mlx5 driver registers a writable service alias for DMA. The application
keeps each submitted buffer stable until its completion. For native queues,
the worker requires both an RC acknowledgement and release of the PMD's
references to that WRITE's payload. The benchmark verifies the received
bytes before it reports the transfer complete.

### Native verbs memory registration

With the kernel modules and [provider](provider/README.md) available,
give the worker separate paths with `--app-socket PATH --mr-socket MR_PATH`.
The application calls `ibv_reg_mr()` on `roce_dpdk0`. The provider pins
the enclosing pages. The kernel sends a single-use Generic Netlink
ticket to the worker, which imports the mapping, assigns keys to the
requested range, and holds a lease through transfer and TX cleanup.
The worker matches the registration to the request on the MR socket.
With an older module, the same socket can pass the scoped FD directly.

```sh
RDMAV_DRIVERS="$PROVIDER_DIR/libroce_dpdk" ROCE_DPDK_MR_SOCKET="$MR_PATH" \
  ./roce_app_write "$APP_SOCKET"
```

Add `--verbs-queues` to the requester to use stock libibverbs queue
operations. The lab client binds the worker queue mapping to the
provider and submits WRITEs with `ibv_post_send()`. In the TCP lab setup,
it creates the CQ and RC QP for each lane after negotiation. It retrieves
individual WR IDs and statuses with `ibv_poll_cq()`. The current lab
shape supports up to 256 outstanding signaled one-SGE WRITEs per QP and
uses a distinct completion queue per QP. The provider also supports
[shared polling CQs](provider/README.md). The worker checks the registered source
range and receiving QP's slice before packet submission. A native WRITE can
choose its length and offsets within those ranges. All WRITEs on a lab QP
must add up to its configured byte count. The worker keeps the validated
addresses, lengths, and packet-number ranges for retransmission. The app polls
completions to reuse slots during longer runs. With scatter/gather it
refills as credits arrive and drains the final completions after the
worker retires the last TX descriptors.

Each native worker checks a shared notification counter before draining its
doorbell ring. It scans the posted send-queue heads on overflow and
periodically to recover from an interrupted notification. The shared ring
layout also lets the worker recover postings from older providers.

The native requester reports a fatal NAK on the affected WRITE and retry
exhaustion on the first outstanding WRITE. Earlier covered WRITEs complete
successfully and later posted WRITEs are flushed. On transfer failure, the
worker releases its registration lease after draining the port and keeps
the registration service available for application deregistration.

For physical `--scatter-gather` with native registration, apply the
[mlx5 patch](dpdk/dpdk-23.11.7-mlx5-dmabuf.patch) to DPDK 23.11.7 and use
the matching common and net mlx5 PMDs built with `-Dibverbs_link=shared`.
The worker resolves the import APIs from its loaded net PMD and uses
the same protection domain as that port. The provider requests local
write permission on the sender MR for the kernel's bidirectional
DMA-BUF import. The sender MR has no remote-write key.
The native WRITE sender enables a NIC completion on the last WQE of
each productive TX doorbell, so a short WRITE can retire while the send
queue is idle. The application WC still waits for the covering RoCE
ACK and for the PMD to release every payload reference for that WRITE.

### In-band connection setup

For a single-QP DPDK-to-DPDK lab run, pass `--cm-peer IPV4 MAC` to each
worker with the other endpoint's data-plane addresses. The worker sends
standard REQ, REP and RTU messages over DPDK to establish the QP and
exchanges DREQ and DREP after the transfer. The sender's path MTU
must fit the receiver's configured path MTU. The lab's CM private data
carries the WRITE buffer address, key and workload. The TCP test socket
reports transfer completion and payload verification. CM establishment,
start of data polling, and disconnect use the in-band connection state.

With `--app-socket`, the application issues its connect or listen request
over the local Unix socket. The requester gets its doorbell queue after
the worker reports establishment. `make check-app` exercises this path
alongside scatter/gather TX and CPU receive placement.

To exercise the standard librdmacm calls, add `--rdmacm-socket CM_PATH`
on each worker and export `ROCE_DPDK_CM_SOCKET="$CM_PATH"` for its local
application. Use a distinct path from the application and MR sockets.
Enable `--verbs-queues` on the requester and configure an MR socket on
both endpoints. The app supplies a provider PD and polling CQs. The
active app calls `rdma_create_qp()` after address and route resolution,
and the passive app calls it on the child ID from `CONNECT_REQUEST`.
The resulting QPNs travel over the local CM socket in `CONNECT` and
`ACCEPT`. The worker returns the negotiated queue mapping to the adapter
as a file descriptor. The adapter moves the QP to RTR and RTS before
permitting REP on the passive side or RTU on the active side. The active
app receives `ESTABLISHED` with the REP private data. The passive app
receives `ESTABLISHED` on RTU or when the first validated WRITE confirms
the connection. The worker preserves RC packets that share the RX burst
with establishment.
The worker authenticates the CM socket against the application process
and sends the CM packets on DPDK queue 0. The requester asks the worker
to disconnect after posting its last WRITE. The worker sends DREQ after
the transfer. Each side delivers `DISCONNECTED` after its local DREP
step. The reply sender keeps polling QP1 during the bounded replay window.

This profile supports a single IPv4 RC QP and up to 23 bytes of application
private data in the lab REQ and REP. The receiver QP accepts plain RDMA
WRITEs into its MR. Use `rdma_destroy_qp()` to release the CM-owned QP after
draining completions and before destroying its ID. The application owns
its PD and CQs. `ROCE_DPDK_CM_EXTERNAL_QP=1` selects the lab's external-QP
flow for testing `CONNECT_RESPONSE` and `rdma_establish()`.
`make libroce_rdmacm.so` builds the adapter for explicit linking. The
`roce_app_write` lab client already includes it.
See the [provider test](provider/README.md) for the combined CM, native
queue, and kernel MR run.

To pair a DPDK worker with a stock RDMA peer, use `--cm-peer IPV4` on
`roce_verbs_write` and `--cm-peer IPV4 MAC` on the DPDK worker. Each option
takes the other endpoint's data-plane address. The native executable
links to system librdmacm and creates its QP through `rdma_create_qp()`.
It verifies a plain RC WRITE and disconnects through the system CM.

```sh
./roce_verbs_write rx 8195 "$CONTROL_IP" "$PORT" "$LOCAL_DATA_IP" 1024 \
    "$RDMA_DEVICE" "$RDMA_PORT" "$GID_INDEX" --cm-peer "$DPDK_DATA_IP"
```

Use `tx` to test in the other direction. Both endpoints need the same
WRITE size and an IPv4 RoCEv2 path that negotiates the configured PMTU.
This test exchanges the benchmark MR offer as application private data.
The native peer also accepts that offer when the DPDK side uses the
application provider and the Unix-socket CM adapter.

## Build and verify

Build with a C compiler, pkg-config, DPDK with the desired PMD, and
libibverbs and librdmacm development files. For a non-system DPDK install, select its
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

With the MR modules loaded and the provider available, run the same
independent peer with a native libibverbs scatter/gather requester.
The added case requires Generic Netlink and checks successful WRITE
completion after the sequence NAK and ACK timeout.

```sh
python3 tests/check_peer.py --verbs-provider "$PROVIDER_DIR/libroce_dpdk"
```

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

### Spark-04 DPDK to Spark-02 stock CM

For the in-band CM test, build all three executables and stage the fixed
services from Spark-04. Pass the Git revision that identifies those builds.

```sh
bundle=$(python3 tests/stage_spark_cm.py "$REVISION")
sudo /bin/bash "$bundle/install.sh"
```

Copy the bundle's `spark-02` directory to Spark-02 and run its `install.sh`
with sudo. That installer grants the test only the three exact devlink
commands for the dedicated first-rail PCI function. The paired bandwidth
services already grant the required MTU commands for these interfaces.
The stock librdmacm peer runs as the SSH user.

```sh
python3 tests/check_spark_cm.py --bundle "$bundle" \
    --destination "/opt/ocs-roce-cm/$REVISION" --manage-peer-mode
```

The runner checks the installed binaries, unit files, live GID, and link.
It resolves the native peer's ARP neighbor while the DPDK port is idle.
It temporarily switches the dedicated Spark-02 port to native RoCE and
restores the original port mode and MTUs after the test. It requires CM
establishment, disconnect, and an 8,195-byte verified WRITE in each
direction at MTUs 1500 and 4200. The Spark-04 requester also reports mlx5
scatter/gather DMA mapping of its memfd buffer. The runner saves the per-role
logs and hashes to its result directory.

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

For a Cortex-X925 comparison, build the optional four-lane iCRC with
CRC32 and PMULL enabled. It folds complete 1 KiB blocks and uses scalar
CRC32 for the remainder. `OCS_ROCE_PORTABLE_CRC` still selects the portable
implementation. [Arm CRC32 and PMULL intrinsics](https://arm-software.github.io/acle/neon_intrinsics/advsimd.html)

```sh
make -B CPPFLAGS=-DOCS_ROCE_PMULL_ICRC CFLAGS='-O3 -g -march=armv8-a+crc+aes'
```

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

### Standard MTU and BURST-style sweep

The paired sweep compares Ethernet MTU 1500 with a 1024-byte RoCE
payload against Ethernet MTU 4200 with a 4096-byte RoCE payload.
The latter matches the packet size used in [BURST §5](https://www.usenix.org/system/files/nsdi26-shen.pdf#page=10).
Both profiles use the same pinned binary and first physical path.
Each sample verifies 16 GiB in 1 MiB WRITEs. The sweep runs one QP
on one worker and 16 QPs on one, two, and four workers at both MTUs.
The TX burst and per-QP send window remain 16 and 256 packets.

```sh
python3 tests/check_spark_bandwidth.py --burst-style-bench \
  --destination "/opt/ocs-roce-bw/$REVISION" --sha256 "$BINARY_SHA256"
```

The installed `ocs-roce-bw-mtu-*` units use loopback control over
management SSH. The runner checks the expected packet count for each
profile and restores the interfaces' original MTUs after the sweep.
If a profile fails, it saves the failure and proceeds once both hosts'
lab services are idle.
Use `--mtu 4200 --profile multi-4` with this command to rerun just the
four-worker jumbo profile.

On November 23, 2026, the `a2c307f` build produced the following
requester medians on Spark-04 → Spark-02.

| QPs | Workers per endpoint | MTU 1500 Gbit/s | MTU 4200 Gbit/s |
| --- | --- | --- | --- |
| 1 | 1 | 29.024 | 41.643 |
| 16 | 1 | 23.733 | Failed |
| 16 | 2 | 43.380 | Failed |
| 16 | 4 | 90.018 | Failed twice |

Each completed profile verified 16 GiB in each of three trials.
One-QP trials at both MTUs completed with zero retries and about one
polling core per endpoint. Each jumbo attempt with 16 QPs ended in
retry exhaustion and Spark-04 logged mlx5 RX queue errors. The cause
of those driver errors remains open.

On November 24, the same build again verified 16 GiB for one QP at each
MTU, with requester rates of 28.928 Gbit/s at 1500 and 41.492 Gbit/s at
4200. The four-worker MTU 4200 repeat recorded five requester RX errors
and ended in retry exhaustion. Both hosts' physical CRC, oversize and
discard counters stayed at zero, and both interface MTUs returned to 1500.

### Separate application and worker bandwidth

The application sweep uses the same 16 GiB, 1 MiB WRITE workload, QP
counts, workers, send window and MTUs as the paired standalone sweep.
Use `--profile smoke` for an 8 MiB first transfer on the same path. The
service pins application threads to CPUs 10–13. The bandwidth report
measures the datapath interval and reports DPDK polling-thread CPU time.
The paired sweep uses the TCP lab setup for the workload and verification.

Stage the paired installers with the matching
per-host worker, application, provider and patched DPDK runtime. The input
directory contains `spark-04` and `spark-02`, each with its own
`SHA256SUMS`.

```sh
bundle=$(python3 tests/stage_spark_app_bandwidth.py "$REVISION" --inputs "$PINNED_INPUTS")
```

Copy the `spark-02` subdirectory to Spark-02 and run its `install.sh` with
sudo. On Spark-04, run `sudo /bin/bash "$bundle/spark-04/install.sh"`.
The services use a dedicated account with access to the two lab
registration devices. Load the signed `roce_mr` and `roce_uverbs` modules
before the native MR profiles. The memfd profile runs with the modules
absent.

```sh
python3 tests/check_spark_bandwidth.py --app-bench memfd-gather --profile smoke --samples 1 \
  --destination "/opt/ocs-roce-app-bw/$REVISION" --app-bundle "$bundle"
python3 tests/check_spark_bandwidth.py --app-bench mr-copy --app-bench mr-gather \
  --destination "/opt/ocs-roce-app-bw/$REVISION" --app-bundle "$bundle"
python3 tests/check_spark_bandwidth.py --app-bench mr-gather --profile short --samples 1 \
  --destination "/opt/ocs-roce-app-bw/$REVISION" --app-bundle "$bundle"
```

The short profile uses one QP and one DPDK worker at each endpoint.
It posts four 1,024-byte single-packet WRITEs at MTU 1500 and two
4,096-byte single-packet WRITEs at MTU 4200. The requester must observe
each verbs WC before it posts the following WRITE on the same open QP.
The runner verifies the mlx5 queue configuration and DMA-BUF mapping.

Each recorded sample requires distinct application and worker processes,
application completions and byte verification. The native modes require
Generic Netlink handoff and the native gather trial checks that mlx5 mapped
the DMA-BUF for the entire payload. The service writes the first mlx5 RX
error syndrome per queue to the journal if the PMD enters queue recovery.
[mlx5 PMD runtime options](https://doc.dpdk.org/guides-23.11/nics/mlx5.html#runtime-configuration)

The worker reports minor and major faults during polling. Setup diagnostics
report the elapsed time for application attach and DMA mapping separately.
The application bandwidth launcher also records elapsed time and process CPU
time in `application_transfer`, including producer threads and receiver byte
verification. The interval starts after memory registration and any in-band
connection setup, and ends when the application observes completion. It can
include time waiting for the worker's completion signal. For manual runs,
set `ROCE_DPDK_APP_BW=1` on the application.
With `--prefault-app`, the worker reads one byte per registered page before
the control exchange and reports the upfront cost. To compare both paths,
stage the updated worker with `--include-prefault` and add
`--compare-prefault` to the application bandwidth command. Use
`--prefault-only` to run the staged prefault variants after collecting
the baseline.

For the jumbo mlx5 investigation, stage with `--include-hugepages`.
Add `--compare-hugepages` to repeat the same MTU 4200 profile with the
normal 4 KiB backing and with 64 temporary 2 MiB pages for the DPDK
packet pools. Use `--hugepages-only` to run the MTU 4200 profile with
2 MiB backing alone. Use `--prefault-only` to hold application alias
prefaulting constant across the two trials. The launcher records the RX
mode per queue and the packet-pool chunk count. The hugepage service releases
its temporary reservation on exit. [DPDK hugepage requirements](https://doc.dpdk.org/guides-23.11/linux_gsg/sys_reqs.html#use-of-hugepages-in-the-linux-environment)

On November 24, 2026, the `8c5058a` four-worker, 16-QP memfd
scatter/gather profile at MTU 4200 verified 16 GiB per run. With 2 MiB
packet-pool backing, median requester payload goodput was 98.012 Gbit/s
across three runs (98.010–98.019 Gbit/s). Both endpoints reported zero
NIC errors or misses, and there were zero requester retries. With 4 KiB
packet-pool backing, two attempts hit mlx5 requester RX syndrome `0x04`
and exhausted retries. Both variants used the mlx5 Vector Neon RX path.
[Jumbo page-backing results](CONFORMANCE.md#spark-04-to-spark-02-jumbo-page-backing)

### DPDK and hardware RoCEv2 comparison

The earlier hardware benchmarks measured about 92.6 Gbit/s per path,
185.17–185.18 Gbit/s across two paths in one direction, and 370.16 Gbit/s
across both paths and both directions. These are the 1 MiB results from
Spark-04 (`spark-7e64`) with Spark-01 (`spark-28a3`) and from
Spark-02 (`spark-4ed9`) with Spark-03 (`spark-016e`). Both runs used the
two PCIe functions for the right QSFP port at each endpoint. Each path
used one QP at Ethernet MTU 1500 and RoCE PMTU 1024. The archived
endpoint metadata records `speed_mbps=200000` for each interface.
[Earlier two-path hardware benchmarks](../summary.md),
[first pair link metadata](../results/20261110T021852Z-4cacd141-client/endpoints.json),
[second pair link metadata](../results/20261112T023026Z-7a1c5c7f-client/endpoints.json)

The table uses requester payload rates from Spark-04 to Spark-02 on
their first path, which reported 100 Gbit/s. Each successful DPDK trial
verified 16 GiB of 1 MiB WRITEs. The ConnectX-7 control used the same
WRITE size and perftest 6.30 with four-second measurement windows as
described below.
MTUs 1500 and 4200 used PMTUs 1024 and 4096 respectively.

| DPDK build and mode | QPs | Polling workers per endpoint | DPDK MTU 1500 Gbit/s | DPDK MTU 4200 Gbit/s | Hardware MTU 1500 Gbit/s | DPDK share at 1500 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `8b075bb` separate processes, memfd scatter/gather | 1 | 1 | 24.619 | 30.690 | 92.600 | 26.6% |
| `8b075bb` separate processes, memfd scatter/gather | 16 | 1 | Failed | Failed | 92.320 | — |
| `8b075bb` separate processes, memfd scatter/gather | 16 | 2 | 37.165 | Failed | 92.320 | 40.3% |
| `8b075bb` separate processes, memfd scatter/gather | 16 | 4 | 84.577 | Failed | 92.320 | 91.6% |
| `3a6a1ca` standalone DPDK | 16 | 4 | 90.506 | — | 92.320 | 98.0% |
| `8c5058a` separate processes, prefaulted memfd scatter/gather | 16 | 4 | 84.272 | 98.012 | 92.320 | 91.3% |

The rates are three-trial medians. Rows marked failed stopped after the
first unsuccessful trial for that MTU. The `8c5058a` jumbo result used
2 MiB packet-pool backing. The matched hardware control used MTU 1500.
The fastest four-worker `8b075bb` trial reached 85.795 Gbit/s. The earlier
134.99 Gbit/s result used local memif on Spark-04.

The standalone DPDK run reached 92.568 Gbit/s with both logical paths
active in one direction on Spark-04 → Spark-02. The two paths use the
left QSFP port on Spark-04 and the right QSFP port on Spark-02. All four
interfaces reported 100,000 Mb/s in sysfs during the DPDK and matched
hardware tests. The hardware control reached 92.340 Gbit/s. On
November 24, 2026, `ethtool` and sysfs again reported 100,000 Mb/s at
both endpoints of the app benchmark (`enp1s0f0np0` on Spark-04 and
`enp1s0f1np1` on Spark-02). Both interfaces advertised 200 Gbit/s modes
with auto-negotiation enabled. The measurements establish the reported
speed of that connection at test time. The reason it came up at
100 Gbit/s remains unverified. A DPDK sweep with the link reporting
200 Gbit/s is pending in both one-way and bidirectional modes.
[Physical dual-rail sweep](#physical-dual-rail-sweep)

In single-sample prefault runs at MTU 1500, four workers reached
84.899 Gbit/s with zero retries and misses. The 16-QP single-worker
profile completed at 25.373 Gbit/s with 33,583 retries. The prefaulted
four-worker jumbo trial hit requester mlx5 RX syndrome `0x04` and
exhausted retries. See the [conformance ledger](CONFORMANCE.md) for
the diagnostic counters.

### Physical dual-rail sweep

The native runner accepts a local perftest 6.30 binary and checks the
RoCEv2 GIDs and current Ethernet MTU on both Sparks. It records the
link speed reported by each interface and requires the speeds to
agree among the selected endpoints. It measures each rail separately
and both rails simultaneously, with separate one-QP and batched
16-QP controls.

```sh
python3 tests/check_spark_native_bandwidth.py --perftest "$PERFTEST_BIN"
```

The matched native control used three trials per configuration and the
middle four seconds of each eight-second perftest 6.30 run.

| Active interface paths | Native one-QP Gbit/s | Native 16-QP Gbit/s |
| --- | --- | --- |
| First rail only | 92.600 | 92.320 |
| Second rail only | 92.640 | 92.330 |
| Both concurrently | 92.640 | 92.340 |

The batched native samples overlapped for at least 3.65 seconds and
each rail was under load throughout both measurement windows. The
two PCIe functions on each Spark reach the same QSFP port. The
measured usable aggregate for this lab link is 92.34 Gbit/s with
16 QPs per rail. The first-rail DPDK result reached 98.0% of its
matched native rate. [NVIDIA DGX Spark port mapping](https://docs.nvidia.com/dgx/dgx-spark/spark-clustering.html)

With the pinned aggregate services installed and all four data
functions in raw Ethernet mode, add `--aggregate` to the DPDK runner.
Each rail uses 16 QPs and four polling cores per endpoint and verifies
16 GiB. The lab TCP connections use loopback SSH forwards over the
management link. The requester option `--start-gate PORT` synchronizes
release across both sender processes on Spark-04. The aggregate metric
uses 32 GiB over the interval from the first sender start to the last
sender completion. The report records overlap and start skew.

The `a2c307f` build verified 32 GiB in each of three concurrent runs
on Spark-04 and Spark-02 at PMTU 1024 and Ethernet MTU 1500. Aggregate
requester goodput was 92.566, 92.580, and 92.568 Gbit/s. The median
of 92.568 Gbit/s matched the 92.340 Gbit/s native control. Each
endpoint used about eight polling cores across the two functions.
The sender windows overlapped by at least 99.97%, with at most
0.099 ms start skew. Total requester retries across both rails were
two, two, and zero. The last run verified exactly 16,777,216 data
packets per rail with zero DPDK misses or packet errors.

### Three-Spark ring sweep

Once the two-Spark target is verified, install the pinned ring services
on Spark-04, Spark-02, and Spark-01. The ring uses the first PCIe path
on each cable. Each Spark sends on `0000:01:00.0` and receives on
`0000:01:00.1`, with both functions in raw Ethernet mode.

```sh
python3 tests/check_spark_ring_bandwidth.py \
  --destination "/opt/ocs-roce-bw/$REVISION" --sha256 "$BINARY_SHA256"
```

Each directed edge verifies 16 GiB using 16 QPs, PMTU 1024, and
four polling cores per endpoint. The runner uses the management
links for TCP setup and releases all three senders after every
receiver is ready. It records each edge's rate, CPU and retries, and
a lower bound on the interval when all three transfers were active.

On November 23, 2026, the `a2c307f` build verified all 48 GiB in
each of three trials. The three sender windows overlapped for at
least 1.518 seconds in every trial. The six Ethernet interfaces
reported 100 Gbit/s and MTU 1500. Requesters used 3.996–4.000 polling
cores and responders used 4.000 each.

| Edge | Median TX Gbit/s | Median RX Gbit/s | Requester retries per trial |
| --- | --- | --- | --- |
| Spark-04 → Spark-02 | 90.237 | 90.269 | 3, 0, 0 |
| Spark-02 → Spark-01 | 47.829 | 47.844 | 1, 1, 0 |
| Spark-01 → Spark-04 | 12.290 | 12.291 | 0, 0, 0 |

One isolated 16 GiB check per slower edge used the same build, QP
count, and CPU assignments. Spark-02 → Spark-01 verified 89.896 Gbit/s
with 13 retries. Spark-01 → Spark-04 verified 12.934 Gbit/s with zero
retries, RX misses, or packet errors. The cause of that path's lower
throughput remains open.
