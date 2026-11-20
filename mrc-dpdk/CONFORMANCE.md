# OCP MRC 1.0 conformance status

The conformance target is the [OCP Multipath Reliable Connection 1.0 specification](https://www.opencompute.org/documents/ocp-mrc-1-0-pdf/). The SRv6 emulator is a reference for path behavior and reorder metrics. An independent MRC endpoint or qualified packet captures are needed to verify wire interoperability.

| Scope | Verified in the current slice | Remaining evidence or implementation |
| --- | --- | --- |
| §§6.1–6.4, 10.1.2, request and completion semantics | Mixed WRITE and WRITEIMM, per-WR geometry, registered-memory leases, ordered immediate completions, remote access errors, RQ exhaustion without RNR, and typed error/flush completions are verified. The OCP library exposes verbs contexts and device queries, CQ/channel events, and public QP create, modify, query, reset, destroy, and linked WR posting. It accepts WRITE and WRITEIMM creation flags and receive posts with or without SGEs. It keeps directional PSN, MPR and WIMM attributes separate, binds actual `ibv_mr` keys by verbs PD, and supports registered scatter/gather buffers and post-time copies of inline WRITE and WRITEIMM data. QP hints are queryable advisory attributes with validated INIT association and lifetime checks. The DPDK worker carries public WRs across two memif rails, dispatches feedback and requests by QPN and opcode, and progresses multiple bidirectional QPs on the same ports. Solicited WRITEIMM notifies an armed solicited-only receive CQ. The MRC context exposes asynchronous CQ overruns, typed responder request and access errors, QP fatal errors, and native device and port events on its pollable FD. Active QPs that share a failed CQ flush work when their next operation observes the failure. | Native event delivery after a physical port transition and independent hardware interoperability for application-managed OOB attributes. |
| §§6.2.2, 7.5.5, wire format | Independent byte vectors for IPv4/UDP, IPv6/UDP, and preserved SRv6 envelopes, iCRC, WRITE with immediate and TSETH, AETH, SETH, NETH, and PETH. Independent pcap replays and live Python feedback verify IPv4, IPv6, and preserved SRv6 with or without a final SRH. Checks cover iCRC, structured EV reflection, CE marking, PSN wrap, packet rejection and receiver QP ERROR for valid requests with an unsupported opcode. Independent packet-buffer, TRIM, and last-hop TRIM NACKs trigger alternate-rail retry. | Interoperability with an external MRC implementation, transit on independent SRv6 switch hardware, and remaining edge-case packet validation. |
| §§7.2, 7.4–7.5, packet reliability | 24-bit PSN and window checks, wrap, selective SACK bitmaps, separate AETH, duplicate suppression, retransmit marking, timeout backoff, a base timer after NACK-driven retransmission, finite retry exhaustion and unlimited-retry recovery when both rails go silent, SACK-driven fast loss, reliability probe RTT sampling and startup retry per rail (probe exchanges observed on the Spark pair), and separately configured regular and last-hop trim classes with distinct NACKs and recovery. Negotiated Dynamic MPR advertises each change in SACKs and gates both first transmissions and retries, while preserving outstanding receive state. Over memif, all five mandatory Table 7-3 NACK reasons were generated and handled, including retry-budget exhaustion and terminal CQ overrun. | Independent live NACK interoperability and recovery after an actual port transition. Both receiver Sparks verified a 16 MiB WRITE after an injected path-1 blackhole. An independent memif peer checks alternate-rail retry on a packet-buffer NACK and confirms that a WRITEIMM credit stays occupied until a semantic ACK. Provider QP errors and typed send completions are verified for retry exhaustion and UNEXP_EVENT. Trim NACK generation follows the local responder capability. Requester Service Time compensation follows the peer responder capability. The responder reports DPDK RX-to-feedback preparation time or adjusts a 128 ns request timestamp. |
| §§7.3, 7.4.7–7.4.8, 7.5.6, endpoint operations | ERTH/EETH byte fixtures and two-rail Port Status Updates observed over memif and the Spark pair. An independent peer verifies reserved QPN 2, Control traffic class, echoed opaque 16-bit identifiers, fresh reports after a lost response, single-rail gating after a second report, and expiry when reports stop. The privileged controller batch API also sends EV probes and Port Status Updates without a QP. Wire checks cover UDP, STEV, and SRv6 EVs, selectable request DSCP, Control replies, returned rail and RTT, partial timeouts, and rejection of a late reply from an earlier batch. | Endpoint-operation interoperability with an external implementation and physical port transitions. |
| §§7.5.1, 8, congestion control | Nominal per-packet accounting, modulo 24-bit feedback, congestion-window admission and RTT/ECN-driven NSCC adjustments. The public QP binds an immutable ONLINE NSCC or disabled CC profile. Unit tests drive the configured maximum window, base RTT, target delay, thresholds, and increase and decrease parameters. The NSCC requester applies receiver-window penalties and restores the saved window on explicit feedback. Responder penalties can be set and cleared independently per QP. A controller profile can enable Quick Adapt. Unit tests exercise averaged-delay and trim triggers, the achieved-byte gate, the old-flight byte accounting phase, and interaction with receiver window limits. The Linux fabric verifies kernel CE marking, forwarded trims, and alternate-rail recovery. | Deployment measurements, link-rate-derived tuning for custom profiles, hardware ECN, and switch-generated trimming. |
| §§9–10, EVs and APIs | Counted QP rails with configurable UDP source-port EVs, IPv6 flow-label reflection, preserved or already decapsulated SRv6 receive, and alternate-path retransmission. The OCP controller ABI manages persistent UDP, STEV, and SRv6 format profiles with programmable field widths. It persists AUTO, explicit, and generated EV profiles and GOOD/DENIED state and gates mutations on CAP_NET_ADMIN. Public QPs lease ONLINE EV and CC profiles. Explicit and generated SRv6 profiles supply per-rail outgoing uSID destinations with optional additional SRH containers on requester QPs. The controller can update administrative EV state during transfer and configure CC profiles. Subscribed GOOD and ASSUMED_BAD transitions reach the controller's EV event CQ across processes. The controller also resolves and administers Linux-backed RoCE ports. Tests compare administrative queries with the Spark-04 interface flags and exercise the kernel setter on isolated virtual links. | Physical port administration under load and hardware uSID forwarding. |

## Specification questions for interoperability

- The endpoint uses the configurable UDP destination and default 4971 specified by §7.5.5.2. Table 7-9 shows the RoCEv2 value 4791, which remains selectable with `--udp-port 4791`.
- Sections 7.5.1 and 7.5.5 describe the PDC IDs differently. The implementation uses the requestor's QPN low 16 bits as SPDCID and the responder's QPN low 16 bits as DPDCID, following Tables 7-6, 7-15, and 7-17.
- Section 6.3.1 directs responders to silently drop outside-window PSNs. Table 7-3 names `PSN_OOR_WINDOW` as a mandatory responder NACK. The endpoint applies the NACK to valid forward PSNs beyond the tracking window. It silently drops pre-start and ambiguous half-space PSNs. The interpretation still needs independent interoperability verification.
- Table 6-5 specifies whole-WR DMA length in every RETH. The endpoint uses that value and validates each payload within the registered region.
- Polatis connects Ethernet links optically. The Spark double ring can verify path use and recovery but does not itself supply ECN marks or packet trimming.

## Verification

From `mrc-dpdk/`, run `make check` for wire, QP, work-request, congestion, connection negotiation, provider resource and QP lifecycle, EV, controller storage, and receive-window tests without DPDK. Build with `make` and run `python3 tests/check_mrc_write.py` for the IPv4 and IPv6 endpoints over two local DPDK memif links. This checks short and zero-length WRITEs, striping, separate ACK semantics, mixed WRITE and WRITEIMM lengths and receive sequencing across loss and PSN wrap, ordered WRITEIMM completions, receive-credit exhaustion, bad-rkey errors, ordered error and flush completions, node port-status updates, correlated reliability probes, TCP QP negotiation, PSN wrap, packet loss, probe-driven path restoration, a live controller update with custom IPv6 EVs, negotiated receive-window shrink, mandatory reliability NACKs, and synthetic trimming of bare IP and preserved SRv6 frames with either inner IP family.

With DPDK's pcap PMD, `python3 tests/check_mrc_pcap.py` verifies the
responder through independent capture generation and interpretation.
Build `tests/mrc_memif_relay` with a memif-enabled DPDK and run
`python3 tests/check_mrc_requester.py` to exercise the requester with an
independent live peer. Both scripts cover IPv4, IPv6, and preserved SRv6
with and without a final SRH. The relay moves Ethernet frames and the
Python peer handles wire encoding, memory placement, and feedback.

For §6.3.1, the standalone memif cases exchange WRITEIMM and Port Status
between full and limited members of the same partition, with either peer
acting as requester. Two limited members and distinct partitions produce
no data placement or feedback. Direct and negotiated connected routes use
the same RC membership check. TCP-negotiated routes also pin the offered
peer P_Key. [Linux RXE P_Key comparison](https://github.com/torvalds/linux/blob/v6.17/drivers/infiniband/sw/rxe/rxe_verbs.h)

For §§6.3.1 and 7.4.4, independent IPv4, IPv6, and preserved SRv6
replays verify the receive-window boundary across PSN wrap. Untrimmed
and trimmed WRITEs before connection start or exactly half a PSN
space away produce no feedback. Forward window overruns produce a
`PSN_OOR_WINDOW` NACK, and the valid next WRITE completes.

For §§6.2.2.2 and 6.3.1, public memif tests check 256- and 512-byte
segment lengths, address increments, and WRITEIMM completion across
PSN wrap. The standalone tests verify mixed work and smaller-PMTU
selection in both directions of an asymmetric TCP offer. Independent
captures verify requester payload, RETH geometry, and immediate data at
2048 and 4096 bytes. Separate replays verify out-of-order 4096-byte
placement over IPv4, IPv6, and preserved SRv6 with and without a final
SRH. Jumbo transfers on the physical NICs remain unverified.

For §7.4.8, one Port Status report with a zero mask pauses writes on
two QPs with overlapping membership and pinned-key filters. A second
report reopens both rails and both QPs complete the queued writes.
The public memif case observes one reply per report on the wire and
verifies that attaching a QP with a peer key from another partition
returns `EACCES`. Use `--pkey-port-status-only` to isolate this case.

For §§10.1.1 and 10.2.1, the public tests create a requester with both
WRITE flags and a responder with one receive SGE. They verify normal
WRITE and WRITEIMM completions, payload placement at the RETH address,
and the receive SGE buffer's original contents. The ABI unit test
checks creation and query attributes, rejection of a READ flag, and
linked receive posting that preserves the valid prefix on error.
[OCP QP example](https://github.com/opencomputeproject/OCP-Multipath-Reliable-Connection/blob/main/examples/mrc_qp.md)

For §7.5.2, the independent replay resends a buffered middle segment
with an ACK request and then fills the preceding gap across PSN wrap.
The responder emits a cumulative SACK before processing the final segment
and completes the WRITEIMM once. The replay covers IPv4, IPv6, and
preserved SRv6 with and without a final SRH.

For §§7.4.4 and 9.3.2, the independent peer returns reliability feedback
on the opposite rail. It sends two valid NACKs to trigger retries and
replays the first retry's NACK while the second retry is outstanding.
The requester uses the reflected transmit entropy to distinguish the
attempts. It accepts both cross-rail probe replies and completes the
WRITEIMM with exactly two retries. Use `--late-nack-only` to run the
replay across both IP families and both preserved SRv6 forms.

For §7.4.5, the requester waits one quarter of the initial ACK timeout
and requires delivery feedback for 16 later packets before an original
transmission is eligible for fast retransmission. The independent peer
holds a 24-packet WRITEIMM behind a single far-ahead SACK, then releases
the delayed packets without a retry. A second case delays a NACK-driven
retransmission beyond the same threshold and observes exactly one retry.
A third drops the original packet and confirms one fast retry before
the local ACK timeout. Use `--fast-loss-only` with `--mode` to check
IPv4, IPv6, and preserved SRv6 with or without an SRH.

The standalone memif suite uses 1, 2, 3, 4, and 8 configured rails for
WRITEIMM across PSN wrap. It verifies per-rail data and probe counters,
including traffic on rail 7 and isolation of that rail by deny mask
`0x80`. The public counted QP case completes an 8,193-byte WRITEIMM and
observes data on all eight rails. It also checks empty, oversized, and
duplicate port lists. The legacy QP attachment APIs use the same
implementation. Use `--rails-only` for the standalone cases or
`--counted-rails-only` for the public cases. For §9.3.2, the backpressure
case rejects WRITE submissions on multiple queues. The requester finds
an eligible queue in the same poll, then uses a different queue after
the receiver discards one packet. The receiver checks both rail indices,
the retransmit bit, the WRITEIMM completion, and the full buffer.
The mapped EV cases use four EV entries on two rails and deny one entry
without disabling its rail. The receiver observes the three eligible EVs
in WRITE packets and reliability probes on their assigned ports, then
verifies payload and completion. Coverage includes explicit, generated,
and AUTO UDP profiles and explicit SRv6 destinations. The SRv6 fixture
also checks that SACK replies reflect each probe's entropy. The store
suite verifies legacy snapshots, persistence of the port mapping, and
rejection of a mapping change while the profile is ONLINE.
The controller test generates eight EVs, grows the offline profile to
32 entries, then shrinks it to four. Each size is brought online and
queried to check the EV value and port order. The counted connectionless
controller case sends an EV probe and a Port Status Update on port 8
and observes both replies there. The receiving fixture checks that bits
1 and 8 in the API mask become bits 0 and 7 on the wire. It also verifies
rejection of ports and mask bits outside the configured rail list.

For §7.5.5.1, the public memif case selects SL 7 with `IBV_QP_AV` and
observes DSCP 26 for the initial WRITEIMM and reliability probes, DSCP 34
for the retry, and DSCP 48 for SACK, NACK, and Transport ACK. A different
SL 0 mapping and a post-attach edit verify the selected mapping and its
attachment lifetime. The independent requester cases verify the data
and retry DSCPs over IPv4, IPv6, and both preserved SRv6 forms, including
the outer class and the ECN bits. They also verify the standalone
default that retries use the data DSCP when no retry class is specified.

For §7.5.3, each responder replay sends a shortened regular trim and
an intact WRITE marked with the configured last-hop DSCP. The replies
carry distinct NACK reasons, the original UDP lengths, and the
triggering PSN, timestamp, and RTX flag. A subsequent probe observes
unchanged receive state. A valid retransmission places the intended
bytes and produces the expected Transport ACK. The requester fixtures
verify alternate-rail retries for both reasons. Only regular TRIM
puts the reported EV in SKIP. The public memif test also verifies that
the responder sends a last-hop NACK with the peer capability marked
unsupported. The marked WRITEIMM leaves destination bytes and the
receive CQ untouched until the RTX arrives on the other rail.

Use `--port-status-only` to isolate the Port Status cases. The peer
withholds a local report response, verifies a fresh periodic report,
and gates the next WRITE with a zero mask. It exercises both an
advertised path restoring progress and progress resuming after a
refreshed zero mask expires. Replacement reports use a numerically
lower identifier to exercise identifier opacity. Use
`--retry-timer-only` to verify the §7.4 retry rules with an independent
peer. For a single WRITEIMM, the peer withholds WRITE feedback and
answers probes. It observes the base timeout, then the doubled timeout,
then sends a valid NACK for the outstanding retry. The requester
retransmits on the other rail and the next timeout uses the base value.
A zero-budget case observes immediate retry exhaustion after its NACK.

`python3 tests/check_mrc_abi_application.py --blackout-only` checks
§7.4.1 and §7.4.2 through the public ABI. A memif ingress filter drops
the initial WRITEIMM, one retransmission on the other rail, and probes
on both rails. With three exponential retries, the remaining retry slots
expire while both EVs are inactive. The requester enters ERROR and
reports `IBV_WC_RETRY_EXC_ERR` and `QP_FATAL`. With infinite retries, the
QP stays RTS for 1.5 seconds of silence, beyond the finite deadline.
Restoring one rail lets a probe requalify it so the pending WRITEIMM
completes on both peers. The filter verifies that no additional WRITE
packets use the inactive rails.

Use `--flow-control-only` to isolate the receiver flow-control case.
The peer applies a maximum penalty, withholds feedback to verify
single-packet admission, then clears the penalty with restore and verifies
that multiple packets can leave before the next SACK. Use
`--probe-correlation-only` to isolate the requester probe case. It checks that replies with a wrong token, wrong reflected
entropy, or an already-used token cannot change sender state. It also
checks that an unanswered rail emits a fresh probe and that a valid reply
with an arbitrary reserved BTH PSN is accepted. The pcap replay exercises
the responder with a nonzero reserved BTH PSN and a separate probe with
wrong PDC identifiers. Only the valid probe produces a SACK, without
changing the receive PSN or byte count. Service Time replays in all four
envelopes check standalone durations, 16-bit timestamp wrap, and exact
reflection for opaque timestamp resolutions. The replies include ordinary
and probe SACKs, NACKs, and EETHs. Portable tests verify RTT subtraction
with the preserved ingress time and discard invalid RTT samples while
retaining delivery accounting.
These tests measure software service intervals. Accuracy against NIC
timestamps remains unverified.

`make tests/mrc_endpoint_application` and `python3 tests/check_mrc_application.py`
verify that an application can start a DPDK requester on an empty provider
QP, post new work while it runs, and poll its own CQ. The test starts after
a QP reset with a noninitial work sequence. It verifies WRITE/WRITEIMM
placement and completion IDs across separate admissions, PSN wrap, and
continued immediate receive sequencing over two memif rails.
The link lifecycle case stops one memif port and restarts it while the
same QP remains attached. It verifies both rails before the stop, only
the surviving rail during the stop, and both rails after restart. A
test-only PMD query error after the DOWN observation checks that the
requester retains the last known state.

Use Spark-04 as the implementation source. After pushing the exact clean commit, run `tests/verify_spark_revisions.sh --sync "$(git rev-parse HEAD)"` to advance the detached test worktrees on Spark-02 and Spark-03. Run the same command without `--sync` immediately before each live test. The script checks that all three worktrees are clean and at the requested commit.

Build `tests/mrc_abi_application` against the OCP headers and run
`python3 tests/check_mrc_abi_application.py` to exercise both endpoints
through the public shared library. It exchanges verbs GIDs, QPNs, PSNs
and MR keys, checks placement across scatter/gather and PSN wrap, confirms
signaled send CQEs and two WRITEIMM receive CQEs. It verifies the
solicited WRITEIMM wakes the completion channel and a subsequent ordinary
WRITEIMM leaves a solicited-only arm pending. Its shared-port case crosses two bidirectional QPs per
peer with separate registered destinations and completion IDs, then resets
one QP and exchanges further WRITEIMM traffic on the surviving QP. A
separate two-QP case observes the penalty and restore bits in ordinary
SACKs across four live settings and checks that changing one responder
QP leaves the other's settings independent. The same case pairs eight
startup PETH requests with probe SACKs across both QPs,
rails, and directions. It checks the destination QPN, PDC identifiers,
probe token, reflected entropy, and the first WRITE's negotiated PSN.
It checks Service Time device capability and configures the remote
responder's unsupported flag on opposite QPs at the two endpoints.
Each local responder continues reporting Service Time on both QPs.

The inline case accepts a linked WRITE prefix, reports the oversized
next WR, then posts a multi-SGE WRITEIMM and an empty inline WRITEIMM.
The requester overwrites and frees its source buffers before progress.
The receiver drops one fragment, observes its retransmission, and
checks the placed bytes and completion events. Provider tests exercise
payload lifetime through completion, error flush, and reset.

Two public SRv6 cases inspect distinct outer SIDs on both rails in
requests and return traffic. One rail uses a uSID destination without
an SRH. The other carries a single additional container. One case
independently decapsulates both rails before responder ingress. The
transit case preserves encapsulation and advances the SRH from its
initial destination and Segments Left 1 to its final SID and Segments
Left 0 before responder ingress. Both cases independently decapsulate
the SRH rail before requester ingress and verify payload and completion
results. A preserved probe addressed to the other rail's SID produces
no additional reply.

The Linux SRv6 fabric test sends a 256 KiB registered WRITEIMM across
two independent namespace routers and verifies the received data. Both
routers advance a real SRH with the kernel's `End` action. One preserves
the envelope and the other decapsulates it with `End.DX6`. Each run
checks the kernel route counters and the negotiated MPR update from eight
to two units. The ECN case uses nftables to mark the outer IPv6 header
on the preserved rail and checks the requester's EV skip counter.
The trim case sends an endpoint-injected trim through the preserving
router, checks the TRIM NACK, and verifies retransmission through the
decapsulating rail. A hardware uSID fabric and switch-generated trimming
remain separate verification targets.

The link case brings a sender veth interface down and back up during
one 4 MiB WRITEIMM. nftables observes Port Status masks `3, 1, 3`
on the surviving rail. Kernel route counters verify continuing traffic
there while rail 1 is down and renewed traffic on rail 1 after restore.
The receiver verifies all 4,096 segments and one immediate completion.
The prior AF_PACKET endpoint did not send the down mask because the PMD
kept its startup link state. A separate veth fixture verifies that
carrier drops when the peer goes down while the local port stays
administratively enabled. ConnectX-7 link transitions remain to be tested.

`make check-mrc-abi` also verifies asynchronous event ordering and
acknowledgement, nonblocking retrieval, object destruction, and the effect
of CQ overrun on a second QP sharing the CQ. It checks QP hint capacity,
masked attributes, same-context sharing, cross-context rejection, and
release after reassignment, reset, and destruction. The public memif error case
overruns a receive CQ and checks `CQ_ERR` followed by `QP_FATAL`. On the
requester, the remote `UNEXP_EVENT` produces an error WC and `QP_FATAL`.
Two additional public memif cases submit a WRITEIMM to a responder with
zero WRITEIMM slots despite an OOB limit of one at the requester, or to a
region without remote write permission. The responder enters ERROR and
flushes both receive WRs without placing data. Its application acknowledges
one `QP_REQ_ERR` or `QP_ACCESS_ERR` event. The requester sees the corresponding
`REM_INV_REQ_ERR` or `REM_ACCESS_ERR` completion and one `QP_FATAL`.
[libibverbs event types](https://github.com/linux-rdma/rdma-core/blob/master/libibverbs/man/ibv_get_async_event.3)

The ABI suite injects Linux uverbs records to check blocking wakeup, polling,
port validation, concurrent availability with provider CQ events, and a single
device-fatal notification when the native event stream closes. It also exercises
provider events when native allocation is unavailable. On Spark-04 the uverbs
allocator opened distinct event descriptors for MRC contexts sharing a verbs context.
Closing MRC contexts preserved the application's verbs descriptor and flags.

The shared-library controller test verifies capabilities, profile
transitions, per-entry batch errors, persistence, and EV and CC QP leases.
It also verifies disabled CC selection and rejects controller CC queries and
mutations without the required capability. Generated profile tests cover
field bounds, sparse masks, fixed fields, skipped UDP port zero, stable
ordering and persistence, and live administrative state under a lease.
SRv6 controller tests check both container widths, explicit values,
generated combinations, rejection of unusable containers, and per-port
EV identity. Programmable format tests cover protected state transitions,
persistence, field packing across byte boundaries, zero-padded fields, and
stale EV bindings after a format change. The portable store tests verify
older snapshots and mixed snapshots with numeric and SRv6 values. Build the ABI test
with `make MRC_INCLUDE_DIR=/path/to/headers tests/test_mrc_ctl_abi`. Run
`./tests/test_mrc_ctl_abi --expect-no-cap` without CAP_NET_ADMIN and
`unshare -Ur ./tests/test_mrc_ctl_abi` for the isolated positive case.
The controller ABI test compares the RoCE port's reported administrative
state with Linux interface flags. The test of enabling and disabling a
Linux interface can run in a disposable network namespace.

```sh
make tests/test_mrc_port_netlink
unshare -Urn sh -ec 'ip link add mrc-test-a type veth peer name mrc-test-b; exec ./tests/test_mrc_port_netlink --veth'
```

Run `unshare -Ur ./tests/test_mrc_abi` for synthetic event CQ coverage of
legacy and current array layouts, completion notifications, one reader per
store, and bounded overflow with loss reporting. Run
`python3 tests/check_mrc_write.py --event-controller tests/test_mrc_ctl_abi`
to observe actual EV failure and recovery from a separate controller
process. Both 16 MiB cases restore rail 2 after a blackhole. The first
reports ASSUMED_BAD and GOOD for the configured port and EV. The
BAD-only subscription reports just ASSUMED_BAD.

With `tests/mrc_abi_application` built, run
`python3 tests/check_mrc_abi_application.py --controller tests/test_mrc_ctl_abi`
to verify two-rail transfer while a separate controller process updates the
profile. Both EVs start DENIED and neither peer completes work. The
controller makes rail 2 GOOD while an active QP holds the profile, and
the WRITE and WRITEIMM operations complete with verified data. Two additional
public QP cases bind generated UDP and STEV profiles. The fixture observes
the expected values on each rail in WRITE and PETH requests and in the
corresponding SACKs, then verifies payload placement and completions.
The same suite drives connectionless EV probes and Port Status Updates in
three wire formats. It holds a request to reverse reply order, withholds
another past the batch timeout, and releases the late reply while a later
batch is active. A shared-port case also answers the controller while a
matching responder QP is idle.
The STEV fixtures use an IPv6 verbs GID. A programmable STEV case also
observes the flow-label and UDP bits from a 12/10/10-bit layout.
The two controller SRv6 cases
observe the leased uSID values in requests on both rails and verify
the reflected inner entropy and both peers' completions. The explicit
case enables initially denied rails through the controller. The generated
case observes the initial destination and the additional SRH container
and simulates the remaining segment transition before DPDK receive.
A separate NSCC case programs a one-packet window and observes both DPDK TX rails
while the peer withholds feedback. It sees one full-sized WRITE with
BTH ACK Request before the peer resumes and validates payload placement
and completions in both directions. The Quick Adapt case uses an enabled
controller profile and observes a three-packet baseline burst. After a
short low-latency WRITE primes a new observation window, the responder
defers the next short WRITE's SACK for at least 10 ms. The resulting
send window admits only one full-sized packet before feedback. The
fixture checks that the delayed SACK carries neither an ECN mark nor a
receiver window penalty, then verifies all four WRITE completions and
their payloads.

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

The public memif cases use `mrc_modify_qp()` with `MRC_QP_MPR_DEST` to
lower and restore the advertised limit on attached RTR and RTS responders.
The peer has already sent each WRITEIMM before the application changes the
limit.
The SACKs and both directional queries report `3, 1, 3` for RTR and
`255, 1, 255` for RTS across PSN wrap. All three WRITEIMMs in each case
place the expected bytes and complete on both peers. The static case
returns zero in every SACK and keeps the negotiated limit of three in
both queries.

A local memif run verified eight 256 KiB WRITEIMMs with an in-flight limit
of two and provider-registered memory. The receiver lowered MPR from
eight units to one after 65 accepted packets. Both peers applied one MPR
update and completed all eight work requests. The targeted case passed
with both the installed Spark snapshot revision and the development build.
The corresponding physical run needs the scoped negotiated pair profile.

For §§7.4.4 and 7.5.4, the portable tests fill the requester window with
32,640 entries, check the last valid receive PSN and first overflow, and
exercise signed SACK offsets near the end of the window. A negotiated
memif WRITEIMM sends 32,641 segments at PMTU 256, crossing PSN wrap and
reusing tracking slots. Both peers apply the responder's change from
255 to 32 units after 512 accepted packets. The receiver checks the
entire buffer and consumes one WRITEIMM credit.

### Concurrent three-Spark verification

The [Spark cycle tests](README.md#concurrent-three-spark-tests) passed
both profiles on the Polatis double ring with three simultaneous
256 MiB WRITEs per run. Before each run, all three test worktrees matched
`0c537484a96b15e51c9cfd82491de83d65d554f4` and the installed binaries
matched SHA-256 `dfd4498a8f3e965409f4f182767a126f29a3e23e9f6a9ff96406bb976eac7305`.
The sender port-ready intervals overlapped by at least 1,944.2 ms in
smoke and 2,124.3 ms in failover.

| Edge | Smoke RX segments by rail | Smoke TX retries by rail | Failover RX segments by rail | Failover TX retries by rail |
| --- | --- | --- | --- | --- |
| Spark-04 to Spark-02 | 135,361 / 126,783 | 446 / 0 | 142,551 / 119,593 | 18,536 / 22,614 |
| Spark-02 to Spark-03 | 131,188 / 130,956 | 22 / 16 | 130,645 / 131,499 | 22,265 / 21,261 |
| Spark-03 to Spark-04 | 131,420 / 130,724 | 209 / 224 | 128,922 / 133,222 | 61,416 / 57,998 |

Each receiver verified the written buffer. The failover profile
used ACK timeout exponent 13 and suppressed rail-1 traffic on Spark-04
from scheduled segments 512 through 2,047. Spark-04 recorded 127
injected drops and its EV failure and recovery masks included rail 1.
The failover journals also reported missed RX frames on all three
requesters. DPDK reported zero port I/O errors and RX buffer exhaustion
in both profiles.

mlx5 setup requires `CAP_NET_RAW` and `CAP_NET_ADMIN` or an administrator-run endpoint. [DPDK mlx5 privilege documentation](https://doc.dpdk.org/guides-23.11/platform/mlx5.html#run-as-non-root)
