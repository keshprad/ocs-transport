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
| A17.3.1–A17.3.2, CA17-20–21 | IPv4 UDP encapsulation, destination 4791, length including iCRC, stable source port per QP | **V**. `WIRE` compares complete IPv4 frames against an independent encoder. Source port derives solely from the local and remote QPN. |
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
| MEMIF | `make check-memif` | Two production DPDK processes verified a WRITE Only, PMTU negotiation from 1024 and 512, and 65 consecutive 8,195-byte WRITEs on one QP. Separate tests verified WRITE Only and repeated 1,048,579-byte WRITEs across 16 independent QPs. |
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
  For Spark, measure the current CPU-copy baseline before evaluating transmit
  buffer chaining, CRC acceleration, or a receive-copy offload.
- **§5.1 and Appendix A.7.** Report QP count, batch size, and core usage with
  throughput. BURST's single-QP result is about 121 Gbit/s in its x86/400G
  testbed. The DPDK test supports 16 QPs and independent RC state. The
  first multi-QP probe shares one polling core and one PMD queue per
  endpoint. Spark physical bandwidth and scaling across cores remain
  open.
- **§§4.1 and 4.7.** The verbs provider and user-space CM are useful references
  if we extend application compatibility after the wire milestone.
