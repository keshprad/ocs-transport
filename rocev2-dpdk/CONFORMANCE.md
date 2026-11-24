# RoCEv2 conformance and interoperability

The target for this implementation is [IBTA InfiniBand Architecture
Specification, Volume 1, Release 2.1](https://infinibandta.org/ibta-specification/),
Annex A17 and the base Reliable Connected transport. The initial audit covers
one IPv4 QP, one nonempty RDMA WRITE at a time, and host buffers on one Ethernet
path. The TCP setup and byte-verification exchange are specific to the lab
tools.

The current Release 2.1 text requires IBTA access. This initial mapping uses
the historical [Annex A17](https://pdfcoffee.com/annex17-rocev2-pdf-free.html)
and [Volume 1 Release 1.2.1](https://www.afs.enea.it/asantoro/V1r1_2_1.Release_12062007.pdf)
clause numbering. Reconcile the IDs and applicability with Release 2.1 before
claiming conformance to that revision. Use [Linux RXE](https://github.com/torvalds/linux/tree/master/drivers/infiniband/sw/rxe)
to check implementation behavior and run independent interoperability tests.

## Requirement-to-evidence ledger

**V** means the named tests directly verify the listed scoped behavior.
**P** means the behavior has partial evidence or a narrower implementation.
**H** means independent endpoint evidence is still required.

| Source | Scoped behavior | Status and evidence |
| --- | --- | --- |
| A17.3.1–A17.3.2, CA17-20–21 | IPv4 UDP encapsulation, destination 4791, length including iCRC, stable source port per QP | **V**. `WIRE` compares complete IPv4 frames against an independent encoder. The single-worker profile derives the source port from the local and remote QPN. The multi-worker profile uses the local QPN so each peer can steer its incoming QPs to the intended queues. |
| A17.3.3–A17.3.4 | Compute the invariant CRC and drop bad frames | **V** for the supported packet shapes. `WIRE` checks independent CRCs for WRITE and ACK encodings, tolerates changes to masked IPv4 fields and rejects truncation and corruption. `PEER` checks that a corrupted WRITE does not advance the live responder. |
| A17.4.1–A17.4.4, §9.6 | Match IPv4 address/QP/partition and use the IPv4-mapped GID on the verbs peer | **P**. `RC` checks wrong peer IP, QPN, and P_Key on feedback. Native control verifies setup for the active IPv4 GID. Broader routing, VLAN, and IPv6 coverage is open. |
| §§5.2, 9.2, 9.4.3, C9-18–20 | RC WRITE Only and First/Middle/Last segmentation, RETH placement, AETH response, and padding | **V** for nonempty plain WRITE. `WIRE` compares all five packet layouts. `RC` checks first, middle and unaligned last geometry and `MEMIF` verifies reassembly at the negotiated PMTU. |
| §§9.7.1, 9.7.4–9.7.5, C9-105, C9-121–122 | PSN increments and wrap, gap NAK with expected PSN, duplicate ACK without replaying placement | **V** in `RC` and `PEER`. The live responder returns the missing PSN in its NAK and returns the same cumulative ACK and MSN for a duplicate after wrap. Byte verification detects any duplicate placement. |
| §§9.7.2, 9.7.5.2, 9.7.6, 9.7.7.1, 9.9.2 | Cumulative ACK progress, MSN and error syndromes, timeout and bounded Go-Back-N retry | **P**. `RC` asserts cumulative progress, stale and unsent feedback rejection, MSN, access errors, and retry exhaustion. `PEER` confirms the live requester resumes from the missing PSN after a NAK, then retransmits from the same PSN when the final ACK is withheld. Only feedback applicable to plain WRITE with no receive credits is exercised. The endpoint defaults to a 32-packet window and 10 ms local timeout. `RC` also verifies that packet preparation and a partial TX burst advance only the PSNs accepted by the driver. |
| §§9.7.4–9.7.5 | Validate the remote key, address range, and message geometry before placement | **V** for the configured host region. `RC` checks incorrect keys, below-base and overrun addresses, and inconsistent message lengths with no memory modification on first-packet rejection. |
| §§12.6.6, 12.7.32, 12.9.7, 12.9.8.5 | CM Message Receipt Acknowledgment (MRA) format and retry deferral | **V** for the lab RC REQ and REP paths. `CM` verifies peer deferral, transaction matching, accepted TX, and replay. The DPDK worker sends MRA while waiting for slow application setup. The exact-kernel guest verifies that behavior with and without packet loss. |
| A17 and Chapter 9 end to end | DPDK-to-native and native-to-DPDK WRITEs with a stock ConnectX-7 endpoint | **V** for the tested one-QP IPv4 host-memory WRITE. Two runs verified 8,195 bytes in each direction at PMTU 1024 with raw Ethernet on the DPDK function and native RoCE on the verbs peer. |

Zero-length WRITE, WRITE with Immediate, SEND, READ, atomics, IPv6 and
RoCE congestion management remain open. The BURST evidence below covers
the supported RDMA CM and DPDK-side verbs provider profiles.

## Reproducible evidence

| ID | Command from this directory | Result |
| --- | --- | --- |
| WIRE | `make check` → `tests/check_wire.py` | Independent BTH/RETH/AETH and iCRC vectors for five opcodes, plus negative decode cases |
| RC | `make check` → `tests/test_roce` | Single- and multi-packet placement, PSN wrap, loss and duplicate recovery, cumulative ACK validation and retry limit |
| CM | `make check` → `tests/test_roce_cm_wire` and `tests/test_roce_cm_session` | Fixed CM MAD vectors, QP1 encapsulation, establishment and teardown, replay, REQ/REP retry timing, and outgoing MRA while the final response is held |
| MEMIF | `make check-memif` | Two production DPDK processes verified a WRITE Only, PMTU negotiation from 1024 and 512, and 65 consecutive 8,195-byte WRITEs on one QP. Separate tests verified WRITE Only and repeated 1,048,579-byte WRITEs across 16 independent QPs. With four available CPUs, a two-queue test verifies WRITEs on three QPs. |
| PEER | `make check-peer` → `tests/check_peer.py` | Independent Python RC peer and production DPDK endpoint exchanged a 515-byte WRITE in each role at PMTU 256. The peer checked every WRITE segment and each response. The test injected a sequence gap, lost ACK, corrupted iCRC, and changed-payload duplicate. |
| PEER-VERBS | `python3 tests/check_peer.py --verbs-provider "$PROVIDER_DIR/libroce_dpdk"` | On the exact Spark-04 kernel in QEMU, the independent peer sent a sequence NAK and withheld the final ACK. It verified both retransmissions from a native scatter/gather requester and checked the payload. The application received one libibverbs completion. Netlink handoff was required. |
| SPARK | [Spark wire interoperability](README.md#spark-wire-interoperability) | Native controls passed both ways. Two mixed runs each verified all 8,195 bytes in both directions on ConnectX-7 at PMTU 1024. |

### Spark hardware verification

On November 23, 2026, the native control used Spark-04 (`spark-7e64`) and
Spark-02 (`spark-4ed9`). The ConnectX-7 ports reported active MTU 1024 with
the IPv4-mapped RoCEv2 GID at index 3. Both processes exited successfully.
The native sender got a verbs completion and the receiver verified all 8,195
bytes in registered memory.

DPDK 23.11.7 with mlx5 detected PCI function `0000:01:00.0` on Spark-04.
DevX failed to create a TIS object (`errno=121 status=0x3 syndrome=0x6a6678`).
The account required administrator-installed services before the mixed
hardware tests could run. See the
[mlx5 runtime requirements](https://doc.dpdk.org/guides-23.11/platform/mlx5.html#run-as-non-root).

A follow-up probe on Spark-04 reproduced the same TIS failure with both the
Verbs and Direct Verbs steering modes (`dv_flow_en=0` and `dv_flow_en=1`).
Both probes stopped before Ethernet port setup and sent no test traffic.

The installed MRC smoke services provide a separate physical DPDK baseline.
Using the root-owned snapshot for `32d67ee5464821c0ad19fbc8e35caa1a5bd3927f`,
Spark-04 → Spark-02 → Spark-01 → Spark-04 each completed and verified a
256 MiB WRITE on UDP 4971 across both rails. The three receivers counted
131,396 / 130,748, 131,052 / 131,092, and 131,356 / 130,788 segments
respectively. This confirmed the lab links and mlx5 initialization under the
existing service capabilities. The prepared RoCEv2 services use the same
capabilities and runtime on Spark-04, with a separate binary and UDP 4791.
The pinned native-verbs test binary subsequently passed the 8,195-byte
control at PMTU 1024 in both directions between Spark-04 and Spark-02.

With the administrator-installed test services, DPDK 23.11.7 opened
`0000:01:00.0` on Spark-04. In the tests with native RoCE enabled there,
DPDK TX emitted 72 packets for a nine-segment WRITE and received zero
ACKs. In reverse, the Spark-02 verbs sender reported retry exhaustion
(vendor error `0x81`), and DPDK saw zero RX packets.

An immediate retest produced the same results. Spark-02's RDMA unicast RX
counter rose by 72. Spark-02's RDMA unicast TX and Spark-04's RDMA unicast
RX counters both rose by 134 over the two directions. Neither port reported
physical RX discards. These observations support mlx5 classifying inbound
UDP 4791 as native RoCE ahead of the DPDK Ethernet queue. See
[NVIDIA's mlx5 steering guidance](https://docs.nvidia.com/networking/display/bfswtroubleshooting/mlnx-dpdk.pdf).

After setting `enable_roce=false` and applying `driver_reinit` to
Spark-04's `0000:01:00.0` only, its Ethernet data link came back with
`10.61.0.2/30`. Spark-02 kept native RoCE on `rocep1s0f1`, port 1,
GID index 3, at `10.61.0.1`. TCP control used the separate rail.
The installed binaries were built from `936d7d1`. The Spark-04 DPDK binary
SHA-256 was `e5b8e6fcb94c9fa46a5dc62324b9820b8d091f378d305a944006136ab025085b`
and the Spark-02 native peer SHA-256 was
`4a440cd887a7ee90a6b7556a60d31d1e7313a79f7eeea9c0cf445278e397674b`.

| Spark-04 role | Result in both runs | Spark-04 journal invocation IDs |
| --- | --- | --- |
| DPDK requester | Completed 8,195 bytes in nine packets with zero DPDK retries. The Spark-02 receiver verified every byte in native registered memory. | `395c843694684aba91374e5cdb2b3d12`, `a163e2bef65742fabd7e0ab3528e4763` |
| DPDK responder | Verified 8,195 bytes in nine packets. The Spark-02 sender reported a successful native verbs WRITE completion and peer verification. | `2d54c3b6a2ce456eab8e80f51e576479`, `e200ded5d6ed46c38493e8e402acbde0` |

Both services exited successfully and were inactive after the repeat.

## BURST implementation notes

Shen et al., [*BURST* (NSDI 2026)](https://www.usenix.org/system/files/nsdi26-shen.pdf),
is a design and benchmarking reference. The IBTA clauses govern interoperability.

- **§4.6.** Use cumulative ACKs and Go-Back-N for the hardware-compatible
  path. BURST's selective-repeat variant is an extension for software peers.
  Keep the standard RC retry and completion behavior in hardware tests.
- **§§4.3–4.4.** Profile packet assembly, CRC, and receive placement separately.
  BURST uses DPDK scatter/gather for transmit and Intel DSA for receive copies.
  The Spark CPU-copy baseline uses Arm CRC instructions and a per-core
  mbuf cache. See [bandwidth probes](README.md#bandwidth-probes) for the
  measurements. The native gather path chains worker-owned headers and
  iCRC with the application payload. Spark RX placement uses CPU memcpy.
- **§5.1 and Appendix A.7.** Report QP count, batch size, and core usage with
  throughput. BURST's single-QP result is about 121 Gbit/s in its x86/400G
  testbed. The DPDK test supports 16 QPs and independent RC state.
  The multi-queue memif test covers independent polling cores per
  endpoint. The physical first-rail sweep verified three 16 GiB runs
  at 90.506 Gbit/s median with 16 QPs and four mlx5 queue pairs. Median
  requester and responder CPU use was 3.978 and 4.000 polling cores.
  Requester retries were 61, 73, and 147. The paired systemd invocation
  IDs for the median result are `9bf2eaf0c60b42f6814d0264ef5e5a41`
  and `5988fd39a3c9489b983a9bd35ed60a40`.
- The paired MTU sweep verified 16 GiB per single-QP trial at both
  PMTU 1024 and PMTU 4096, with zero retries in all six trials. At
  PMTU 4096 each trial verified 4,194,304 data packets. See the
  [standard and BURST-style results](README.md#standard-mtu-and-burst-style-sweep)
  for the multi-QP jumbo failures.
- The dual-rail physical sweep reached 92.568 Gbit/s verified median
  with 16 QPs and four mlx5 queue pairs per rail, matching the
  92.340 Gbit/s native control. The median sample verified 16 GiB per
  rail with zero retries. Its TX/RX invocation IDs are
  `dbe800735ce0439e9a39e61b28d2a452` / `6b815e9187ce4f6198337dc254587319` and
  `e6e831349cb1421794ab63636bed2388` / `bf95e952be92437d88b23f471e3b35a0`.
- The concurrent three-Spark ring verified 16 GiB per edge in each of
  three trials at PMTU 1024 with 16 QPs and four polling workers per
  endpoint. Sender windows overlapped by at least 1.518 seconds.
  See the [ring results](README.md#three-spark-ring-sweep). The
  paired TX and RX invocation IDs for each median edge are recorded
  below.
- **§§4.1 and 4.7.** The verbs provider and user-space CM are useful references
  if we extend application compatibility after the wire milestone.

| Ring edge | Median TX invocation | Receiver invocation |
| --- | --- | --- |
| Spark-04 → Spark-02 | `98f6e058ed304946878030553075fba2` | `c01f973e8ea8410faddbfb0d4687d891` |
| Spark-02 → Spark-01 | `419e6d18fc8f491c805a8575978b11df` | `368182b74fa841babdcda9bfd1b598bb` |
| Spark-01 → Spark-04 | `ed8a707852434029a2ada50c2b6720de` | `f95732fc56b840ba9aeece9c34743aa3` |

## BURST service implementation

| Reference | Component | Current evidence |
| --- | --- | --- |
| Figure 5(a), §4.1 | Separate application and DPDK worker processes | `make check-app` verifies an 8,195-byte WRITE and 3,145,764 bytes across four QPs and two workers. Each endpoint uses an application-owned shared buffer. The receiving application checks every byte before peer verification. |
| Figure 5(a), §4.1 | Kernel registration and worker mappings | `/dev/roce_mr` pins caller pages and sends a single-use Generic Netlink ticket bound to the worker. The worker imports a CPU mapping and can export its lease as a DMA-BUF. The exact Spark-04 kernel guest verified process authorization, retry after FD exhaustion, duplicate import rejection, cancellation, and expiry. The provider lifecycle and both copy and scatter/gather WRITE tests passed with Netlink required. The same binaries passed on the older kernel module using SCM_RIGHTS. |
| Figure 5(a), §4.1 | Standard verbs contexts and resources | Stock libibverbs found `roce_dpdk0` in the Spark-04-kernel guest. The MR fixture verifies range and access validation, process-scoped keys, deregistration, and application exit with an outstanding worker lease. The CM applications use libibverbs polling CQs and RC QPs. Provider checks cover unmatched QPN and wrong-PD rejection, successful rebinding after a rejected configuration, and `EBUSY` for referenced MR, PD and CQ resources. |
| Figure 5(b), §4.2, Appendix A.1 | WQE notification and completion | Worker queues consume shared WQEs from native `ibv_post_send()` and publish individual results for `ibv_poll_cq()`. The provider fixture verifies partial linked-list posting, CQ capacity, queue closure and error WC mapping. With Netlink required, the exact Spark-04-kernel guest verified native copy and scatter/gather on one QP and native copy on four QPs across two workers. Each QP posted one WRITE at a time and returned all three signaled completions. |
| Figures 7–8, §§4.3–4.4 | Scatter/gather TX and CPU RX placement | The requester chains worker-owned headers and trailer around the application payload. Independent wire vectors cover PMTU 4096. The RC peer verifies sequence NAK and timeout retries. With standard verbs registration on both ends, a four-process memif test verified 24,585 bytes in both copy and scatter/gather modes before deregistration. RX copies into the MR before acknowledging. The DPDK 23.11.7 mlx5 patch builds, and the worker resolves its DMA-BUF map/unmap APIs. Physical ConnectX-7 DMA from native MRs remains unverified. |
| Figures 5(c) and 9, §4.7 | Standard RDMA CM over the DPDK datapath | `make check` verifies wire format, peer and transaction identity, TX backpressure, setup and disconnect retries, crossed DREQs, DREP replay, and the REP and RTU gates. The single-QP lab adapter forwards standard librdmacm calls over an authenticated Unix socket. CM-owned QPs use `rdma_create_qp()` with a provider PD and polling CQs. The worker hands back the queue FD, waits for QP readiness, and sends REQ, REP, RTU, DREQ and DREP on DPDK queue 0. The exact Spark-04 kernel guest exercised CM-owned and external QPs. TCP carries the lab's WRITE completion and verification. Stock librdmacm verified WRITE and disconnect in both directions with Linux RXE and physical ConnectX-7 at MTUs 1500 and 4200. |

The registration socket carries the request, the correlation cookie
and the assigned keys. The kernel resolves the Unix peer and binds the
ticket to that worker's process and namespaces. The ticket identifies
the mapping while the exporter holds the kernel file descriptor.
[BURST design](https://www.usenix.org/system/files/nsdi26-shen.pdf#page=6)

The stock libibverbs provider fixture on the Spark-04 kernel checks
three QPs sharing a four-entry polling CQ. It verifies aggregate posting
credits, partial posting with `bad_wr`, completion identity for duplicate
WR IDs on different QPs, ordered success and error completions within each
QP, and continued polling after one QP is destroyed. The CQ stays busy
until the last QP detaches. The WRITE path reserves CQ capacity at posting
time to cover possible errors. Asynchronous CQ notifications and overrun
events remain outside the polling profile.
[IBTA Volume 1 §§10.2.6, 10.8.5](https://www.afs.enea.it/asantoro/V1r1_2_1.Release_12062007.pdf)

With Netlink handoff required, the Spark-04-kernel provider fixture
reused an open context, PD, MR and polling CQs across separate native queue
bindings. It rejected a replacement while a QP was attached, kept the old
WR counters intact during a WRITE on the new queue, and replaced a
binding with unused slots after its last QP was destroyed. A disconnected
application's MR could no longer be acquired. The broker kept the key
reserved until the worker's final lease released the mapping. The same
build passed a CM-owned scatter/gather transfer with four ordered
1,024-byte WRITEs and clean resource teardown.

The native provider rejects requests that overrun the local MR or cross
the destination QP's slice. The worker validates those bounds again
and snapshots the accepted addresses before admitting the WR. In the
Spark-04-kernel guest, a CM-owned QP posted three 8,195-byte WRITEs in
reverse address order at PMTU 1024. Copy and scatter/gather each placed
the payload at offsets 16,390, 8,195, and 0, with three ordered WCs and
24,585 verified bytes. The wire trace preserved the RETH addresses after
the test rewrote the shared WQEs after admission. In scatter/gather the
peer returned a sequence NAK after the test dropped the first WRITE
packet. The retransmission kept the original destination and payload.
Both runs required Netlink and completed resource teardown. The same
build passed the four-QP, two-worker stock TAP test at PMTU 4096 with
2,084 ordered WCs and 17,078,380 verified bytes.

The native send queue accepts different nonempty WRITE lengths within
its negotiated byte budget. The worker assigns each admitted WR a packet
range and advances the verbs completion cursor only when the ACK covers
the entire range. The RC unit test verifies WRITE Only and First/Middle/Last
segments across PSN wrap. In the Spark-04-kernel guest at PMTU 256, three
WRITEs of 255, 515, and 256 bytes produced five packets and verified all
1,026 bytes. The serial copy run completed each WR before admission of
the next. The scatter/gather run recovered the same RETH and payload
after loss and a sequence NAK. Both returned ordered WCs with the
individual WRITE lengths. A separate stock TAP run at PMTU 4096
verified 2,084 ordered WCs across four QPs and two workers.

Selective WRITE signaling keeps the per-WR flag in the provider. The
worker retires all WRs after the covering ACK and TX reference release.
The fixture verified that successful unsignaled work releases posting
credits without emitting a WC. `sq_sig_all` reports success without
individual flags. Remote-access and flush errors generate WCs with or
without a signal flag. QP destruction can reclaim a fully retired,
successful unsignaled suffix without discarding visible completions.
[IBTA Volume 1 §§10.8.5–10.8.6](https://www.afs.enea.it/asantoro/V1r1_2_1.Release_12062007.pdf), [Linux RXE completion generation](https://github.com/torvalds/linux/blob/v6.17/drivers/infiniband/sw/rxe/rxe_comp.c#L408-L431)

A separate application signaled only the last WR in each linked post.
With required Netlink and the production DPDK worker on the Spark-04
kernel, four QPs each posted 521 scatter/gather WRITEs, wrapping their
256-entry send queues twice. Each QP returned exactly the 19 expected
WR IDs in order. The receiver verified 17,078,380 bytes with zero RC
retries, and all application resources were released.

A separate four-QP scatter/gather application used one CQ with the
production DPDK worker on that kernel. It posted 129 WRITEs per QP
before polling, verified all 516 completions in order within each QP,
and verified 4,228,620 bytes at the receiver. The transfer had one RC
retry. Both applications released their MRs and PDs, and the requester
destroyed all four QPs before releasing the shared CQ.

The CM wire tests cover fixed 256-byte MADs in Volume 1 Chapter 12
and the QP1 transport described in
Annex A17 §A17.6.1.1. They check the UD opcode, destination and source QPs,
Q_Key, length, IPv4 checksum and iCRC against independent vectors. QP1 UD
receive also accepts the AckReq bit seen in Linux RXE GSI frames. The
encoder leaves it clear. The QP uses its announced CM Starting PSN for
receive and the peer's advertised Starting PSN for transmit, as specified
by Volume 1 §12.7.31.
[IBTA Volume 1](https://www.afs.enea.it/asantoro/V1r1_2_1.Release_12062007.pdf)
[Linux CM field definitions](https://codebrowser.dev/linux/linux/include/rdma/ibta_vol1_c12.h.html)

An incoming MRA for an outstanding REQ or REP starts a wait for
`4096 ns × 2^ServiceTimeout` plus the configured packet lifetime. The
direct-link lab uses a 1 ms packet lifetime and a 250 ms regular retry
interval. The worker checks both communication IDs once known and
matches the transaction and peer before deferring retries. Duplicate
MRAs leave the current wait unchanged. An accepted retransmission starts
the regular retry interval again. The worker timestamps CM sends after
DPDK accepts the packet and rechecks the exchange deadline after waiting
on the application. The disconnect loop refreshes its clock when it
enters TIMEWAIT so it preserves the DREP replay window.
[IBTA Volume 1 §§12.6.6, 12.7.32, 12.9.7, 12.9.8.5](https://www.afs.enea.it/asantoro/V1r1_2_1.Release_12062007.pdf)

In the exact Spark-04 kernel guest, a memif TX test shim delayed REP
or RTU by 550 ms. With an injected MRA, the other endpoint sent the
pending REQ or REP once. Each control with the same delay and no MRA
sent it three times before the reply. All four trials used separate
`librdmacm` applications, CM-owned QPs, Netlink MR handoff and
scatter/gather TX. Each returned three successful libibverbs completions
and verified 24,585 bytes with zero RC retries.
The updated implementation also verified an 8,195-byte WRITE and clean
in-band CM teardown in both directions against stock Linux RXE over TAP
at PMTU 4096 in the same kernel guest. That interoperability run used a
separate DPDK application with Netlink MR handoff.

During `librdmacm` application setup, the DPDK worker polls QP1 while
waiting for ACCEPT, QP_READY, or ESTABLISH on the Unix socket. It holds
REP or RTU until the application is ready and polls at up to 1 ms
intervals while the socket is idle. It schedules MRA halfway
through the applicable REQ timeout budget and allows immediate MRA for
budgets of 2 ms or less. The advertised service timeout covers the
remaining lab setup deadline. A duplicate REQ or REP while the worker
is still waiting can trigger replay of an accepted MRA.

In the exact Spark-04 kernel guest, the prompt application exchanged
REQ, REP and RTU with no MRA. Three separate trials delayed ACCEPT,
QP_READY, or ESTABLISH by 550 ms. Each emitted one MRA and the peer
sent its pending message once. Two trials delayed the application by
2.3 seconds and dropped the first outgoing MRA, once in each role. A
single peer retry elicited the replay in each trial. All six trials
verified 24,585 bytes and three native WRITE completions with zero RC
retries. Both kernel modules unloaded cleanly.

Packet captures with a stock Linux RXE peer confirmed both directions at
PMTU 4096. With a 1.2-second DPDK application delay before ESTABLISH, RXE
sent one REP after the change and five with the previous build. With a
5.3-second delay before ACCEPT, RXE sent one REQ after the change and
two with the previous build. The updated worker sent one MRA in each
role. Every run verified an 8,195-byte WRITE and clean CM disconnect.

The kernel guest used QEMU TCG with networking disabled and the Ubuntu
`6.17.0-1021.21-nvidia` image. Both `check_provider` and `check_mr`
exited successfully. Spark hosts still require trusted signatures for
the out-of-tree modules. The same sources also built against the
Spark-02 `7.0.0-1025-nvidia` headers.

With Netlink ticket handoff required, the guest ran the Figure 9 CM
sequence in both single-QP copy and scatter/gather tests. Each transferred
24,585 bytes in 27 packets with zero RC retries and three successful
libibverbs completions. The trace showed each application creating its
QP and passing that QPN into CM. The same QPs moved to RTR and RTS before
`rdma_accept()` or `rdma_establish()` and were destroyed after
`DISCONNECTED`. A separate four-QP, two-worker run verified
98,340 bytes and three completions per QP using TCP lab setup. All
endpoints exited successfully and both kernel modules unloaded.

Packet-loss injection at the DPDK memif burst interface separately
dropped the first RTU, the first DREP, and both. The initial validated
RC WRITE confirmed the passive connection after RTU loss. A duplicate
DREQ elicited the same DREP transaction after DREP loss. Each trial
verified 24,585 bytes with zero RC retries and three native CQEs. The TCP
trace contained only WRITE completion and verification bytes. DREP
replays are capped by the CM retry count. The lab uses that retry horizon
for TIMEWAIT. The IBTA timer uses path lifetime and peer ACK delay.
[IBTA Volume 1 §§12.9.7–12.9.8](https://www.afs.enea.it/asantoro/V1r1_2_1.Release_12062007.pdf)

Two further exact-kernel trials exercised RC loss with CM-owned QPs,
Netlink-backed native MRs and scatter/gather TX. Dropping a three-segment
WRITE MIDDLE mbuf provoked a sequence NAK for that PSN. Dropping the final
positive ACK provoked a timeout, a duplicate final WRITE and a replay of
the cumulative ACK. Each run needed one retry and verified 24,585 bytes.
The requester had acknowledged and retired two of three WRITEs before
processing the final ACK. Each run returned three ordered successful
libibverbs completions after their covering ACKs.

The pinned mlx5 PMD normally requests a TX CQE after a threshold of
32 mbufs. A sender that posts one short gathered WRITE and waits for its
WC can leave the last payload reference behind that threshold. Reading TX
descriptor status consumes CQEs the NIC has already produced. The native
requester now opts its TX queues into signaling the last WQE of each
productive doorbell, and the PMD checks CQ space before accepting a burst.
The worker waits for final WRITE retirement before it ends a successful
transfer. Retirement requires both a covering RoCE ACK and release of
the last DPDK reference to the WRITE payload.

The patched shared-ibverbs mlx5 PMDs built on Spark-04 and exported the
queue configuration API. A driver-level check exercised default short
bursts, the opt-in signal and accepted-mbuf count, and CQ counter wrap with
inline TX. In the Spark-04-kernel guest, a partial burst accepted one
packet, followed by a zero-acceptance burst. The requester resumed at the
first unsent PSN, completed three WRITEs in order, and verified 24,585 bytes
without RC retries. In a separate run, a fault shim retained the final
WRITE's external payload reference for 20 ms after the peer ACK. The
worker kept that WRITE pending until the shim released the reference,
then retired it before port shutdown.

The short native application profile posted four 1,024-byte WRITEs at
PMTU 1024 over memif and two 4,096-byte WRITEs at PMTU 4096 over stock
TAP. Both Spark-04-kernel guest runs required Netlink registration and
used a single CM-owned QP. The verbs trace records each WC before the
next call to post a WRITE. All WR IDs matched, both peers verified the
payloads with zero retries, and the jumbo trace captured two 4,170-byte
WRITE_ONLY Ethernet frames. Resource teardown and module unloads passed.
The Spark hardware profiles check the same posting pattern and require
the mlx5 queue configuration and DMA-BUF registration. Physical CQ
generation from these queues still requires verification.

Two exact Spark-04-kernel guest trials verified terminal error status and
teardown with native scatter/gather queues. A cumulative remote-access
NAK for the second WRITE produced `SUCCESS`, `REM_ACCESS_ERR` and
`WR_FLUSH_ERR` in order. With the first WRITE acknowledged and later ACKs
withheld, seven retries produced `SUCCESS`, `RETRY_EXC_ERR` and
`WR_FLUSH_ERR`. Both apps in each trial destroyed their QPs and CQs,
deregistered their MRs and deallocated their PDs before exit. Required
Netlink registration and all module unloads passed.
[Linux RXE completion handling](https://github.com/torvalds/linux/blob/v6.17/drivers/infiniband/sw/rxe/rxe_comp.c)

On November 24, 2026, the Spark-04 requester mapped a 12,288-byte
application memfd buffer for mlx5 scatter/gather DMA. The Spark-02
application verified the 8,195-byte WRITE over nine packets at PMTU 1024.
The requester reported zero retries and both ports reported zero errors.
These were the `ocs-roce-gather-smoke.service` invocations
`aa9d8d665c6e4d438fe9a5ba77aa02bc` on Spark-04 and
`60b8327c324149768abfe95a88113e3f` on Spark-02. The smoke test used the
memfd backend and the TCP lab setup.

The Spark-04 kernel guest also paired DPDK with stock Linux `rdma_rxe`.
Stock librdmacm drove the native QP. Both directions established over QP1,
verified an 8,195-byte RC WRITE, and disconnected. The DPDK requester
reported zero RC retries. The application-backed runs used copy TX and
required Generic Netlink to import kernel-pinned MRs.

| Ethernet MTU | RC PMTU | WRITE packets per direction | DPDK port | Verified profiles |
| --- | --- | --- | --- | --- |
| 1500 | 1024 | 9 | `net_af_packet` on veth | Standalone and application-backed |
| 4200 | 4096 | 3 | Stock `net_tap` on a guest bridge | Application-backed |

At MTU 4200 the harness set the actual DPDK and Linux TAP MTU before the
test. The wire capture confirmed 4096-byte RC payloads in both directions.

On November 24, 2026, the pinned `95afa13` build completed four
ConnectX-7 CM trials between Spark-04 DPDK and Spark-02 stock librdmacm.
Each established and disconnected over RoCEv2 CM and verified an
8,195-byte WRITE. The DPDK requester mapped 12,288 bytes of application
memfd memory for scatter/gather DMA and reported zero RC retries at both
MTUs. The native requester reported successful verbs completion. The
runner resolves Spark-02's ARP neighbor before mlx5 DPDK takes delivery
of the data port's Ethernet traffic. It restored both MTUs to 1500 and
the Spark-02 test port to raw Ethernet afterward.

| Direction | Ethernet MTU | WRITE packets | Spark-04 invocation |
| --- | ---: | ---: | --- |
| DPDK → ConnectX-7 | 1500 | 9 | `f9b6e81cdefa4ff69a103458e628670a` |
| ConnectX-7 → DPDK | 1500 | 9 | `25213dd90b1a40b29ad351b44f16083f` |
| DPDK → ConnectX-7 | 4200 | 3 | `6c39e13378fc45e59c28f51c191f165d` |
| ConnectX-7 → DPDK | 4200 | 3 | `6db99583152946a19e9318410c0f1766` |

The exact Spark-04 kernel guest verified native queue reuse beyond the
256-entry ring. The single-QP CM runs transferred 521 WRITEs in both copy
and scatter/gather mode. The out-of-band runs transferred 521 WRITEs per
QP across four QPs and two workers. The trace checked every posted WR ID
against its successful CQ entry as the ring wrapped twice. The two
configurations verified 133,897 and 535,588 bytes with one packet per
WRITE, then 4,269,595 and 17,078,380 bytes with nine packets per WRITE at
PMTU 1024. Both runs used required Generic Netlink handoff and had zero
RC retries. The worker retires scatter/gather WRITEs in QP order after
the RC ACK and release of that slot's PMD references.

Version 3 appends an array of second source descriptors after the
version 2 send queue layout. Both SGEs must use the configured MR. The
worker snapshots both addresses and lengths after validation and uses
the same snapshot on retransmission.
The wire tests compare two-span packet bytes, padding and iCRC against
the contiguous encoder. The provider test rejects the wrong descriptor
size, an invalid second lkey and an out-of-range second address. It also
polls ordered completions for a two-SGE WRITE followed by a one-SGE WRITE.

On the exact Spark-04 kernel, one-QP Netlink-backed copy and scatter/gather
CM runs each verified 24,585 bytes and three completions at PMTU 1024.
The gather trace showed four mbuf segments for a packet that crossed
the boundary with one byte from the first SGE and 1,023 from the second.
Dropping that packet triggered one RC retry and the same verified payload.
A four-QP, two-worker gather run verified 98,340 bytes and 12 ordered
completions with zero RC retries in two guest runs. At PMTU 4096, a
two-SGE scatter/gather WRITE from the DPDK requester verified 8,195
bytes against stock Linux RXE over TAP.
The updated worker and app also passed the CM retransmission fixture
with the older ABI-34 provider using the default version 2 queue.
In a separate local memif comparison at PMTU 1024, each trial verified
256 MiB across four QPs with one worker per endpoint. Two batches of nine
alternating trials per build put median goodput for the default
contiguous-payload path at 36.187 and 35.945 Gbit/s after the change.
The corresponding `e7bcf99` medians were 38.490 and 35.417 Gbit/s.
Both builds used the same application binary. The difference varied by batch.
[libibverbs scatter/gather posting](https://github.com/linux-rdma/rdma-core/blob/v50.0/libibverbs/man/ibv_post_send.3)

The native worker skips the doorbell ring when its publication counter
has stayed idle. A full ring triggers immediate send-queue recovery.
A 1 ms periodic scan covers the gap between publishing an SQ head and
signaling the worker. The notification word occupies existing padding
and preserves the shared queue layout. The doorbell unit test exercises
overflow and reuse. On the exact Spark-04 kernel in QEMU, the original
build, updated build, and updated worker with either an older provider
or a test provider that suppressed every notification each verified
17,078,380 bytes and 2,084 ordered completions in both copy and
scatter/gather mode. All four QPs posted, admitted, acknowledged and
retired 521 WRITEs.

The requester visits a QP's application completion state when an ACK
advances it or while a covered WRITE is still waiting on a TX reference.
On Spark-04, a synthetic 16-QP native completion loop on one Cortex-X925
core went from 21.38 to 1.39 ns per idle poll. Nine alternating local memif
trials per build and QP count at PMTU 1024 verified 128 MiB per trial with
no retries or port errors. Median requester goodput was 34.862 to 34.963
Gbit/s with one QP and 34.325 to 34.369 Gbit/s with 16 QPs. Those samples
used memfd scatter/gather and one worker per endpoint.

The exact Spark-04-kernel guest held the final WRITE's TX reference for
20 ms after the last ACK. The requester issued the final verbs completion
after releasing that reference, with no subsequent ACK. The same build
passed a stock TAP PMTU 4096 test with four QPs on two workers. All
2,084 native completions arrived in order and the receiver verified
17,078,380 bytes with nine RC retries. Both runs required Netlink
registration and completed resource teardown. The terminal fault
fixture preserved the successful prefix, followed by the remote-access
or retry-exhausted WC and the flushed suffix. The retry-exhaustion trial
observed exactly seven retries after ACK progress reset the budget.

The exact Spark-04 kernel guest also verified `rdma_create_qp()` on the
active ID and the passive child. Its trace matched each app's actual QPN
to both workers' views, checked the queue FD at both ends, and observed
both QPs in RTS before the adapter authorized REP or RTU. The active
`ESTABLISHED` event preserved all 20 bytes of the test's REP private data.
Copy and scatter/gather each completed 521 multipacket WRITEs with all
completions in order. Both external-QP controls completed three WRITEs.
All four profiles verified every byte with zero RC retries.

Stock DPDK `net_tap` also exercised native scatter/gather with both
Linux TAP interfaces and their bridge at MTU 4200. The worker used
PMTU 4096, and a capture showed 4,170-byte Ethernet WRITE frames.
On the exact Spark-04 kernel with required Netlink, one CM-owned QP
verified 4,269,595 bytes and all 521 completions with one RC retry.
Four QPs across two workers verified 17,078,380 bytes and 2,084 ordered
completions in both copy and scatter/gather, with 44 and 48 retries
respectively. The guest used a one-packet send window and
TX burst. The Unix application negotiation and TCP control waits also
completed on a bridge carrying IPv6 traffic that interrupted earlier
builds with TAP receive signals. [DPDK TAP PMD](https://doc.dpdk.org/guides-23.11/nics/tap.html)

The native-MR staging archives include a matched patched DPDK 23.11.7
runtime, worker and application, ABI-34 provider and unsigned modules for
each Spark's exact kernel. They passed archive checksums, dynamic loader
relocations and PCI-disabled EAL startup. The worker's mlx5 DMA-BUF
symbols resolved in that runtime. Both hosts require trusted module
signatures before the physical DMA-BUF mapping test.

The kernel modules in the `c5c8dd9` lab bundle were built against the
running headers on Spark-04 (`6.17.0-1021-nvidia`) and Spark-02
(`7.0.0-1025-nvidia`). Each `roce_mr` and `roce_uverbs` signature verified against
the module-signing-only certificate. Both hosts passed relocation checks
for the pinned applications, provider and mlx5 libraries. The signed
Spark-04 binaries passed the Netlink ticket and CM application tests in
the exact-kernel guest. Loading the modules on the physical hosts still
requires console enrollment of the certificate.

The paired application bandwidth harness stages identical userspace and
DPDK artifacts on Spark-04 and Spark-02. The 16 GiB modes mirror the
standalone MTU 1500 and 4200 tests. An 8 MiB profile checks first use.
The result parser requires distinct application and worker PIDs, sender
completions, receiver verification and the expected mlx5 mapping in
scatter/gather modes. The signed-module modes require the Generic Netlink
registration path. A local PMTU 1024 memif transfer through the staged
launchers verified 8 MiB and eight WRITEs over 8,192 packets with zero
retries. The `8b075bb` services on Spark-04 and Spark-02 verified 8 MiB
with distinct worker and application PIDs at each MTU. The mlx5 requester
mapped the full application memfd for scatter/gather. Requester throughput
was 25.454 Gbit/s at MTU 1500 and 32.239 Gbit/s at MTU 4200, with zero RC
retries. These smoke transfers used eight 1 MiB WRITEs per trial.

At MTU 1500, the `8b075bb` 16 GiB application four-worker profile
verified all three transfers at 84.577 Gbit/s median requester
throughput. That is 91.6% of the 92.320 Gbit/s ConnectX-7 control on
the same path with 16 QPs. The median sender and receiver invocation
IDs are `4163dced97e74fe59c8bbf3cd8901bdd` and
`28a9a72fcafd49b0a7c8da277c766441`. The three requester retry counts
were 0, 1,424, and 421. See the
[hardware comparison](README.md#dpdk-and-hardware-rocev2-comparison).

At MTU 4200, the `8b075bb` single-QP application profile verified
three 16 GiB transfers at a median of 30.690 Gbit/s with zero retries.
All three 16-QP profiles exhausted retries. Spark-04 logged mlx5 RX
syndrome `0x04`, the local protection error, in those trials. The
prefaulted four-worker jumbo trial reproduced the syndrome. The sender
TX and receiver RX counters both reached 386,593 packets, and the
receiver reported no misses. The sender recorded four RX errors. This
isolates a receive-side hardware completion error for the requester
while the underlying buffer or queue cause remains open.
[rdma-core mlx5 syndrome definitions](https://github.com/linux-rdma/rdma-core/blob/v50.0/providers/mlx5/mlx5dv.h#L994-L1006)

The reported jumbo error WQE counters were past the initial 1,024
receive descriptors. A local DPDK 23.11.7 allocator probe with PCI
disabled and the same `--no-huge` pool parameters found one backing
chunk at MTU 1500 and two at MTU 4200 for all tested queue counts.
Each jumbo buffer spanned 4 KiB page boundaries within its chunk.
On the same Spark pair, the MRC jumbo comparison eliminated the
same `0x04` errors twice by switching packet backing to 2 MiB pages
while holding scalar RX and the workload fixed. The RoCEv2 stager
provides the corresponding page-backing comparison and logs the actual
DPDK RX mode and pool chunk count.
[MRC jumbo hardware evidence](../mrc-dpdk/CONFORMANCE.md#spark-04-to-spark-02-link-recovery-and-jumbo-mtu)

An isolated Spark-04 kernel guest verified the hugepage service helper.
DPDK EAL used 128 MiB as the service account and allocated the production
four-queue jumbo pool with 10,239 buffers in one backing chunk. The
helper rejected mismatched mounts and competing reservations, kept the
pages while EAL was active, and restored the pool to zero on exit.

### Spark-04 to Spark-02 jumbo page backing

On November 24, 2026, both hosts ran the installed `8c5058a` memfd
scatter/gather profile at Ethernet MTU 4200 and RoCE PMTU 4096. Each
endpoint used four workers, 16 QPs, 1 MiB WRITEs, prefaulted application
mappings, and Vector Neon RX on all four queues. The benchmark link
reported 100,000 Mb/s at both ends.

Both attempts with ordinary 4 KiB backing allocated the packet pool in
two chunks. The Spark-04 requester recorded five and four mlx5 RX errors
respectively, including syndrome `0x04`, and exhausted RC retries. With
2 MiB backing, the pool occupied one chunk at each endpoint. Each run
verified all 16 GiB and 4,194,304 data packets with zero RC retries, NIC
errors or misses. These trials show that changing the packet-pool backing
resolved the failure for this workload. The underlying registration or
descriptor failure in the 4 KiB case remains unverified.

| Requester Gbit/s | Responder Gbit/s | Spark-04 journal invocation | Spark-02 journal invocation |
| ---: | ---: | --- | --- |
| 98.012 | 98.000 | `cd636aa436f042a295cc80cf65efcfed` | `dfd4ec8756d24e65b9dc4bc64412c9d4` |
| 98.019 | 98.028 | `6107af619a8c433e844396eb72fa5537` | `86da4f1c5a764857909a68de6efa7fdf` |
| 98.010 | 98.010 | `ffb5253a115b45a0be7cfea802baed2d` | `c93f15e291ab4aaab3bc2ab202157ac8` |

Median goodput was 98.012 Gbit/s at the requester and 98.010 Gbit/s at
the responder, measured over the roughly 1.40-second DPDK transfer.
The receiver application's timed interval also included setup waits and
full-buffer verification and had a median of 31.28 seconds. Both hosts
released all 64 temporary 2 MiB pages and returned to Ethernet MTU 1500.

Three MTU 1500 trials of the same prefaulted build with ordinary packet
backing verified 16 GiB each at 83.410, 84.272 and 84.542 Gbit/s. The
requester recorded 15, 0 and 0 retries. Both endpoints had zero NIC
errors. The responder recorded 592 misses in the first trial and zero
in the repeats. The median is 91.3% of the 92.320 Gbit/s native
16-QP result on the same path at MTU 1500. The median sender and receiver
journal invocation IDs are `1390e60312d24588b5e67f6bcd23899c` and
`1eebdfda86bb430e89c55252d429c6c6`.

The `--hugepages-only` runner selected just the 2 MiB variant on both
hosts. A verification run with the installed `8c5058a` memfd build
reported 98.015 Gbit/s at the requester, verified 16 GiB, and recorded
zero requester retries and zero NIC errors. The sender and receiver
invocation IDs are `38171ad1580f4308a5d4b250f64848db` and
`615a5a2eb0834f60b4526a60261c28be`.

### Spark-04 to Spark-02 application bandwidth rerun

On November 24, 2026, the installed `8c5058a` services ran three more
trials at each MTU with four DPDK workers and 16 QPs per endpoint. Each
trial completed and verified 16 GiB of 1 MiB WRITEs using prefaulted
memfd scatter/gather. The MTU 4200 profile used 2 MiB packet-pool pages.
Both endpoints reported 100,000 Mb/s before each sweep and returned to
Ethernet MTU 1500 at the end.

| Ethernet MTU | Requester trials (Gbit/s) | Requester median (Gbit/s) | Responder median (Gbit/s) |
| --- | --- | ---: | ---: |
| 1500 | 84.282, 84.104, 84.694 | 84.282 | 84.284 |
| 4200 | 98.027, 98.021, 98.019 | 98.021 | 98.028 |

The MTU 1500 median was 91.3% of the matched 92.320 Gbit/s hardware
control. One MTU 1500 trial recorded 49 retries and 1,805 responder RX
misses. The other five trials recorded zero retries and misses. No
trial recorded a NIC packet error. The median sender and receiver
journal invocation IDs were `1306c7dad52440eb8b033dcfb0b96ce7` and
`f302f1205e984d719f1bbf9faa411612` at MTU 1500, and
`de66015e6789499f8cf1ff8d232b2230` and
`6a989d33966f42718b8e2bb1dcf26885` at MTU 4200. Both Spark hosts still
required enrollment of the module-signing key for kernel-backed MR runs.

### Additional datapath measurements

On the four-worker MTU 1500 profile, one prefaulted trial measured
84.899 Gbit/s with zero misses and retries. Receiver polling faults
fell from 4,194,308 in the default median trial to six in that run.
Prefaulting the receiver alias took 337 ms. The prefaulted single-worker
16-QP trial completed at 25.373 Gbit/s with 2,765,156 receiver misses
and 33,583 retries. These two prefaulted rates are individual samples.

A smaller sender-only pool for gather mbufs passed the stock TAP jumbo
test with 2,084 ordered WRITE completions. At PMTU 1024 over local memif,
nine alternating 128 MiB samples per variant yielded 34.744 Gbit/s with
the shared pool and 34.362 Gbit/s with the compact pool. Both variants used
one polling worker per endpoint and completed all trials without retries,
drops, or reported packet errors. The implementation uses the shared pool.

Two matched 8 MiB runs on the exact Spark-04 kernel in QEMU put one
read per application MR page before transport setup. Prefaulting
2,048 pages took 63–66 ms per worker. It removed 2,048 sender and
2,047–2,048 receiver minor faults from the polling interval. The
four runs verified the data and all eight requester completions with
zero RC retries. The Spark runner records polling faults, attach
time, DMA setup and prefault time separately for the physical comparison.

Application transfer timing was verified through the staged launcher on
Spark-04 memif. The exact Spark-04 kernel guest also verified one timing
record per application in native copy and scatter/gather, with 521
successful WRITEs per run and no retries. The same builds emitted no
timing records when disabled. During in-band CM the responder's application
elapsed time includes the disconnect replay wait before worker completion.

The lab verifier now compares each byte in blocks so the compiler can
vectorize the comparison. It scans a failing block for the first
mismatched offset. An isolated 16 GiB Spark-04 verification took 6.74 s
of thread CPU time on the Cortex-A725 used for the receiver application,
compared with 16.60 s before the change. The Cortex-X925 took 2.33 s
compared with 7.09 s. Both builds detected the same corrupt byte at the
start, on either side of the 4 KiB boundary, and above the 4 GiB offset.
This measures full-buffer verification after the datapath interval.
In three alternating 2 GiB memif trials per build with the same workers,
the receiver application took 2.076 → 0.842 s of median CPU time and
3.816 → 2.586 s of elapsed time. The worker medians were 63.121 and
62.950 Gbit/s for that two-worker, 16-QP test.

Scatter/gather application producers now wait for ring space or native
SQ completion credit when needed. They can post successive batches without
waiting for the worker to acknowledge admission of each batch. In five
alternating 128 MiB memif runs per build on Spark-04 at PMTU 1024, one
WRITE per batch improved from 8.00 to 22.41 Gbit/s. At 8 and 32 WRITEs per
batch the medians were 23.90 to 23.74 and 23.94 to 22.55 Gbit/s. Seven
additional 512 MiB runs per build with prefaulting and 32 WRITEs per batch
measured 36.09 to 36.54 Gbit/s. Every run verified all bytes with zero RC
retries. A four-producer test submitted 516 WRITEs to one worker and verified
every completion. The exact Spark-04 kernel guest verified 521 completions
per QP across native queue wrap. Separate guest trials verified recovery
from packet loss and short or zero-accepted TX, along with terminal
completion statuses and resource teardown.

The optional Arm PMULL iCRC produces the same IEEE checksum as scalar
CRC32. Independent jumbo vectors verify the encoded bytes and reject
payload corruption on decode. A local Spark-04 Cortex-X925 packet
microbenchmark cycled through 64 payload buffers and measured thread CPU
time, with 140,000 packets per sample and seven alternating samples.

| Packet operation | 1024-byte payload, scalar → PMULL | 4096-byte payload, scalar → PMULL |
| --- | ---: | ---: |
| Scatter/gather header and trailer encoding | 59.8 → 49.4 ns | 159.3 → 139.2 ns |
| Copy TX encoding | 67.4 → 66.0 ns | 207.7 → 188.0 ns |
| RX decode and CPU memcpy | 70.4 → 63.2 ns | 190.9 → 174.0 ns |

Scatter/gather encoding was essentially flat on Cortex-A725 in the same
microbenchmark. The data-plane sweep can compare both builds on the
physical link. [Arm CRC32 and PMULL intrinsics](https://arm-software.github.io/acle/neon_intrinsics/advsimd.html)

The opt-in build passed the RoCEv2 and MRC regressions. The exact
Spark-04 kernel guest exposed PMULL to the worker and verified both
copy and scatter/gather with Netlink-backed MRs. It verified
4,269,595 bytes in each single-QP CM run and 17,078,380 bytes in each
four-QP run, with 521 ordered completions per QP. The scalar and
portable builds passed the same independent jumbo wire vectors.

The Arm IPv4 iCRC prefix now masks the fields as it loads the five words
needed for CRC32. The isolated Cortex-X925 packet benchmark measured
scatter/gather encode time of 49.78 → 44.65 ns at PMTU 1024 and
139.59 → 134.59 ns at PMTU 4096. Both builds used PMULL. The resulting
IPv4 and IPv6 CRCs matched the portable implementation on 12,000
varied packet lengths and alignments. At PMTU 1024, seven alternating
256 MiB memif trials per build with four QPs and one worker per endpoint
measured 35.373 Gbit/s before the prefix change and 35.403 Gbit/s after.
Bulk receive-buffer recycling measured 34.718 Gbit/s in the same test.
All successful trials verified every byte with zero retries. The memif
PMD reported a 1,518-byte frame limit, so the jumbo numbers come from
the isolated packet benchmark.

The RC requester now returns the WRITE index it used to build each
packet. The scatter/gather worker uses that same index to hold the
matching native SQ entry while the NIC owns a payload segment. The RC
test verifies the returned index across WRITE boundaries and PSN wrap.
The exact Spark-04 kernel guest verified 521 native completions per QP
with one and four QPs. Injected WRITE and ACK loss recovered in one retry
each. A short TX followed by a zero-accepted TX resumed at the first
unsent PSN without an RC retry.

Two TX mbuf batching experiments verified all bytes and returned
slightly lower throughput in the Spark-04 memif comparison.
The worker keeps the three-mbuf allocation per scatter/gather packet.
With the shared WRITE index, nine alternating 128 MiB memif runs per
build at PMTU 1024 gave 35.08 and 35.68 Gbit/s requester medians before
and after the change. Both builds used PMULL and pinned their pollers
to Cortex-X925 cores. Every run verified all bytes with zero RC retries.
An isolated TX CPU test was effectively flat at PMTUs 1024 and 4096.

Some DPDK 23.11.7 guest runs log `EAL: Error: Invalid memory` at
memif port teardown. Backtraces located that diagnostic in
`memif_free_regions()` during both port stop and EAL interrupt handling. The affected trials
reported zero RC retries and verified all bytes and completions.
