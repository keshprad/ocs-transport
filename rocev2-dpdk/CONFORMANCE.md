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
| A17 and Chapter 9 end to end | DPDK-to-native and native-to-DPDK WRITEs with a stock ConnectX-7 endpoint | **V** for the tested one-QP IPv4 host-memory WRITE. Two runs verified 8,195 bytes in each direction at PMTU 1024 with raw Ethernet on the DPDK function and native RoCE on the verbs peer. |

Zero-length WRITE, WRITE with Immediate, SEND, READ, atomics, IPv6, RoCE
congestion management, standard RDMA CM, and a DPDK-side verbs provider remain
open. No overall clause count is asserted.

## Reproducible evidence

| ID | Command from this directory | Result |
| --- | --- | --- |
| WIRE | `make check` → `tests/check_wire.py` | Independent BTH/RETH/AETH and iCRC vectors for five opcodes, plus negative decode cases |
| RC | `make check` → `tests/test_roce` | Single- and multi-packet placement, PSN wrap, loss and duplicate recovery, cumulative ACK validation and retry limit |
| MEMIF | `make check-memif` | Two production DPDK processes verified a WRITE Only, PMTU negotiation from 1024 and 512, and 65 consecutive 8,195-byte WRITEs on one QP. Separate tests verified WRITE Only and repeated 1,048,579-byte WRITEs across 16 independent QPs. With four available CPUs, a two-queue test verifies WRITEs on three QPs. |
| PEER | `make check-peer` → `tests/check_peer.py` | Independent Python RC peer and production DPDK endpoint exchanged a 515-byte WRITE in each role at PMTU 256. The peer checked every WRITE segment and each response. The test injected a sequence gap, lost ACK, corrupted iCRC, and changed-payload duplicate. |
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
  measurements. Transmit buffer chaining and receive-copy offload remain
  open.
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
| Figure 5(a), §4.1 | Standard verbs contexts and resources | Stock libibverbs found `roce_dpdk0` in the Spark-04-kernel guest. The MR fixture verifies range and access validation, process-scoped keys, deregistration, and application exit with an outstanding worker lease. The CM applications create CQs and RC QPs before setup. Provider checks cover unmatched QPN and wrong-PD rejection, successful rebinding after a rejected configuration, and `EBUSY` for referenced MR, PD and CQ resources. |
| Figure 5(b), §4.2, Appendix A.1 | WQE notification and completion | Worker queues consume shared WQEs from native `ibv_post_send()` and publish individual results for `ibv_poll_cq()`. The provider fixture verifies partial linked-list posting, CQ capacity, queue closure and error WC mapping. With Netlink required, the exact Spark-04-kernel guest verified native copy and scatter/gather on one QP and native copy on four QPs across two workers. Each QP posted one WRITE at a time and returned all three signaled completions. |
| Figures 7–8, §§4.3–4.4 | Scatter/gather TX and CPU RX placement | The requester chains worker-owned headers and trailer around the application payload. Independent wire vectors cover PMTU 4096. The RC peer verifies sequence NAK and timeout retries. With standard verbs registration on both ends, a four-process memif test verified 24,585 bytes in both copy and scatter/gather modes before deregistration. RX copies into the MR before acknowledging. The DPDK 23.11.7 mlx5 patch builds, and the worker resolves its DMA-BUF map/unmap APIs. Physical ConnectX-7 DMA from native MRs remains unverified. |
| Figures 5(c) and 9, §4.7 | Standard RDMA CM over the DPDK datapath | `make check` verifies wire format, peer and transaction identity, TX backpressure, setup and disconnect retries, crossed DREQs, DREP replay, and the REP and RTU gates. The single-QP lab adapter forwards standard librdmacm calls over an authenticated Unix socket. CM-owned QPs use `rdma_create_qp()` with a provider PD and polling CQs. The worker hands back the queue FD, waits for QP readiness, and sends REQ, REP, RTU, DREQ and DREP on DPDK queue 0. The exact Spark-04 kernel guest exercised CM-owned and external QPs. TCP carries the lab's WRITE completion and verification. Stock librdmacm verified WRITE and disconnect in both directions with Linux RXE and physical ConnectX-7 at MTUs 1500 and 4200. |

The registration socket carries the request, the correlation cookie
and the assigned keys. The kernel resolves the Unix peer and binds the
ticket to that worker's process and namespaces. The ticket identifies
the mapping while the exporter holds the kernel file descriptor.
[BURST design](https://www.usenix.org/system/files/nsdi26-shen.pdf#page=6)

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

The exact Spark-04 kernel guest also verified `rdma_create_qp()` on the
active ID and the passive child. Its trace matched each app's actual QPN
to both workers' views, checked the queue FD at both ends, and observed
both QPs in RTS before the adapter authorized REP or RTU. The active
`ESTABLISHED` event preserved all 20 bytes of the test's REP private data.
Copy and scatter/gather each completed 521 multipacket WRITEs with all
completions in order. Both external-QP controls completed three WRITEs.
All four profiles verified every byte with zero RC retries.

The native-MR staging archives include a matched patched DPDK 23.11.7
runtime, worker and application, ABI-34 provider and unsigned modules for
each Spark's exact kernel. They passed archive checksums, dynamic loader
relocations and PCI-disabled EAL startup. The worker's mlx5 DMA-BUF
symbols resolved in that runtime. Both hosts require trusted module
signatures before the physical DMA-BUF mapping test.

The paired application bandwidth harness stages identical userspace and
DPDK artifacts on Spark-04 and Spark-02. The 16 GiB modes mirror the
standalone MTU 1500 and 4200 tests. An 8 MiB profile checks first use.
The result parser requires distinct application and worker PIDs, sender
completions, receiver verification and the expected mlx5 mapping in
scatter/gather modes. The signed-module modes require the Generic Netlink
registration path. A local PMTU 1024 memif transfer through the staged
launchers verified 8 MiB and eight WRITEs over 8,192 packets with zero
retries. The physical Spark performance sweep is pending.

Two matched 8 MiB runs on the exact Spark-04 kernel in QEMU put one
read per application MR page before transport setup. Prefaulting
2,048 pages took 63–66 ms per worker. It removed 2,048 sender and
2,047–2,048 receiver minor faults from the polling interval. The
four runs verified the data and all eight requester completions with
zero RC retries. The Spark runner records polling faults, attach
time, DMA setup and prefault time separately for the physical comparison.

Some DPDK 23.11.7 guest runs log `EAL: Error: Invalid memory` at
memif port teardown. Backtraces located that diagnostic in
`memif_free_regions()` during both port stop and EAL interrupt handling. The affected trials
reported zero RC retries and verified all bytes and completions.
