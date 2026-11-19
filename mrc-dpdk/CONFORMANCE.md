# OCP MRC 1.0 conformance status

The conformance target is the [OCP Multipath Reliable Connection 1.0 specification](https://www.opencompute.org/documents/ocp-mrc-1-0-pdf/). The SRv6 emulator is a reference for path behavior and reorder metrics. An independent MRC endpoint or qualified packet captures are needed to verify wire interoperability.

| Scope | Verified in the current slice | Remaining evidence or implementation |
| --- | --- | --- |
| §§6.1–6.4, 10.1.2, request and completion semantics | Mixed WRITE and WRITEIMM, per-WR geometry, registered-memory leases, ordered immediate completions, remote access errors, RQ exhaustion without RNR, and typed error/flush completions are verified. The OCP library exposes verbs contexts and device queries, CQ/channel events, and public QP create, modify, query, reset, destroy, and linked WR posting. It keeps directional PSN, MPR and WIMM attributes separate, binds actual `ibv_mr` keys by verbs PD, and sends across registered scatter/gather buffers. The DPDK worker carries public WRs across two memif rails, dispatches feedback and requests by QPN and opcode, and progresses multiple bidirectional QPs on the same ports. Solicited WRITEIMM notifies an armed solicited-only receive CQ. The context queues asynchronous CQ overruns and provider-triggered QP ERROR events with blocking or pollable retrieval and acknowledgement. Active QPs that share a failed CQ flush work when their next operation observes the failure. | Port and device events, finer QP event classifications, inline and extended verbs posting, QP hints, and independent hardware interoperability for application-managed OOB attributes. |
| §§6.2.2, 7.5.5, wire format | Independent byte vectors for IPv4/UDP, IPv6/UDP, and preserved SRv6 envelopes, iCRC, WRITE with immediate and TSETH, AETH, SETH, NETH, and PETH. Independent pcap replays and live Python feedback verify IPv4, IPv6, and preserved SRv6 with or without a final SRH. Checks cover iCRC, structured EV reflection, CE marking, PSN wrap, packet rejection and receiver QP ERROR for valid requests with an unsupported opcode. Independent packet-buffer and TRIM NACKs trigger alternate-rail retry. | Interoperability with an external MRC implementation, transit routing on an independent SRv6 fabric, and remaining edge-case packet validation. |
| §§7.2, 7.4–7.5, packet reliability | 24-bit PSN and window checks, wrap, selective SACK bitmaps, separate AETH, duplicate suppression, retransmit marking, logical timers, SACK-driven fast loss, reliability probe request/response matching and RTT sampling (probe exchanges observed on the Spark pair), and separately classified trimmed packets with TRIM NACK recovery. Negotiated Dynamic MPR advertises each change in SACKs and gates both first transmissions and retries, while preserving outstanding receive state. Over memif, all five mandatory Table 7-3 NACK reasons were generated and handled, including retry-budget exhaustion and terminal CQ overrun. | Independent live NACK interoperability and recovery after an actual port transition. Both receiver Sparks verified a 16 MiB WRITE after an injected path-1 blackhole. An independent memif peer checks alternate-rail retry on a packet-buffer NACK and confirms that a WRITEIMM credit stays occupied until a semantic ACK. Provider QP errors and typed send completions are verified for retry exhaustion and UNEXP_EVENT. Trim NACK generation and service time are capability-gated. |
| §§7.3, 7.4.7–7.4.8, 7.5.6, endpoint operations | ERTH/EETH byte fixtures and a two-path Port Status Update exchange using reserved QPN 2 and echoed tokens, observed over memif and the Spark pair. | Physical port transitions, out-of-order status handling, and optional EV probe initiation. |
| §8, congestion control | Nominal per-packet accounting, modulo 24-bit feedback, congestion-window admission and RTT/ECN-driven NSCC adjustments. The public QP binds an immutable ONLINE NSCC or disabled CC profile. Unit tests drive the configured maximum window, base RTT, target delay, thresholds, and increase and decrease parameters. | Deployment measurements, link-rate-derived tuning for custom profiles, and optional receiver penalty and quick adaptation. Verify ECN and trimming on a packet-switching fabric or with controlled injection. |
| §§9–10, EVs and APIs | Two paths with configurable UDP source-port EVs, IPv6 flow-label reflection, preserved or already decapsulated SRv6 receive, and alternate-path retransmission. The OCP controller ABI queries device EV capacity and built-in UDP/STEV formats, applies persistent AUTO or explicit profile lifecycle and GOOD/DENIED operations, and gates mutations on CAP_NET_ADMIN. Public QPs lease ONLINE EV and CC profiles. The controller can update administrative EV state during transfer and query and configure CC profiles with CAP_NET_ADMIN. Active-only rotation distinguishes denied, congestion-skipped, and failed EVs. A fresh correlated probe can restore a failed EV. | Programmable EV formats, generated and SRv6 EV profiles, controller EV events and probes, and remaining controller operations. |

## Specification questions for interoperability

- The endpoint uses the configurable UDP destination and default 4971 specified by §7.5.5.2. Table 7-9 shows the RoCEv2 value 4791, which remains selectable with `--udp-port 4791`.
- Sections 7.5.1 and 7.5.5 describe the PDC IDs differently. The implementation uses the requestor's QPN low 16 bits as SPDCID and the responder's QPN low 16 bits as DPDCID, following Tables 7-6, 7-15, and 7-17.
- Section 6.3.1 directs responders to silently drop outside-window PSNs. Table 7-3 names `PSN_OOR_WINDOW` as a mandatory responder NACK. The receiver currently sends that NACK for valid addressed requests outside its tracking window. Confirm the intended behavior with an independent implementation.
- Table 6-5 specifies whole-WR DMA length in every RETH. The endpoint uses that value and validates each payload within the registered region.
- Polatis connects Ethernet links optically. The Spark double ring can verify path use and recovery but does not itself supply ECN marks or packet trimming.

## Verification

From `mrc-dpdk/`, run `make check` for wire, QP, work-request, congestion, connection negotiation, provider resource and QP lifecycle, EV, controller storage, and receive-window tests without DPDK. Build with `make` and run `python3 tests/check_mrc_write.py` for the IPv4 and IPv6 endpoints over two local DPDK memif links. This checks short and zero-length WRITEs, striping, separate ACK semantics, mixed WRITE and WRITEIMM lengths and receive sequencing across loss and PSN wrap, ordered WRITEIMM completions, receive-credit exhaustion, bad-rkey errors, ordered error and flush completions, node port-status updates, correlated reliability probes, TCP QP negotiation, PSN wrap, packet loss, probe-driven path restoration, a live controller update with custom IPv6 EVs, negotiated receive-window shrink, mandatory reliability NACKs, and synthetic trimming of bare IP and preserved SRv6 frames.

With DPDK's pcap PMD, `python3 tests/check_mrc_pcap.py` verifies the
responder through independent capture generation and interpretation.
Build `tests/mrc_memif_relay` with a memif-enabled DPDK and run
`python3 tests/check_mrc_requester.py` to exercise the requester with an
independent live peer. Both scripts cover IPv4, IPv6, and preserved SRv6
with and without a final SRH. The relay moves Ethernet frames and the
Python peer handles wire encoding, memory placement, and feedback.

`make tests/mrc_endpoint_application` and `python3 tests/check_mrc_application.py`
verify that an application can start a DPDK requester on an empty provider
QP, post new work while it runs, and poll its own CQ. The test starts after
a QP reset with a noninitial work sequence. It verifies WRITE/WRITEIMM
placement and completion IDs across separate admissions, PSN wrap, and
continued immediate receive sequencing over two memif rails.

Use Spark-04 as the implementation source. After pushing the exact clean commit, run `tests/verify_spark_revisions.sh --sync "$(git rev-parse HEAD)"` to advance the detached test worktrees on Spark-02 and Spark-03. Run the same command without `--sync` immediately before each live test. The script checks that all three worktrees are clean and at the requested commit.

Build `tests/mrc_abi_application` against the OCP headers and run
`python3 tests/check_mrc_abi_application.py` to exercise both endpoints
through the public shared library. It exchanges verbs GIDs, QPNs, PSNs
and MR keys, checks placement across scatter/gather and PSN wrap, confirms
signaled send CQEs and two WRITEIMM receive CQEs. It verifies the
solicited WRITEIMM wakes the completion channel and a subsequent ordinary
WRITEIMM leaves a solicited-only arm pending. Its shared-port case crosses two bidirectional QPs per
peer with separate registered destinations and completion IDs, then resets
one QP and exchanges further WRITEIMM traffic on the surviving QP.

`make check-mrc-abi` also verifies asynchronous event ordering and
acknowledgement, nonblocking retrieval, object destruction, and the effect
of CQ overrun on a second QP sharing the CQ. The public memif error case
overruns a receive CQ and checks `CQ_ERR` followed by `QP_FATAL`. On the
requester, the remote `UNEXP_EVENT` produces an error WC and `QP_FATAL`.

The shared-library controller test verifies capabilities, profile
transitions, per-entry batch errors, persistence, and EV and CC QP leases.
It also verifies disabled CC selection and rejects controller CC queries and
mutations without the required capability. Build it
with `make MRC_INCLUDE_DIR=/path/to/headers tests/test_mrc_ctl_abi`. Run
`./tests/test_mrc_ctl_abi --expect-no-cap` without CAP_NET_ADMIN and
`unshare -Ur ./tests/test_mrc_ctl_abi` for the isolated positive case.
With `tests/mrc_abi_application` built, run
`python3 tests/check_mrc_abi_application.py --controller tests/test_mrc_ctl_abi`
to verify two-rail transfer while a separate controller process updates the
profile. Both EVs start DENIED and neither peer completes work. The
controller makes rail 2 GOOD while an active QP holds the profile, and
the WRITE and WRITEIMM operations complete with verified data. A separate
NSCC case programs a one-packet window and observes both DPDK TX rails
while the peer withholds feedback. It sees one full-sized WRITE with
BTH ACK Request before the peer resumes and validates payload placement
and completions in both directions.

### Spark-04 to Spark-02 physical baseline

Both peers completed a 4194304-byte IPv4 WRITE on the two direct Polatis
circuits at commit `048d0567aaea86874d23a9c080b328de04e69d51` with
DPDK 23.11.7 and UDP destination 4971. Spark-04 sent 2048 segments per
rail, Spark-02 placed 2048 per rail, and Spark-04 reported zero retries,
935 SACKs, two correlated reliability probe replies, and two port-status
replies. The receiver verified the written buffer. The same command pair
on UDP 4791 exhausted its retry budget. RoCE was enabled on all four ports,
and NVIDIA documents that mlx5 treats UDP 4791 as RoCE when enabled.
[NVIDIA mlx5 troubleshooting](https://docs.nvidia.com/networking/display/bfswtroubleshooting/mlnx-dpdk.pdf)

### Spark-04 to Spark-02 recovery with injected loss

At commit `b279ab303e8a085d903120259b187d82cadc79b8`, Spark-02 verified
the 16777216-byte WRITE sent on UDP 4971. The sender's test configuration
suppressed path-1 transmission from the 512th scheduled segment until
2048 segments had been scheduled. The receiver placed all 16384 unique
segments, with 9095 on path 0 and 7289 on path 1, and counted one duplicate.
Spark-04 completed the WRITE and reported 131 injected drops, 132 retries
on path 0, and no retries on path 1. It recorded 130 fast retries, eight
correlated probe replies, one EV failure, and one EV recovery. The receiver
placed 7289 segments on path 1, confirming that transmission resumed after
the blackhole. Port counters reported zero misses, errors, and receive
buffer exhaustion on both Sparks.

### Scoped runner verification on Spark-02, Spark-03, and Spark-04

The `mrc-test` systemd runner passed the smoke and injected-loss profiles
on both pairs. Before each run, all three test worktrees matched
`6a02243b88b8bac406a4877230cfc185f0c7fa7f`. The three root-owned snapshots
and the verified Spark-04 build had the same SHA-256 for `mrc_write`
(`fa07413417ffc2d4542a5353d5837d5ed751ae8499f8bde4c3e15b839f8889d0`).
The smoke and failover units explicitly load the ring mempool and mlx5 PMDs.

| Pair | Smoke, 4 MiB, segments by rail | Failover, 16 MiB, segments by rail | Failover duplicate packets |
| --- | --- | --- | --- |
| Spark-04 to Spark-02 | 2048 / 2048 | 9122 / 7262 | 30 |
| Spark-04 to Spark-03 | 2048 / 2048 | 9062 / 7322 | 6 |

The receivers verified all expected data and reported zero port misses,
errors, and receive buffer exhaustion. The failure profile suppressed
rail-1 transmission between scheduled segments 512 and 2047. The rail-1
delivery totals show that both pairs resumed use of the affected rail.
Receiver journals and sender unit state supplied the scoped runner
evidence. Spark-04 journal access is needed to inspect the sender's
individual retry and EV counters for these runs.

### Negotiated WRITEIMM and Dynamic MPR

A local memif run verified eight 256 KiB WRITEIMMs with an in-flight limit
of two and provider-registered memory. The receiver lowered MPR from
eight units to one after 65 accepted packets. Both peers applied one MPR
update and completed all eight work requests. The targeted case passed
with both the installed Spark snapshot revision and the development build.
The corresponding physical run needs the scoped negotiated pair profile.

### Concurrent three-Spark verification

The [Spark cycle tests](README.md#concurrent-three-spark-tests) exercise
three simultaneous 256 MiB WRITEs on the full double ring. The smoke
profile checks both rails on all three edges. The recovery profile drops
path-1 traffic on Spark-04's outgoing edge and checks that all three
WRITEs complete and the affected EV recovers. All three scoped runners
matched the pinned Git revision and binary digest. The first live smoke
attempt stopped in receiver EAL setup because systemd exported two runtime
paths in one variable. The unit renderer and preflight now require clearing
that variable so EAL uses the per-role runtime path. The hardware cases are
pending the unit refresh.

mlx5 setup requires `CAP_NET_RAW` and `CAP_NET_ADMIN` or an administrator-run endpoint. [DPDK mlx5 privilege documentation](https://doc.dpdk.org/guides-23.11/platform/mlx5.html#run-as-non-root)
