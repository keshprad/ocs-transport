# OCP MRC 1.0 conformance status

**Current status — 2026-11-22:** 95 of the 98 transport requirements have
software evidence within the documented scope, one (M81) is conditionally
not applicable, and two (M30, M59) remain unresolved. M15 silent dropping is
fixed and tested. M31 and M89 are verified under explicitly documented
contextual interpretations, not upstream errata. Full conformance and
independent hardware interoperability are not established.

M30 needs clarification of mandatory responder PSN_OOR_WINDOW generation
versus outside-window silence; M59 needs a defined PSNE trigger/wire encoding.
The requested software fixes and focused verification are complete. The user
cleared the tracking goal; this does not close either normative requirement.
Detailed evidence, limits, and historical runs are retained below; temporary `/tmp` logs are local
artifacts, not durable published CI evidence.

The conformance target is the [OCP Multipath Reliable Connection 1.0 specification](https://www.opencompute.org/documents/ocp-mrc-1-0-pdf/). The SRv6 emulator is a reference for path behavior and reorder metrics. An independent MRC endpoint or qualified packet captures are needed to verify wire interoperability.

| Scope | Verified in the current slice | Remaining evidence or implementation |
| --- | --- | --- |
| §§6.1–6.4, 10.1.2, request and completion semantics | Mixed WRITE and WRITEIMM, per-WR geometry, registered-memory leases, ordered immediate completions, remote access errors, RQ exhaustion without RNR, and typed error/flush completions are verified. The OCP library exposes verbs contexts and device queries, CQ/channel events, and public QP create, modify, query, reset, destroy, and linked WR posting. It accepts WRITE and WRITEIMM creation flags and receive posts with or without SGEs. It keeps directional PSN, MPR and WIMM attributes separate, binds actual `ibv_mr` keys by verbs PD, and supports registered scatter/gather buffers and post-time copies of inline WRITE and WRITEIMM data. QP hints are queryable advisory attributes with validated INIT association and lifetime checks. The DPDK worker carries public WRs across two memif rails, dispatches feedback and requests by QPN and opcode, and progresses multiple bidirectional QPs on the same ports. Solicited WRITEIMM notifies an armed solicited-only receive CQ. The MRC context exposes asynchronous CQ overruns, typed responder request and access errors, QP fatal errors, and native device and port events on its pollable FD. Active QPs that share a failed CQ flush work when their next operation observes the failure. | Native event delivery after a physical port transition and independent hardware interoperability for application-managed OOB attributes. |
| §§6.2.2, 7.5.5, wire format | Independent byte vectors for IPv4/UDP, IPv6/UDP, and preserved SRv6 envelopes, iCRC, WRITE with immediate and TSETH, AETH, SETH, NETH, and PETH. Independent pcap replays and live Python feedback verify IPv4, IPv6, and preserved SRv6 with or without a final SRH. Checks cover iCRC, structured EV reflection, CE marking, PSN wrap, packet rejection and receiver QP ERROR for valid requests with an unsupported opcode. Independent packet-buffer, TRIM, and last-hop TRIM NACKs trigger alternate-rail retry. | Interoperability with an external MRC implementation, transit on independent SRv6 switch hardware, and remaining edge-case packet validation. |
| §§7.2, 7.4–7.5, packet reliability | 24-bit PSN and window checks, wrap, selective SACK bitmaps, separate AETH, duplicate suppression, retransmit marking, timeout backoff, a base timer after NACK-driven retransmission, finite retry exhaustion and unlimited-retry recovery when both rails go silent, SACK-driven fast loss, reliability probe RTT sampling and startup retry per rail (probe exchanges observed on the Spark pair), and separately configured regular and last-hop trim classes with distinct NACKs and recovery. Negotiated Dynamic MPR advertises each change in SACKs and gates both first transmissions and retries, while preserving outstanding receive state. Resource NACKs and terminal CQ overrun are generated and handled over memif. Independent peers inject PSN_OOR_WINDOW to verify requester retry and exhaustion; responder generation remains unresolved under M30 because M15 requires silent dropping. | Independent MRC-peer NACK interoperability and peer/carrier-loss recovery beyond the tested local administrative transition. Both receiver Sparks verified a 16 MiB WRITE after an injected path-1 blackhole. An independent memif peer checks alternate-rail retry on a packet-buffer NACK and confirms that a WRITEIMM credit stays occupied until a semantic ACK. Provider QP errors and typed send completions are verified for retry exhaustion and UNEXP_EVENT. Trim NACK generation follows the local responder capability. Requester Service Time compensation follows the peer responder capability. The responder reports DPDK RX-to-feedback preparation time or adjusts a 128 ns request timestamp. |
| §§7.3, 7.4.7–7.4.8, 7.5.6, endpoint operations | ERTH/EETH byte fixtures and two-rail Port Status Updates observed over memif and the Spark pair. An independent peer verifies reserved QPN 2, Control traffic class, echoed opaque 16-bit identifiers, fresh reports after a lost response, single-rail gating after a second report, and expiry when reports stop. The privileged controller batch API also sends EV probes and Port Status Updates without a QP. Wire checks cover UDP, STEV, and SRv6 EVs, selectable request DSCP, Control replies, returned rail and RTT, partial timeouts, and rejection of a late reply from an earlier batch. | Endpoint-operation interoperability with an external implementation and physical port transitions. |
| §§7.5.1, 8, congestion control | Nominal per-packet accounting, modulo 24-bit feedback, congestion-window admission and RTT/ECN-driven NSCC adjustments. The public QP binds an immutable ONLINE NSCC or disabled CC profile. Unit tests drive the configured maximum window, base RTT, target delay, thresholds, and increase and decrease parameters. The NSCC requester applies receiver-window penalties and restores the saved window on explicit feedback. Responder penalties can be set and cleared independently per QP. A controller profile can enable Quick Adapt. Unit tests exercise averaged-delay and trim triggers, the achieved-byte gate, the old-flight byte accounting phase, and interaction with receiver window limits. The Linux fabric verifies kernel CE marking, forwarded trims, and alternate-rail recovery. | Deployment measurements, link-rate-derived tuning for custom profiles, hardware ECN, and switch-generated trimming. |
| §§9–10, EVs and APIs | Counted QP rails with configurable UDP source-port EVs, IPv6 flow-label reflection, preserved or already decapsulated SRv6 receive, and alternate-path retransmission. The OCP controller ABI manages persistent UDP, STEV, and SRv6 format profiles with programmable field widths. It persists AUTO, explicit, and generated EV profiles and GOOD/DENIED state and gates mutations on CAP_NET_ADMIN. Public QPs lease ONLINE EV and CC profiles. Explicit and generated SRv6 profiles supply per-rail outgoing uSID destinations with optional additional SRH containers on requester QPs. The controller can update administrative EV state during transfer and configure CC profiles. Subscribed GOOD and ASSUMED_BAD transitions reach the controller's EV event CQ across processes. The controller also resolves and administers Linux-backed RoCE ports. Tests compare administrative queries with the Spark-04 interface flags and exercise the kernel setter on isolated virtual links. | Physical port administration under load and hardware uSID forwarding. |

## Requirement-to-evidence audit (2026-11-21)

This audit covers the 98 `MUST`/`MUST NOT`/`SHALL` occurrences in the
normative transport sections of the OCP MRC 1.0 PDF dated 2026-03-21,
the two additional `MUST` clauses in the controller header, and all 57
symbols exported by `libmrc-dpdk.so`. IDs M01–M98 follow PDF text order;
a grouped row covers each listed occurrence, not just one representative
requirement. Definitions and licensing text are excluded. Conditional
requirements are identified explicitly. Referenced IBTA and UET specifications
are not expanded into an additional claim of complete IBTA/UET conformance.

The PDF SHA-256 is
`d59de939f159272c54ec33f924739944431dd5e92a3d3b232bc15315b052fd27`.
The application and controller header SHA-256 values used for this build are
`768dac13a433a434b737b3c9fa07f5b32d4063576490ff8696355d492205a6ad`
(`mrc.h`) and
`27b93a05e9fe90e096ff3e401dfdac8567267a7b850315a0efc8e65108f4ee2b`
(`mrc_ctl.h`). The tested checkout is based on
`32d67ee5464821c0ad19fbc8e35caa1a5bd3927f` plus the working changes described
below. The [linked historical record](https://github.com/keshprad/ocs-transport/blob/f0dbcba03cba4e050a5b6b7a3cbdac952474e681/mrc-dpdk/CONFORMANCE.md)
is background evidence, not the revision tested in this audit.

**Status:** **V** = directly asserted by the named local tests; **P** =
partial evidence with the missing check stated; **Q** = specification or
lifecycle interpretation remains unresolved; **H** = external hardware or
fabric evidence required; **N/A** = conditional behavior not implemented or
advertised. V establishes the tested software behavior, not independent
hardware interoperability. Historical physical runs below retain their own
revision and binary hashes and do not validate subsequent working changes.

### Test identifiers and reproducible entry points

Paths below are relative to `mrc-dpdk/`. A function or case name following an
identifier identifies the assertion to inspect, rather than merely a source
file that mentions the feature.

| ID | Test artifact / command | Evidence scope |
| --- | --- | --- |
| U-QP | [test_mrc_qp.c](tests/test_mrc_qp.c), `make check` | PSN arithmetic, receive and send windows, SACK rotation, delivery versus completion, timers, maximum window, invalid trigger state snapshots. |
| U-WIRE | [test_mrc_wire.c](tests/test_mrc_wire.c), `make check` | Independent byte fixtures, encode/decode boundaries, CRC, control headers, IPv4/IPv6/SRv6. |
| U-WR | [test_mrc_wr.c](tests/test_mrc_wr.c), `make check` | OOO placement, immediate ordering, malformed geometry, MR bounds, staging and resource errors. |
| U-PROVIDER | [test_mrc_provider.c](tests/test_mrc_provider.c), `make check` | Registered memory, inline/scatter-gather lifetime, CQ notifications/errors, ordered flush and reset. |
| U-CC | [test_mrc_cc.c](tests/test_mrc_cc.c), `make check` | Byte accounting, RTT/Service Time, flow control, NSCC and Quick Adapt formulas. |
| U-NEG | [test_mrc_connect.c](tests/test_mrc_connect.c), `make check` | Strict OOB records and all 729 valid support/offer combinations for three optional capabilities. |
| U-MPR | [test_mrc_mpr.c](tests/test_mrc_mpr.c), `make check` | Static/dynamic limits, zero/stale/oversized updates, outstanding retry bounds and wrap. |
| U-EV | [test_mrc_ev.c](tests/test_mrc_ev.c), `make check` | Active set, DENIED/SKIP/ASSUMED_BAD transitions and probe recovery. |
| U-STORE | [test_mrc_ctl_store.c](tests/test_mrc_ctl_store.c), `make check` | Persistent profiles, leases, concurrent edits, generated/custom fields, SRv6 and CC. |
| ABI | [test_mrc_abi.c](tests/test_mrc_abi.c), `unshare -Ur ./tests/test_mrc_abi` | Public context/QP/CQ/MR/hint/event APIs; requires an accessible verbs device. |
| CTL | [test_mrc_ctl_abi.c](tests/test_mrc_ctl_abi.c), `./tests/test_mrc_ctl_abi --expect-no-cap` and `unshare -Ur ./tests/test_mrc_ctl_abi` | Controller discovery, privilege checks, formats, profile lifecycle, batches and leases. |
| APP | [check_mrc_abi_application.py](tests/check_mrc_abi_application.py), `python3 tests/check_mrc_abi_application.py --controller tests/test_mrc_ctl_abi` | Public API processes over memif, including controller changes and connectionless operations. |
| EP | [check_mrc_application.py](tests/check_mrc_application.py), `python3 tests/check_mrc_application.py` | Application-owned work on a running endpoint and injected link-query transition. |
| WRITE | [check_mrc_write.py](tests/check_mrc_write.py), `python3 tests/check_mrc_write.py --event-controller tests/test_mrc_ctl_abi` | Standalone memif endpoints, TCP OOB, all NACKs, limits, live profiles and real cross-process EV events. |
| PEER | [check_mrc_requester.py](tests/check_mrc_requester.py), `python3 tests/check_mrc_requester.py` | Independent live Python responder; cross-rail SACK/NACK, flow control, probe correlation and retry timing. |
| RTT | [check_mrc_endpoint_rtt.py](tests/check_mrc_endpoint_rtt.py), `make check-endpoint-rtt` | 864 independent wire/exact-clock cases through production decode and requester control; SACK/probe/NACK, four directional offers, four envelopes, service bounds and retry ambiguity. |
| SVC-API | [check_mrc_public_service_time.py](tests/check_mrc_public_service_time.py), `make MRC_INCLUDE_DIR=/path/to/pinned/headers check-public-service-time` | Live public bidirectional QPs; linker witnesses assert successful public capability settings reach actual requester/responder configuration. |
| MPR-PEER | [check_mrc_mpr_retry.py](tests/check_mrc_mpr_retry.py), `make check-mpr-retry` | Independent live peer with manually encoded OOB and feedback; shrink holds retry, CACK advance releases it, static and invalid updates leave valid retries eligible. |
| PCAP | [check_mrc_pcap.py](tests/check_mrc_pcap.py), `python3 tests/check_mrc_pcap.py` | Independent wire generation and inspection in IPv4, IPv6 and preserved SRv6 with/without SRH; requires pcap PMD. |
| CSID | [check_mrc_srv6_csid.py](tests/check_mrc_srv6_csid.py), `make check-srv6-csid` | Independent RFC 9800 header oracle and Linux NEXT-CSID forwarding in isolated namespaces: 30 vectors, 92 shifts, 18 SRH promotions, unchanged inner payload/iCRC. |
| PORT | [test_mrc_port_netlink.c](tests/test_mrc_port_netlink.c) | Read-only kernel port queries; `unshare -Urn sh -ec 'ip link add mrc-test-a type veth peer name mrc-test-b; exec ./tests/test_mrc_port_netlink --veth'` exercises isolated netlink mutation. |
| FABRIC | [check_mrc_srv6_fabric.py](tests/check_mrc_srv6_fabric.py) | Historical Linux namespace SRH forwarding, decapsulation, ECN, trim and jumbo evidence below; not a hardware uSID test. |

Build standalone tests with `make tests/mrc_write_ready`. The default WRITE
fixture waits for all requested rails before starting traffic; the production
CLI retains asynchronous startup and can be selected with `--binary ./mrc_write`.

Build public tests with `make MRC_INCLUDE_DIR=/path/to/pinned/headers
libmrc-dpdk.so tests/test_mrc_abi tests/test_mrc_ctl_abi
tests/mrc_abi_application tests/mrc_endpoint_application tests/mrc_memif_relay`.
Use the matching DPDK `PKG_CONFIG_PATH` at build time and `LD_LIBRARY_PATH`
at runtime. Tests use isolated temporary stores and memif sockets. Namespace
capabilities in these commands do not authorize modifying physical ports.

### Normative transport requirements

| ID | Source | Requirement | Status and direct evidence / remaining check |
| --- | --- | --- | --- |
| M01 | §6.2.1 | Complete requester WRs in posting order. | **V** — U-PROVIDER `test_qp_lifecycle_and_ordered_flush`; APP duplex, inline and `public_packet_boundaries`; WRITE deferred Transport NAK success/error/flush. |
| M02 | §6.2.2.1 | Calculate valid RoCE iCRC. | **V** — U-WIRE independent write/control fixtures and integrity tests; PCAP accepts valid vectors and rejects corrupted CRC. |
| M03, M04 | §6.2.2.2 | Segment at PMTU; increment per-packet VA by PMTU. | **V** — APP `public_packet_boundaries` at 256/512/1024; APP PMTU wire witness; PCAP `check_jumbo_requester` at 2048/4096 and `check_packet_boundaries`; WRITE asymmetric PMTU negotiation. |
| M05 | §6.2.2.2 | Enforce MR boundaries. | **V** — U-WR `test_reject_malformed_without_placement`; U-PROVIDER `test_bounds_and_permissions`; APP guard bytes and last byte exactly at MR end. Public malicious remote overrun remains narrower than the unit matrix. |
| M06, M07 | Table 6-10 | Support logical timers and fast loss detection/retransmission. | **V** — U-QP `test_local_ack_timeout`; PEER `--retry-timer-only`, `--fast-loss-only`. |
| M08 | Table 6-10 | Receive UET-style trimmed packets. | **V** — U-WIRE trimmed request tests; PCAP shortened and intact DSCP-marked frames, unchanged receive state, then valid retry. |
| M09 | §6.3 | Process OOO requests across messages, retain immediate data, generate Transport ACK/SACK/NACK. | **V** — U-WR `test_out_of_order_and_wrap`, `test_imm_resource_errors`; PCAP `check_deferred_receive_error`, `run_replay`; WRITE `check_reliability_nacks`. All three subordinate obligations are exercised. |
| M10, M11, M12 | §6.3.1 | Maintain ePSN; accept OOO in-window PSNs rather than requiring equality. | **V** — U-QP `test_receive_window_and_ar`, `test_maximum_window`, `test_receive_history_half_space`; PCAP OOO placement and boundary replay. |
| M13 | §6.3.1 | Generate semantic ACKs for accepted operations under transport rules. | **V** — PCAP `check_responses` inspects AETH PSN/MSN; WRITE/APP check ordered completions and Transport NAK errors. Inherited IBTA rules beyond supported WRITE operations are outside this inventory. |
| M14 | §6.3.1 | Recognize valid duplicates and acknowledge without repeating the operation. | **V** — U-QP receive-window and U-WR OOO tests; PCAP duplicate LAST_IMM, exactly one completion; WRITE dropped final ACK recovery. |
| M15 | §6.3.1 | Silently discard out-of-window requests. | **V** — intact and trimmed requests outside the window produce no feedback or placement. U-QP checks boundary/wrap/state preservation; PCAP checks independent wire silence followed by valid completion. Mandatory responder NACK generation is separately qualified under M30. |
| M16, M17 | §§6.3.2–6.3.3 | Complete WRITEIMM after prior placement; retain ImmDt until completion. | **V** — U-WR OOO/stash tests; PCAP deferred error and reordered WRITE/WRITEIMM; APP ordered immediate values across wrap. |
| M18 | §6.4 | Successful AETH advertises unsupported message-credit flow control (`0x1f`). | **V** — PCAP exact AETH vectors and `check_packet_boundaries`; WRITEIMM admission is separate negotiated resource accounting. Error AETH syndrome bits follow Transport NAK encoding. |
| M19, M20 | §7.2 | Reliability feedback cannot imply a transport outcome; use transport control for completion. | **V** — U-QP `test_delivery_and_transport_ack`; PEER holds WRITEIMM credit after SACK until Transport ACK; APP typed remote error completions. |
| M21 | §7.3 | Endpoint operations tolerate loss, reorder and missing retransmission guarantees. | **V** — APP `check_endpoint_operations`: reversed replies, timeout, stale reply in a later batch; PEER Port Status loss and replacement. |
| M22, M23 | §7.3 | Endpoint requests and replies use QPN 2. | **V** — U-WIRE `test_endpoint`; APP endpoint wire callbacks inspect both directions; PEER Port Status checks. |
| M24, M25 | §7.4.1 | Maintain Local ACK Timeout and allow per-QP configuration. | **V** — U-QP `test_local_ack_timeout_matrix` enumerates 6656 valid configurations plus invalid boundaries; ABI `test_public_qp`; APP finite/infinite blackout; PEER measured timeout/backoff. |
| M26 | §7.4.3 / Table 7-2 | Populate entropy according to UDP, STEV or SRv6 mode. | **V** — APP generated UDP/custom STEV and explicit/generated SRv6 cases inspect fields on both rails; CTL format field packing. |
| M27 | §7.4.4 | Set RTX on retransmitted data. | **V** — PEER NACK/timer fixtures, APP inline loss/backpressure and last-hop trim callbacks, PCAP reflected RTX. |
| M28 | §7.4.4 | Enter ERROR on non-retriable NACK or retry exhaustion. | **V** — WRITE `check_reliability_nacks`; APP blackout and wire-error cases check typed WC and QP_FATAL. |
| M29 | §7.4.4 / Table 7-3 | Requester handles every defined NACK. | **V** — WRITE resource/error recovery; PEER explicitly injects PSN_OOR_WINDOW for successful retry and exhaustion in all four envelopes; trim reception remains independent of optional generation. |
| M30 | §7.4.4 / Table 7-3 | Responder supports mandatory NACK generation. | **Q** — resource and unexpected-event codes are verified. PSN_OOR_WINDOW has no automatic generation path after enforcing M15 silence. Table 7-3 names outside-window PSNs, which §6.3.1 explicitly requires silently dropping. No alternative trigger is invented; this remaining conflict requires clarification. |
| M31 | §7.4.4 | Reset retransmission timer to the base value. | **V (contextual interpretation)** — feedback-driven NACK/fast-loss retransmissions reset to base; timeout-driven retransmissions retain §7.4.2 mandatory exponential backoff. PEER measures both paths in all four envelopes; U-QP covers timeout configuration. SRC_TIMER is not separately defined; this contextual reading is explicit, not an upstream erratum. |
| M32 | §7.4.4 | Return a supplied timestamp in SACK under timestamp rules. | **V** — PCAP `check_service_time` covers standalone, adjusted/wrapped and opaque timestamps; U-WIRE WRITE/PETH fixtures. |
| M33 | §7.4.4 | Accept the peer's maximum bitmap/window and gate sending. | **V** — U-NEG directional and matrix tests; U-MPR maximum/shrink; WRITE maximum 32,640-entry window crossing wrap. |
| M34 | §7.4.5 | Avoid spurious fast retransmission and repeat fast retries of one packet. | **V** — PEER `--fast-loss-only`: sparse later delivery, delayed NACK retry, actual loss, and timeout after one fast retry. |
| M35, M36 | §7.4.6 | Answer reliability probes with PR SACK and ordinary reliability state. | **V** — PCAP probe SACK CACK/bitmap/counters; PEER correlated probe replies; APP SRv6 probe entropy. |
| M37, M38 | §7.4.6 | Ignore reserved probe BTH.PSN; echo/correlate PETH identifier in SACK offset. | **V** — PCAP nonzero reserved PSN and `0x91bc` token; PEER `--probe-correlation-only` wrong token/entropy/stale reply and reserved response PSN. |
| M39 | §7.4.7 | Echo endpoint operation's opaque request identifier. | **V** — APP independent endpoint wire checks and reversed replies; PEER lower-valued replacement Port Status identifier. |
| M40, M41 | §7.5 | Track received packets and use modulo-24-bit PSN arithmetic. | **V** — U-QP serial arithmetic/window tests; PCAP boundary and OOO replays across wrap; APP boundary completions. |
| M42, M43 | §7.5 | Maintain contiguous CACK and initialize it to rq_psn−1. | **V** — U-QP receive/rotation tests; PCAP probe before valid data and after rejected prefixes. |
| M44, M45, M46 | §7.5 | Track highest untrimmed and lowest unsacked PSNs; initialize highest to rq_psn−1. | **V** — U-QP `test_sack_rotation_and_probe`; PCAP trim then probe proves trimmed frames do not advance state. M45 repeats the highest-PSN obligation. |
| M47, M48 | §7.5.1 | Limit receiver penalty to 0–127; allow per-QP disable. | **V** — U-CC `test_receiver_flow_control`; APP `rx_flow_control` two-QP isolation/disable; PEER penalty/restore window admission. |
| M49, M50 | §7.5.2 | Request ACK before idle; emit delayed SACK when a reordered AR gap closes. | **V** — APP QuickAdapt single WRITE_ONLY_IMM AR and PEER flow-control/credit-exhaustion AR assertions; PCAP `check_retransmitted_ack_request` directly checks delayed gap-closing SACK; U-QP AR state. |
| M51 | §7.5.2 | Populate SACK MPR according to negotiated tracking range. | **V** — U-MPR; APP RTR/RTS `3,1,3` and `255,1,255` wire/query checks; static mode sends zero per §7.5.4. |
| M52, M53 | §7.5.2.1 | Reflect request entropy; zero the absent IPv6 component for IPv4. | **V** — PCAP `check_responses`, PEER independent feedback, APP mapped EV and SRv6 cases. |
| M54, M55 | §7.5.2.2 | Emit 64-bit bitmap with signed offset and returned OOO count. | **V** — U-QP rotation and maximum-window tests; U-WIRE SETH/CC_STATE fixtures; PCAP independently reads both fields. |
| M56 | §7.5.2.3 | SACK entropy comes from the triggering request. | **V** — PCAP trigger-specific entropy assertions; APP mapped SRv6 probes; PEER cross-rail SACK validation. |
| M57, M58 | §7.5.3 | Recognize trims; perform no payload placement or semantic work for them. | **V** — PCAP shortened and intact marked WRITE plus state-checking probe; APP last-hop case preserves target bytes/CQ until RTX. |
| M59 | §7.5.3 | Generate NACKs for the listed mandatory receiver events. | **V/Q** — resource events are tested, but PSNE has no assigned Table 7-17 encoding. GERR uses lowercase “should” and is not a separate capitalized mandatory obligation under §4.4. The bitmap-error trigger/encoding requires clarification. |
| M60 | §7.5.3 | Select NO_BITMAP or NO_PKT_BUFFER according to the exhausted resource. | **V** — WRITE mandatory NACK matrix directly asserts distinct sent/received reason masks and recovery across four trim capability directions. This clause does not depend on resolving the PSNE terminology. |
| M61 | §7.5.3 | Mandatory NACK reception remains supported without optional trim generation. | **V** — WRITE tests resource/error reasons across all four trim configurations; PEER independently injects PSN_OOR_WINDOW for retry/exhaustion and packet-buffer/both trim NACKs with trim disabled across four envelopes. Forward-window traffic now tests silent timeout, not NACK generation. |
| M62, M63, M64 | §7.5.3 | When enabled, generate TRIM for configured regular-trim DSCP. | **V** — PCAP trim reason/length assertions; WRITE asymmetric capability cases prove local responder enable controls generation, independently of requester offer. |
| M65 | §7.5.3 | Configure last-hop trim DSCP when that extension is supported. | **V** — APP `--last-hop-trim-only`, `mrc_dpdk_set_trim_last_hop_dscp` validation; PCAP distinct regular/last-hop DSCPs. |
| M66, M67 | §7.5.3 | Receive both trim NACK forms and retransmit under retry rules. | **V** — PEER both reasons in four wire modes; APP last-hop alternate rail; WRITE typed retry exhaustion. |
| M68, M69 | §7.5.4 | Static mode sends zero MPR and ignores received MPR. | **V** — APP `static_mpr` wire values; U-MPR static fields; MPR-PEER injects nonzero static-mode MPR and verifies eligible alternate-rail retry with zero window updates. |
| M70 | §7.5.4 | Dynamic mode sends nonzero MPR in every SACK. | **V** — APP Dynamic MPR wire witness including reliability probes; U-MPR advertised field. |
| M71, M72 | §7.5.4 | Parse MPR feedback; change resources only on advancing CACK and changed MPR. | **V** — U-MPR stale/equal/wrap/zero/oversized matrix; APP live lower/restore values; MPR-PEER independently injects equal/stale CACK and zero/oversized updates, asserting zero updates and retry admission. |
| M73, M74 | §7.5.4 | Enforce CACK+MPR on new packets and retries after shrink. | **V** — U-MPR and MPR-PEER: 131 packets across wrap, shrink 256→128 at CACK index 0, NACK already-sent index 130, observe no retry for 100 ms, then advance CACK without window growth and assert alternate-rail retry and exact completion. Removing the retry MPR guard makes this test fail. |
| M75 | §7.5.5.1 | Support at least one data traffic class. | **V** — APP `--service-level-dscp-only`: SL 7 data 26/retry 34/control 48; PEER DSCP checks in four wire modes. |
| M76, M77 | §7.5.5.2 | Configurable UDP destination defaults to 4971; transmit zero UDP checksum. | **V** — U-WIRE explicit 4791 and zero-checksum vectors; WRITE `--udp-ports-only` transfers on 1/4791/65535 in four envelopes and rejects mismatched ports; WRITE/APP default 4971 traffic; PCAP independently validates UDP. Table 7-9's 4791 discrepancy remains documented. |
| M78 | §7.5.5.6 | Trim NACK preserves original declared UDP length. | **V** — U-WIRE NETH/trim vectors; PCAP compares original length to NACK; APP last-hop 64-byte nominal length. |
| M79, M80 | §7.5.5.8 | Advertised Service Time supports both roles and directional peer attributes. | **V** — U-NEG 729 offers; RTT exact numeric compensation in all four directional configurations and four envelopes; SVC-API observes actual public-QP-to-endpoint peer capability wiring in live bidirectional traffic. Local reporting remains independent of peer support. |
| M81 | §7.5.5.8 | Requester-generated compensated timestamps use specified resolution. | **N/A** — requester stores local send times and does not originate TSETH. Timestamped input and 128 ns reporting are covered by M83–M85. |
| M82 | §7.5.5.8 | Stored-timestamp requester interprets SACK/NACK Service Time. | **V** — RTT independently encoded SACK/NACK/probe frames pass through production decode/control with exact clocks and known service durations; asserts numeric RTT including zero/max/oversized durations and retry ambiguity. SVC-API verifies the public configuration path. This establishes software arithmetic and wiring, not hardware clock accuracy. |
| M83, M84, M85 | §7.5.5.8 | Adjust TSETH in matching resolution or report standalone Service Time. | **V** — PCAP `check_service_time` checks SACK/NACK/probe adjustment, 16-bit wrap, opaque preservation and standalone positive durations; U-CC resolution/saturation. |
| M86 | §8.2 | Retain congestion/QP state until idle or ERROR. | **V** — reset/destroy now enter provider ERROR before releasing the route; queued work and MR leases remain until reset discards them. Explicit ERROR transitions and flushes before release. ABI release-order witnesses and live reset_pending/destroy_pending/error_pending/error_pending_overflow tests assert ERROR at actual release with held semantic ACKs; reset/destroy preserve no-CQE cancellation behavior. Direct endpoint destructors also enter ERROR before disposing pending state; failed creation preserves caller ownership. The release-order test failed before the fix. |
| M87, M88 | §8.3.1 | Maintain 32-bit in-flight bytes and modulo-24-bit acknowledged-byte count. | **V** — U-CC nominal size, receive accounting, stale feedback, 256-byte rounding and wrap; PEER window admission/restore and invalid SACK regression. |
| M89 | §8.4 | Stored-timestamp requester does not use reflected timestamp as its send clock. | **V (contextual interpretation)** — stored local send time is the sole send clock. Under §7.5.5.8, negotiated tx_timestamp is interpreted as a Service Time duration, not as a reflected send timestamp. RTT exact-clock and SVC-API public wiring tests cover directional capability support. This reconciles the two sections contextually, not by an upstream erratum. |
| M90 | §8.7 | Charge OnSend only after irrevocable send selection. | **V** — EP application and APP `rail_backpressure` reject queue submissions and verify same-poll alternate selection; send metadata/byte accounting commits only after successful enqueue. Physical wire transmission after NIC enqueue remains outside software observation. |
| M91, M93, M94 | §9.3.1 | Rotate configured EVs, select active EVs, detect bad paths. | **V** — U-EV transitions; APP counted/mapped EVs; WRITE live deny/restore and cross-process ASSUMED_BAD/GOOD; APP blackout recovery and explicit SRv6 re-enable regression. |
| M92 | §9.3.1 | Configure network switches for ECN. | **V (software deployment)** — FABRIC configures and reads back CE policies on both Linux routers, counts actual marked packets, independently counts reflected SKIP SACKs for both EVs, and verifies requester response and exact completion. The decapsulation rail explicitly propagates CE to ECT inner traffic; a separate four-codepoint check preserves Not-ECT. This proves the tested software deployment, not physical-switch/WRED configuration. |
| M95 | §9.3.2 | Correlate feedback to transmit plane, regardless of ingress plane. | **V** — PEER `--late-nack-only`, `--sack-trigger-only` and probe replies retain original entropy on opposite rail; APP EV-to-port mapping. |
| M96 | §9.3.5 | Encode optional SRH using RFC 9800 compressed segment lists. | **V** — CSID independently compares production header bytes to RFC 9800 NEXT-CSID encoding, then observes Linux kernel shifts/SRH promotions and terminal decoding. Thirty vectors cover omitted/debug/full/reduced SRH, changed locator blocks, inner IPv4/IPv6, and 32/16, 48/16, 32/32 locator/function widths. Vendor/hardware interoperability remains additional evidence, not an intrinsic prerequisite for this encoder clause. |
| M97 | §9.3.5.1 | Accept preserved and decapsulated SRv6 stacks. | **V** — U-WIRE SRv6 dispatch/bad-stack tests; APP `srv6`, `srv6_transit`, controller profiles and independent decapsulation; FABRIC historical End/End.DX6. |
| M98 | §10.1.2.2 | Exchange directional QP attributes out of band. | **V** — U-NEG strict records, directional state and 729 support/offer cases; WRITE TCP OOB asymmetric PMTU/capabilities; APP exchanges QPN/PSN/GID/MR through an application-owned channel. |
| A01 | `mrc_ctl.h`, privilege preamble | Controller processes require CAP_NET_ADMIN. | **V** — CTL positive namespace and `--expect-no-cap` rejection; U-STORE separate ownership/lease checks. |
| A02 | `mrc_ctl.h`, explicit profile lifecycle | Populate explicit array entries before bringing the profile online. | **V** — CTL `test_profile_lifecycle`, explicit SRv6 values; U-STORE typed edits and invalid activation. |

### Advertised capabilities and resource limits

The capability masks are the values returned by `mrc_query_device[_ex]()`
and `mrc_ctl_query_device[_ex]()`, not a list inferred from supported packet
formats. A numeric limit being queryable is not evidence of an end-to-end
transfer at that limit.

| Advertised surface | Direct evidence | Status / residual requirement |
| --- | --- | --- |
| Application/controller API 1.0.1, minimum 1.0.0; protocol version 1 | ABI resource/version checks; CTL discovery; legacy/current EV event layouts | **V** for these pinned headers/layouts. Arbitrary future header compatibility is not claimed. |
| `MRC_DEVICE_CAP_TRIM_NACK` | APP last-hop peer-unsupported case; WRITE 00/01/10/11 trim offers; PEER/PCAP both reasons | **V** software generation and reception; hardware-generated trims remain H. |
| `MRC_DEVICE_CAP_DYNAMIC_MPR` | U-NEG matrix; WRITE four offer combinations; APP live RTR/RTS/static cases | **V** — live changes plus independent withheld/released retry and invalid-update cases in MPR-PEER. |
| `MRC_DEVICE_CAP_SVC_TIME` | U-NEG matrix; WRITE four offers with trim/retry; APP asymmetric QPs; PCAP adjustment; U-CC SACK/NACK math | **V** — responder wire values, exact numeric requester compensation (RTT), and public capability wiring (SVC-API). Hardware clock behavior is outside these assertions. |
| MPR maximum 255 units, resolution 1 (32,640 packets) | U-QP `test_maximum_window`; WRITE `maximum_negotiated_mpr`; APP RTS 255→1→255 | **V** at maximum window and wrap. |
| Local/peer WRITEIMM limits 1024 | ABI range validation/query; U-WR resource exhaustion; WRITE/APP small-credit saturation | **V** — APP `--capacity-only` holds semantic ACKs while passing SACKs, observes exactly 1024 completions and a blocked 1025th operation, then releases credit and checks all 1025 completions. Legal WIMM1024/MPR1 attachment is separately asserted. |
| 1024 QP hints | ABI `test_qp_hints` capacity/lifetime and sharing checks | **V** hint resource contract; hints are advisory, with no advertised scheduling/performance guarantee. |
| 36 format profiles (4 built-in + 32 custom); 32 EV profiles; 1024 total EVs; 32 EVs/profile; alignment 1; max width 256; batch maximum 32 | CTL discovery, custom format lifecycle, generated profile count 8→32→4, field bounds and batch errors; U-STORE persistence/leases | **V** — CTL `test_profile_capacity` fills all 32 custom format, EV, and CC slots, brings profiles ONLINE with 32 EVs each (1024 total), checks ENOSPC, reopen persistence, and freed-slot reuse. APP separately verifies 32 live rails. |
| UDP, STEV and SRv6 formats; AUTO, explicit and generated modes | APP mapped AUTO/explicit/generated UDP, custom generated STEV, explicit/generated SRv6; CTL format-mode compatibility and rejection | **V** for supported combinations. AUTO SRv6 is rejected; mode masks do not advertise every format×mode combination. |
| `MRC_CTL_OPT_CAP_EV_EVENT` | ABI current/legacy CQ layout, notification, overflow and reader ownership; WRITE live BAD/GOOD and BAD-only subscriptions | **V** software events including cross-process endpoint-to-controller delivery. |
| `MRC_CTL_OPT_CAP_EP_OP_EV_PROBE` | APP endpoint ops UDP/STEV/SRv6/shared/counted: reorder, timeout, late reply, DSCP, RTT and port 8 | **V** batch operation and correlation; physical path RTT accuracy is not claimed. |
| 32 CC profiles; `uet-1.0-nscc`; disabled CC and Quick Adapt settings | CTL CC/QA lifecycle; U-CC formulas; APP one-packet CC profile and delay-driven Quick Adapt | **V** — CTL verifies 32 ONLINE profiles with overflow/persistence/reuse. APP `public_32_cc_profiles_under_traffic` keeps 32 QPs active together, observes every QPN before feedback, and asserts exactly one data packet per configured 1112-byte window. Link-rate tuning remains deployment-specific. |
| Queried physical-port mask and administrative state | CTL compares to kernel flags; PORT isolated veth mutation; historical physical transition below | **P/H** — successful physical port mutation through the public controller API while QPs are active remains unverified. |
| Not advertised: online EV structure/CC profile modification, explicit-range EV option, precise EV event drop counts | CTL discovery mask assertions and offline/lease checks | **N/A** as advertised optional features. Administrative EV GOOD/DENIED changes remain supported online; they are not structural edits. |
| DPDK extensions: 1–32 rails, SL/DSCP, receiver flow control, MR binding, preserved SRv6 | APP and EP extension checks, counted 8-rail traffic, PEER DSCP and flow tests | **V** — public and standalone transfers exercise all 32 rails; per-rail callbacks/counters verify dispatch, with payload and completion assertions. |

### Public API entry-point coverage

Every exported symbol appears below. **Live** means the public call participates
in a completed memif application/controller scenario. **ABI** means a direct
resource/validation unit test; it must not be read as end-to-end wire coverage.

| Exported functions | Evidence and level | Remaining coverage |
| --- | --- | --- |
| `mrc_query_version`, `mrc_query_device`, `mrc_query_device_ex` | ABI `test_resources_and_completions`, CTL discovery; Live application discovery | Capability maxima saturation as listed above. |
| `mrc_create_context`, `mrc_destroy_context` | ABI ownership/child lifetime; Live every APP scenario | Active cancellation interpretation M86. |
| `mrc_create_qp`, `mrc_destroy_qp`, `mrc_get_qpn`, `mrc_modify_qp`, `mrc_query_qp` | ABI `test_public_qp`; Live simplex/duplex, reset reuse, dynamic MPR and error cases | Arbitrary application-managed OOB mismatch matrix; active teardown M86. |
| `mrc_post_send`, `mrc_post_recv` | ABI invalid linked-list suffixes; Live registered/inline/scatter-gather, empty WRITEIMM, boundaries, RQ exhaustion, WR order | Maximum 1024 in-flight WRITEIMM and sustained multi-thread posting not established. |
| `mrc_create_cq`, `mrc_destroy_cq`, `mrc_poll_cq` | ABI lifecycle/overflow; Live all APP sends/receives and async CQ error | High-contention concurrent CQ polling not established. |
| `mrc_create_comp_channel`, `mrc_get_comp_channel_fd`, `mrc_destroy_comp_channel`, `mrc_req_notify_cq`, `mrc_get_cq_event`, `mrc_ack_cq_events` | ABI notification/ownership; Live simplex solicited WRITEIMM and event-backed completion | No physical interrupt moderation/performance claim. |
| `mrc_get_async_event`, `mrc_ack_async_event`, `mrc_dpdk_get_async_fd` | ABI native record injection/FD ownership; Live APP CQ_ERR, QP_FATAL, QP_REQ_ERR, QP_ACCESS_ERR | Actual physical port/device event delivery remains H. |
| `mrc_create_qp_hint`, `mrc_destroy_qp_hint`, `mrc_query_qp_hint`, `mrc_modify_qp_hint` | ABI `test_qp_hints`: capacity, masked attributes, sharing, foreign context, reassignment and release | No wire-visible performance contract; advisory metadata only. |
| `mrc_dpdk_bind_mr`, `mrc_dpdk_unbind_mr` | ABI keys/PD/lease checks; Live all registered APP transfers and boundary guards | Concurrent deregistration is unit/provider evidence, not a remote-device stress test. |
| `mrc_dpdk_attach_qp`, `mrc_dpdk_attach_qp_rails`, `mrc_dpdk_attach_srv6_qp`, `mrc_dpdk_progress_qp` | Live simplex/duplex, counted/mapped rails, backpressure and SRv6; ABI validation | 32 simultaneous rails and all configured rail bits are verified in the public live capacity test; physical multiport interoperability remains separate. |
| `mrc_dpdk_set_sl_dscp`, `mrc_dpdk_set_rx_flow_control`, `mrc_dpdk_set_trim_last_hop_dscp` | Live APP service-level wire checks, two-QP penalty/restore isolation and last-hop trim | Physical QoS queue behavior remains H. |
| `mrc_dpdk_set_profile_store`, `mrc_dpdk_set_ev_store` | ABI compatibility alias; Live application/controller sharing and profile leases | Distributed/multi-host controller storage is not supported by this local backend. |
| `mrc_ctl_query_version`, `mrc_ctl_query_device`, `mrc_ctl_query_device_ex` | CTL discovery and ABI legacy/current EV event layout selection | Pinned header/version scope only. |
| `mrc_ctl_modify_ev_fmt_profile`, `mrc_ctl_query_ev_fmt_profile` | CTL custom UDP/STEV/SRv6 widths, packing, state transitions; Live custom STEV profile traffic | Custom SRv6 format programming is CTL evidence; custom-format hardware uSID forwarding remains H. |
| `mrc_ctl_modify_ev_profile`, `mrc_ctl_query_ev_profile` | CTL explicit/AUTO/generated lifecycle and per-entry errors; Live denied→GOOD, mapped and generated profiles | Global capacity exhaustion/persistence/reuse now pass CTL; every invalid profile×format combination is not exhaustive. |
| `mrc_ctl_modify_cc_profile`, `mrc_ctl_query_cc_profile` | CTL NSCC/disabled/QA persistence and QP leases; Live CC window and Quick Adapt | Profile-capacity saturation passes CTL; link-rate tuning remains external. |
| `mrc_ctl_create_ev_event_cq`, `mrc_ctl_poll_ev_event` | ABI layout/overflow; Live WRITE separate controller BAD/GOOD and BAD-only subscriptions | Precise dropped-event count is not advertised. |
| `mrc_dpdk_attach_endpoint`, `mrc_dpdk_attach_endpoint_rails`, `mrc_dpdk_detach_endpoint`, `mrc_dpdk_progress_endpoint`, `mrc_ctl_ep_batch_send_wait` | Live APP UDP/STEV/SRv6/shared/counted endpoint operations, partial timeout and stale reply | APP verifies WRITEIMM traffic on all 32 rails; physical network RTT calibration remains external. |
| `mrc_ctl_query_port`, `mrc_ctl_modify_port` | CTL query vs kernel and rejected invalid operations; PORT backend veth setter | Successful mutation via the public API of an active physical RoCE port remains H; a backend test is not public API evidence. |

### Acceptance gates for the all-requirements goal

Current disposition: **95 requirements have direct software evidence within
the stated implementation/deployment scope (M31 and M89 under the documented
contextual interpretations); one conditional requirement (M81) is not applicable;
two (M30 and M59) remain qualified.** This is not a claim that all 98 are
satisfied. Fixing M15 removes the previously exercised PSN_OOR_WINDOW responder
trigger, so its unresolved mandatory-generation obligation is tracked under M30.

The expanded tests are evidence, not a blanket conformance certificate. A row
is complete only when the tested behavior satisfies its normative obligation
under the stated deployment and capability conditions. Full conformance remains
unproven while any applicable row lacks that evidence, regardless of goal status.

| Gate | Evidence needed to close it | Current disposition |
| --- | --- | --- |
| M15 / M30 window handling | Enforce explicit silent-drop rule; independently assess mandatory NACK support | M15 fixed and verified. M30 retains the unresolved responder-generation trigger; requester PSN_OOR_WINDOW handling is independently verified. |
| M31 timer scope | Verify feedback reset and mandatory timeout backoff under contextual reading | Both paths measured across four envelopes; accepted contextual interpretation documented above. |
| M59 bitmap-processing error | A defined wire encoding/trigger for PSNE or authoritative mapping to an assigned NACK, then injected-error evidence | No assigned PSNE value in Table 7-17; do not invent a reserved wire code. M60 resource reasons are independently verified. |
| M86 state lifetime | Observe that active reliability/congestion state remains until idle or a genuine ERROR transition, including reset/destroy and explicit ERROR | Fixed: genuine ERROR precedes route disposal; ABI and live held-ACK witnesses pass. No cancellation exemption is assumed. |
| M89 stored-clock/Service Time wording | Verify local send clock and negotiated duration as distinct inputs | Accepted contextual reading documented above; exact-clock and public capability tests verify the distinction. |
| M92 deployment ECN | Identify the packet-switching nodes in the tested topology, inspect their ECN configuration, and observe marked traffic reaching feedback | Both-router CE configuration, marked-packet counts, per-EV feedback, and requester response pass in the isolated software deployment. Physical hardware is not demanded by the clause; other deployments require their own configuration evidence. |
| M96 compressed SRv6 encoding | Independent RFC 9800 header oracle and forwarding implementation exercise supported locator/container/SRH combinations | Thirty independent RFC/kernel cases pass. Hardware forwarding remains additional interoperability evidence, not a missing intrinsic encoder check. |
| Remaining verified rows | Audit assertions against complete requirement language, retain negative/boundary cases, and rerun affected tests after every implementation change | Existing 98-row mapping retained; no “all satisfied” claim while qualified rows remain. |

The [published OCP specification](https://www.opencompute.org/documents/ocp-mrc-1-0-pdf/)
and [official repository issues](https://github.com/opencomputeproject/OCP-Multipath-Reliable-Connection/issues)
were checked for clarification. No applicable public erratum was found for
M15/M31/M59/M89; absence from these sources is not proof that no private guidance
exists. The subsequent contextual audit below resolves M31/M89 for this implementation
under explicit interpretations; M30 and M59 still need external clarification.
The user reported no additional authoritative guidance. The official repository
was rechecked at current `main` commit
`662a1509e7e49315ae912099ceec3874a04b1442`: its
[`test/mrc_test.c`](https://github.com/opencomputeproject/OCP-Multipath-Reliable-Connection/blob/662a1509e7e49315ae912099ceec3874a04b1442/test/mrc_test.c)
only opens a verbs device and allocates a PD; it performs no MRC operations.
The test directory contains that placeholder and its Makefile. The available
issue/PR history concerns API query versioning and README updates, not the
four transport interpretations above. This repository supplies the API
contract, not an independent transport implementation or conformance oracle.
Both current `mrc.h` and `mrc_ctl.h` hashes match the pinned build headers
listed above; the unresolved transport questions are not caused by stale API
headers.


Read-only lab inventory on 2026-11-21 found four ConnectX-7 interfaces on
Spark-04 and Spark-02. Spark-03 currently enumerates only its management
Ethernet and Wi-Fi adapters, not the four data NICs required by the historical
three-node cycle. No existing MRC services were active in the inspected local
service listing. The earlier three-node hardware results remain historical;
this inventory does not authorize changing current links or treating old
snapshots as evidence for current source. No independently accessible packet
switch configuration or separate MRC endpoint implementation was established.

### Contextual interpretation and silent-drop correction (2026-11-22)

M15 now follows §6.3.1 rule 6 directly: intact, trimmed, and unsupported-opcode
requests outside the tracking window receive no response. In-window unsupported
opcodes still produce Invalid Request. Requester PSN_OOR_WINDOW reception and
retry/exhaustion remain supported and independently injected in all four wire
modes. This resolves M15 but does not resolve M30's conflicting responder-generation
obligation; no replacement trigger or reserved PSNE encoding is invented.

M15 validation: all 10 portable programs pass, plus nine independent ingress
vectors in `make check-endpoint-window` that snapshot responder state, tracking
slots, and the completion-drain clock. The timer test fails before its fix and
passes afterward. All 34 PCAP scenarios and 20 live resource/error/window cases
pass on the final build. Service Time NACK vectors now use defined in-window
trim events instead of the removed forward-window trigger. Logs:
`/tmp/mrc-window-all-units.log`, `/tmp/mrc-window-timer-before.log`,
`/tmp/mrc-window-timer-after.log`, `/tmp/mrc-m15-pcap-final.log`, and
`/tmp/mrc-m15-live-final.log`. The earlier binary also fails the new silent-drop
assertion (`/tmp/mrc-m15-before.log`).

The final source manifest is `/tmp/mrc-conformance-20261122-source-sha256.txt`,
SHA-256 `ef9330561407b7f99ebc332d504425ece02ee89eb47d0e6a3dd7fde012f047f6`.
It includes source/test C, headers, Python, Makefile and .gitignore; the previous
checkpoint manifest below is retained as historical evidence. The shared library
and benchmark were rebuilt; unrelated historical suites were not rerun wholesale.

M31 uses §7.4.4's feedback-retransmission context: NACK and fast-loss retries
reset the timer to base, while timeout retries follow §7.4.2's explicit exponential
schedule. Twelve focused live cases verify timeout backoff, NACK reset, fast-loss
reset, and exhaustion across four envelopes. Logs: `/tmp/mrc-m31-contextual-audit.log`
and `/tmp/mrc-psn-oor-independent.log` (eight additional injected window-NACK cases).
The default PEER suite now contains 50 cases; this update ran the focused cases,
not a fresh full 50-case run.

M89 uses local stored send time as the sole send-clock input; negotiated
Service Time is a duration under §7.5.5.8, never a reflected send timestamp under
§8.4. The 864-case RTT suite adds clock translations across timestamp-wrap and
one-second boundaries, unchanged stored-clock assertions, and valid credit/reliability
processing even when duration samples are unusable. Public Service Time wiring also
passes. Mutants that truncate local clocks or discard credits for invalid durations
fail. Logs: `/tmp/mrc-m89-current.log`, `/tmp/mrc-m89-mutations.log`.
These are explicitly documented contextual interpretations, not published errata.
M59's undefined PSNE trigger/encoding remains unresolved.

### Prior conformance-closure verification (2026-11-21)

At this earlier checkpoint, M15/M31/M59/M89 were treated as requiring
normative clarification; the 2026-11-22 audit supersedes that disposition. Their chosen implementation behavior has tests, but
that is not a substitute for satisfying the unresolved wording. The user
confirmed no additional authoritative guidance is available. No upstream
messages, issues, commits, or deployment changes were published.

Verified changes in this iteration:

- M86: public and direct endpoint cancellation reaches ERROR before pending
  reliability/congestion state is discarded. Failed creation preserves the
  caller's QP/work; explicit ERROR with flush overflow remains ERROR and
  detaches the route. Before-fix witnesses failed; current witnesses pass.
- Thirty-two distinct CC profiles carry simultaneous traffic with independent
  one-packet admission bounds, rather than merely occupying configuration slots.
- M96: independent RFC 9800 encoding checks and actual Linux NEXT-CSID hops
  verify 30 vectors, 92 shifts and 18 SRH promotions, preserving payload/iCRC.
- M92: both software forwarding nodes have inspected CE policies and observed
  per-EV feedback. An explicit ECT-only policy preserves CE across End.DX6;
  four separate synthetic codepoint checks cannot inflate transfer counters.
- Fixture lifecycle: standalone memif peers use two-phase shutdown so the
  responder stops polling before the client disconnects. Client-first close
  ordering alone was insufficient: the responder intentionally drains for
  eight RTOs plus 250 ms. A prior receiver crash had no preserved stack, so
  its exact PMD fault site is not claimed as proven. The final 89-case suite
  passes with explicit ordering and bounded missing-peer diagnostics.
- Independent PEER/MPR relay RX polling now stops before terminal feedback
  can make the client disconnect; quiescence is acknowledged only after
  that feedback is enqueued. Ports remain until requester exit and explicit
  stop. Quiescence
  is acknowledged and bounded; extra frames are rejected. Both-rail readiness
  is observed before alternate-rail NACK assertions. Initial reruns exposed
  missing readiness and queued feedback after terminal quiescence; those
  fixture errors were corrected without relaxing transport assertions.
  Earlier unexplained ConnectionRefused attribution remains unproven.

| Current run | Result | Log |
| --- | --- | --- |
| Portable programs | 10 pass, including retained-state cancellation, full PSN reuse, 6656 timeout configurations | `/tmp/mrc-retention-units-final.log` |
| Public ABI/controller | Both pass | `/tmp/mrc-retention-abi-final.log`, `/tmp/mrc-retention-controller-final.log` |
| Public application/controller | 48 scenarios pass | `/tmp/mrc-retention-public-final.log` |
| Public teardown observation wrapper | 4 scenarios pass, including flush overflow | `/tmp/mrc-retention-witness-final.log` |
| Direct endpoint application | 2 scenarios pass, including pending destruction and failed-create ownership | `/tmp/mrc-direct-cancel-after.log` |
| Standalone | 89 scenarios pass | `/tmp/mrc-conformance-write-final.log` |
| Independent requester | 42 scenarios pass | `/tmp/mrc-peer-quiesced-ready-v2.log` |
| Independent MPR peer | 24 scenarios pass | `/tmp/mrc-mpr-quiesced-final.log` |
| Relay protocol | 3 checks pass: terminal enqueue, post-terminal rejection, bounded stop timeout | `/tmp/mrc-relay-protocol-final-v2.log` |
| Independent packet replay/capture | 34 scenarios pass | `/tmp/mrc-conformance-pcap-final.log` |
| Numeric endpoint RTT | 672 cases pass against rebuilt endpoint source | `/tmp/mrc-conformance-rtt-final.log` |
| RFC 9800/Linux NEXT-CSID | 30 cases pass | `/tmp/mrc-conformance-csid-verified.log` |
| Current software ECN fabric | Both-router policy, feedback and exact delivery pass | `/tmp/mrc-conformance-ecn-final.log` |

Build/run entry points for added checks are `make check-srv6-csid`,
`make MRC_INCLUDE_DIR=/path/to/headers check-public-teardown`, and
`make check-relay-protocol` (the last needs the pcap PMD runtime).
The final ECN run used kernel 6.17.0-1021-nvidia and AF_PACKET 23.11.7,
with the driver extracted under `/tmp/mrc-af-packet-runtime` and bound to
`/mnt` only inside a private `unshare -Urnm` namespace. This avoids DPDK's
world-writable-plugin-path rejection without changing host mounts or ports.
The baseline fabric without ECN also passed before the final lifetime rebuild;
only the current ECN run is claimed against the final endpoint binary.

This prior checkpoint source manifest uses the same sorted path/hash format described
in the earlier ledger, includes all new C/Python fixtures, and is saved at
`/tmp/mrc-conformance-source-sha256.txt`. Its SHA-256 is
`6a2d1bc20911ffc7a9f049e0d19f915a2535b0e5c0ec98db0e08fa301f340645`.

| Current artifact | SHA-256 |
| --- | --- |
| `mrc_write` | `5f0ff99780575dc9cc7c8a86225c390ed427deb6e4bd6a45e776376187ab4205` |
| `tests/mrc_write_ready` | `875c6c6b80eaca29f936a46e377092338b2fd424c9856d201a5d2db8ea34f153` |
| `libmrc-dpdk.so` | `ebd7c4f3402a0ee91ce013e29a8906e54906e0cb7a1db0cc55f2fb92c7566648` |
| `tests/mrc_abi_application` | `845df8ec626b89af9c6bb490d887d2203c1f6be05e2cf8ab956046594f662af8` |
| `tests/test_mrc_abi` | `092062ccc1099db2258db2ad88ea61ff5c751fbc957c8624d71277ada36abc7c` |
| `tests/test_mrc_ctl_abi` | `972c08413aa75924b0692229bbbcf73af4cdf4e933d76ab176dd2e7290560f50` |
| `tests/mrc_endpoint_application` | `f53b4c854feb28fbbe94cedcbd9a24f9b141862672cd27ef6b5f7b4ba0c999c4` |
| `tests/mrc_public_teardown` | `9237858b6f748f21aa0158218a7d8fe69d2779aab716b584268969e170194128` |
| `tests/mrc_memif_relay` | `4c827f577e0c6c8bca31587ae7c0b2be8a95967e519f628e13b4fb8a686fa8a6` |
| `tests/mrc_srv6_encode` | `dd18a244fcea44aca2238fe603c887cc88046288eb78a488292c5c80af8a4a8e` |
| `tests/mrc_endpoint_rtt` | `c6c5ecc1613c28377034bdeb5eafb01065073e9afef1f246eec18be14a2abd5c` |

### Initial audit verification ledger

This ledger predates the expanded-coverage follow-up below. The initial source/test diff relative to the base above has SHA-256
`e7ca64f52a8c5f2fbbf8b4ce47c470c3b0ad940480685dd763708afc48fdb20d`
(`git diff -- mrc-dpdk/src mrc-dpdk/tests`, documentation excluded).
The build used aarch64, DPDK 23.11.7 and libibverbs pkg-config version
1.14.50.0. The pcap replay used the matching pcap-enabled DPDK runtime.

| Run | Result |
| --- | --- |
| `make check` | All 10 portable test programs passed, including the 729-case negotiation matrix. |
| ABI | Passed, including namespace-only EV event CQ coverage and native event descriptor checks. |
| CTL without CAP_NET_ADMIN | Expected rejection passed. |
| CTL in user namespace | Passed all lifecycle/format/CC/QA cases; new unpopulated-profile activation rejection preserves OFFLINE state and values. |
| APP with controller | 41 scenarios passed, including three new public boundary cases, live profiles, mapped EVs, explicit/generated SRv6, endpoint operations, CC and Quick Adapt. |
| EP | Both application-owned endpoint scenarios passed. |
| WRITE with event controller | 60 scenarios passed, including 12 asymmetric cases and both cross-process EV event subscriptions. |
| PEER | 30 independent requester scenarios passed, including cross-rail SACK/NACK and invalid-trigger state preservation. |
| PCAP | 34 independent replay/capture scenarios passed; the four new boundary cases rejected 592 prefixes and completed 48 WRITEIMMs. |
| PORT | Isolated veth administrative changes passed across processes; physical ports were not modified. |

Built artifact SHA-256 values:

| Artifact | SHA-256 |
| --- | --- |
| `mrc_write` | `1b4cb05a3aa3e03ae26e4ce7ccc14cb596f792764f55966ccb8303822d274e20` |
| `libmrc-dpdk.so` | `d0e3dee8aaab968b67273e470f2e1a220c831b3adecb8ec9a22cdb68d3a358e5` |
| `tests/mrc_abi_application` | `edd77464510fb70a92fb8c7397c5f2665816fde77a11a5250d5daab0520b9704` |
| `tests/test_mrc_ctl_abi` | `a25f41b5c805da1ac1efcc09ed6559d0472086522f706eb8f3cbfe05a276193b` |

### Verified changes and outstanding work

Verified fixes carried into this audit:

- Re-enabled EVs schedule a probe immediately, before short transfers can
  finish ahead of the periodic probe timer. The explicit SRv6 controller
  case passed ten repeated runs and the full public/controller suite.
- A data SACK whose trigger does not identify a retained send is rejected
  before cumulative/bitmap delivery, MPR, EV or congestion feedback changes.
  Unit state snapshots and independent peer window/EV checks failed before
  the fix and passed afterward in all four wire modes. Valid negative
  offsets, opaque probe identifiers, and feedback on another rail remain
  covered.
- The earlier Transport NAK completed-prefix fix and jumbo RX-buffer FCS
  reservation remain part of the base revision. Their historical and current
  software evidence is described below; a fixed hardware jumbo run is still
  outstanding.

New audit coverage adds 729 support/offer combinations, 12 live asymmetric
feature cases, 592 independently generated rejected packet prefixes across
four envelopes, and public linked batches of ten WRITEIMMs at each of three
PMTUs. Boundary cases include zero length, all four padding residues,
PMTU−1/PMTU/PMTU+1, two-PMTU lengths, a final byte at the MR end, and PSN wrap.
No additional production defect was demonstrated by these new cases.

Outstanding requirements are deliberately retained rather than marked passed:

1. **Unresolved normative requirements:** mandatory responder PSN_OOR_WINDOW
   generation versus explicit outside-window silence (M30), and undefined PSNE
   encoding for bitmap-processing errors (M59). M15 is fixed; M31/M89 use the
   documented contextual interpretations. M60 is independently verified.
2. **Priority software gaps closed:** M61 has the mandatory NACK matrix;
   M73/M74 have independent withheld/released retry observations;
   M79/M80/M82 combine exact-clock production endpoint tests with live public
   capability wiring; M86 has ERROR-before-state-release fixes and witnesses
   for public and direct endpoint cancellation, including flush overflow.
3. **Resource maxima verified:** 1024 simultaneous WRITEIMM credits, 32 live
   rails, global profile saturation with 1024 populated EVs, and 32 distinct
   CC profiles actively limiting 32 simultaneous QPs. These are specific
   capacity contracts, not arbitrary combinations or long-duration stress.
4. **Deployment scope:** M92 is verified on the explicitly configured
   software packet fabric; M96 uses an independent RFC 9800 oracle and Linux
   NEXT-CSID forwarding. Physical switch/vendor interoperability, hardware
   trim generation, link-rate tuning, physical async events, controller port
   mutation under load, and a freshly pinned hardware jumbo run remain
   additional deployment targets. They must not be silently substituted for
   the software evidence or automatically treated as intrinsic requirements
   of every endpoint clause.



### Expanded coverage follow-up (2026-11-21)

- M61: `check_mrc_write.py --mandatory-nacks-only` completed all 20
  combinations of five mandatory NACK reasons and four local/peer trim
  settings. Assertions include matching sent/received reason masks, recovery
  completions, retry exhaustion, and typed remote-operation failure.
- `check_mrc_requester.py --trim-disabled-only` passed 12 independent
  injection cases: packet-buffer, regular trim, and last-hop trim NACKs across
  IPv4, IPv6, SRv6, and SRv6 with SRH. The requester retained alternate-rail
  retransmission, payload placement, and semantic completion with local
  trim generation disabled. Both sets are included in their default suites.
- Evidence logs: `/tmp/mrc-audit-mandatory-nacks-oob.log`,
  `/tmp/mrc-audit-mandatory-nacks-oob-repeat.log`, and
  `/tmp/mrc-audit-trim-disabled.log`. An initial matrix attempt did not
  trigger the bitmap fault because the short transfer used only rail 0.
  Resource-fault cases now exchange capabilities through TCP OOB before
  traffic, and the CQ-overrun case explicitly negotiates four WRITEIMM
  credits so it can overflow its one-entry CQ. The revised 20-case matrix
  passed twice. The out-of-window case intentionally uses mismatched PSNs
  without OOB. Those historical runs generated a NACK; the 2026-11-22
  correction now requires silence and timeout exhaustion for the same case. No general all-link startup
  guarantee is inferred from these runs.

- RTT and SVC-API: 672 exact-clock numeric feedback cases and live public
  capability wiring pass. Temporary builds with the wrong capability
  direction, missing service compensation, ignored ingress timestamp, or
  forced public peer capability all fail their targeted assertions.
- MPR-PEER: 24 cases pass across four envelopes (shrink, static, equal CACK,
  stale CACK, zero MPR, oversized MPR). Removing only retry-window admission
  makes the shrink test fail. A larger flight exposed the relay's nonblocking
  datagram backpressure; it now retains one burst, services reverse traffic,
  and fails a stalled fixture after three seconds. An earlier isolated relay
  disconnect could not be diagnosed from the old discarded child output;
  current failures preserve both child diagnostics. Subsequent bounded runs
  passed, including the final isolated 24-case matrix.
- U-QP: all 6656 valid timeout parameter configurations plus invalid and
  maximum retry-count boundaries pass. Receive-history tests process a full
  24-bit PSN reuse cycle plus 128 packets, verify half-space saturation and
  oldest duplicate handling, and snapshot unchanged state for rejected PSNs.
  History off-by-one and timer boundary mutations are rejected.
- APP capacity exposed a production defect: a legal responder WIMM1024/MPR1
  attachment returned EINVAL because work slots were allocated from the
  packet window alone. `configure_route` now sizes semantic work capacity
  for both limits. The regression verifies that attachment, then independently
  saturates 1024 semantic credits with a larger packet window: 1025 posted
  WRITEIMMs, SACKs allowed, semantic ACKs held, exactly 1024 receive completions
  for 20 ms, then credit release and 1025 ordered completions with exact bytes,
  immediate values, and allocation guards.
- CTL allocates 32 ONLINE custom formats, 32 ONLINE EV profiles with 32 entries
  each, and 32 ONLINE CC profiles; verifies ENOSPC at the next allocation,
  reopened persistence, and reuse after returning a profile to INIT.
- Public 32-rail traffic passes with all rail bits observed. The fixture orders
  memif client disconnect before server ring teardown after a concurrent
  teardown crash; five subsequent counted-rail suites pass.
- WRITE now defaults to a test-only all-rail readiness wrapper. It has a
  three-second deadline and explicit port diagnostics; an unconnected-rail
  negative test fails after 3.027 seconds. The production CLI startup is
  unchanged. Its full expanded suite passes 89 cases, including all 32 rails,
  20 mandatory NACK combinations and 13 configurable UDP destination cases
  (ports 1/4791/65535 in four envelopes plus mismatched-port rejection).



### Earlier expanded-coverage verification

The goal here is stronger requirement-level software evidence, not a claim
that all 98 transport requirements are conformant. Specification interpretation
and independent hardware requirements retain their Q/H status above.

| Run | Final result | Evidence log |
| --- | --- | --- |
| Portable tests | All 10 programs pass, including 6656 timer configurations and full PSN reuse | `/tmp/mrc-qp-boundary-units.log` |
| Public application/controller | 43 scenarios pass, including 1024 outstanding WRITEIMMs and 32 rails | `/tmp/mrc-expanded-public-final.log` |
| Standalone with all-rail readiness | 89 scenarios pass | `/tmp/mrc-expanded-write-ready-full.log` |
| Independent requester | 42 scenarios pass | `/tmp/mrc-expanded-peer-full.log` |
| Independent MPR peer | 24 scenarios pass | `/tmp/mrc-audit-mpr-final.log` |
| Exact-clock endpoint RTT | 672 numeric cases pass | `/tmp/mrc-endpoint-rtt.log` |
| Live public Service Time wiring | Both peers and both capability directions pass | `/tmp/mrc-expanded-service-final.log` |
| Public ABI | Passes after rebuild against current sources | `/tmp/mrc-expanded-abi-final.log` |
| Controller | Permission rejection and full lifecycle/capacity suites pass | `/tmp/mrc-expanded-ctl-denied.log`, `/tmp/mrc-expanded-ctl-final.log` |

The first full public run detected an unintended test-fixture mask edit that
zeroed WRITEIMM credits in the existing MPR test. That edit was corrected;
focused MPR/capacity checks and the full 43-scenario rerun pass. The tested
production change in this follow-up is the independent WRITEIMM work-capacity
allocation fix, in addition to the earlier SACK/SRv6 fixes retained above.
No physical-port or external-network changes were performed.

The earlier expanded-coverage source manifest includes every `.c`, `.h`, and `.py` file under
`mrc-dpdk/src` and `mrc-dpdk/tests`, plus `mrc-dpdk/Makefile` and
`mrc-dpdk/.gitignore`, sorted by repository-relative path. Each manifest line
is `<file SHA-256>  <path>\n`. Its SHA-256 is
`55d80f75093a05a7be2529bf2c2fc67fa8eac453ad9f03696ac90971d8d70004`; the inspected manifest is
`/tmp/mrc-expanded-source-sha256.txt`. This includes newly added untracked
fixtures, unlike a plain working-tree `git diff` hash.

| Final artifact | SHA-256 |
| --- | --- |
| `mrc_write` | `1b4cb05a3aa3e03ae26e4ce7ccc14cb596f792764f55966ccb8303822d274e20` |
| `tests/mrc_write_ready` | `719e023ccc3438fcb173802e935c578008cc5b4c4bbb48d6c29b985e11512e85` |
| `libmrc-dpdk.so` | `f78395bf14224232d358071bed28844ec982d7a1547817374873d28a42357f55` |
| `tests/mrc_abi_application` | `e939e2a71abac61dbd5c8412a88c428bc7127b5aa8124b8169baf83a57e17918` |
| `tests/test_mrc_ctl_abi` | `e565e59c1d1b511e111920891b6e98e80f54d5b3fb2e8ffc141c41d555517197` |
| `tests/mrc_endpoint_rtt` | `94a8783ddfda8fb2e7339c1fcf4920cc33397e71fb48e7c9cf60cacd09d90ad8` |
| `tests/mrc_public_service_time` | `bff24d8dc7984a47dce5e732d5e3347b90f1102b51bbac1b04b4af52fa50a2e5` |


## Specification questions for interoperability

- The endpoint uses the configurable UDP destination and default 4971 specified by §7.5.5.2. Table 7-9 shows the RoCEv2 value 4791, which remains selectable with `--udp-port 4791`.
- Sections 7.5.1 and 7.5.5 describe the PDC IDs differently. The implementation uses the requestor's QPN low 16 bits as SPDCID and the responder's QPN low 16 bits as DPDCID, following Tables 7-6, 7-15, and 7-17.
- Section 6.3.1 outside-window silence is now enforced, including forward overruns. Table 7-3 mandatory responder `PSN_OOR_WINDOW` generation remains unresolved under M30; the former forward-overrun trigger has been removed.
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

Requester SACK trigger validation is covered by `make check` and
`python3 tests/check_mrc_requester.py --sack-trigger-only`. Data SACKs must
identify a retained transmitted packet before cumulative delivery, bitmap,
MPR, EV, RTT, or congestion feedback is applied. Probe responses retain their
separate identifier/entropy correlation. Unit regressions compare the entire
QP and packet-slot state before and after rejection, including 24-bit PSN
wrap, and accept valid negative trigger offsets and opaque probe tokens.
The independent peer sends future and pre-start trigger PSNs with otherwise
valid cumulative ACKs and byte credits, a window-restore flag, and an
ALWAYS_SKIP mark. The requester retains its one-packet window, ignores both
SACKs without EV transitions or retries, and accepts subsequent valid data
SACKs on the other rail with the original entropy. All four modes (IPv4,
IPv6, SRv6, and SRv6 with SRH) passed. Both the unit and live-peer regressions
failed before the fix. This verifies software behavior over memif; external
MRC NIC interoperability remains unverified.

For §§6.2.4 and 6.3.3–6.3.5, three WRITEIMMs with one posted receive
produce one successful requester completion, one remote-operation error,
and one flush. The memif check receives only the Transport NAK and the
focused case also passed across 24-bit PSN wrap. Independent IPv4, IPv6,
and preserved SRv6 replays with and without a final SRH deliver the third
WRITEIMM and both fragments of the second before the first. The first
consumes the receive descriptor. The NAK carries PSN 1, the failed
WRITEIMM's final packet, and completed MSN 1. The unit, ABI, memif,
independent requester, and packet replay suites passed on Spark-04.

For §6.3.1, the standalone memif cases exchange WRITEIMM and Port Status
between full and limited members of the same partition, with either peer
acting as requester. Two limited members and distinct partitions produce
no data placement or feedback. Direct and negotiated connected routes use
the same RC membership check. TCP-negotiated routes also pin the offered
peer P_Key. [Linux RXE P_Key comparison](https://github.com/torvalds/linux/blob/v6.17/drivers/infiniband/sw/rxe/rxe_verbs.h)

For §§6.3.1 and 7.4.4, independent IPv4, IPv6, and preserved SRv6
replays verify the receive-window boundary across PSN wrap. Untrimmed
and trimmed WRITEs before connection start or exactly half a PSN
space away produce no feedback. Forward window overruns now also produce no
feedback, and the valid next WRITE completes (2026-11-22 correction).

For §§6.2.2.2 and 6.3.1, public memif tests check 256- and 512-byte
segment lengths, address increments, and WRITEIMM completion across
PSN wrap. The standalone tests verify mixed work and smaller-PMTU
selection in both directions of an asymmetric TCP offer. Independent
captures verify requester payload, RETH geometry, and immediate data at
2048 and 4096 bytes. Separate replays verify out-of-order 4096-byte
placement over IPv4, IPv6, and preserved SRv6 with and without a final
SRH. The Linux SRv6 fabric's `--jumbo` case negotiated PMTU 4096 and
delivered a 1,048,749-byte WRITEIMM. Kernel ingress counters saw 128
full-sized packets on each rail. The receiver validated the complete
buffer, including the 173-byte tail. One rail preserved the header and
the other decapsulated it with `End.DX6`. Jumbo transfers on the physical
NICs remain unverified.

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

For §7.5.2, the requester sets BTH.AR when the next segment is blocked
by its tracking window, the peer's MPR, or the WRITEIMM credit limit.
With only one credit and two queued WRITEIMMs, the independent peer
checks BTH.AR on the first WRITEIMM's last segment and withholds the
credit until a Transport ACK. The requester then completes both WRs.
Run this with `--flow-control-only`.

The independent responder replay resends a buffered middle segment
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
the local ACK timeout. A fourth drops the original and its fast retry
with zero linear retries. The timeout following the fast retry uses
the original Local ACK Timeout, as for a NACK-driven retry. Use
`--fast-loss-only` with `--mode` to check IPv4, IPv6, and preserved SRv6
with or without an SRH.

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
to two units. The ECN case configures and reads back marking rules on both routers,
counts actual CE bits and reflected SKIP SACKs per EV, and checks the
requester's skip counter. On the tested kernel, native End.DX6 discarded
outer CE; the fixture configures an explicit ECT-only pre-decap CE merge
and verifies bare-inner CE on egress. A separate four-codepoint test ensures
Not-ECT remains unchanged. This is deterministic software policy evidence,
not queue-driven WRED or a claim of native tunnel CE propagation.
The trim case sends an endpoint-injected trim through the preserving
router, checks the TRIM NACK, and verifies retransmission through the
decapsulating rail. Independent Linux NEXT-CSID forwarding is now covered by CSID; physical
switch/vendor interoperability and switch-generated trimming remain separate
verification targets.

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
The three-Spark `negotiated` profile uses eight 32 MiB WRITEIMMs per edge.
The same transfer passed over two local memif rails with eight
completions on both peers, one Dynamic MPR update at each peer, and
131,072 received segments per rail. A standalone check verified that all
four SSH forwards opened on loopback and closed on exit. The installed
profile also passed on all three Sparks, as recorded below.

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

### Three-Spark verification after the Transport NAK fix

Spark-04, Spark-02, and Spark-03 passed smoke, injected failover, and
negotiated WRITEIMM/Dynamic MPR in that order on the Polatis double ring.
Before each run, all three test worktrees matched
`fb324750bf52e92ed94ac5fe8633f407eee59e10`. The installed
`/opt/ocs-mrc/fb32475.psTsQnf9/mrc_write` matched SHA-256
`b85dc23a741c51b840d94222564daaf3598ff0d6e7eaec1f8bb7761aae91f3a7`
on all three Sparks. All 18 sender and receiver journal invocations
were checked against the live harness output.

| Profile | Edge | RX segments by rail | TX retries by rail |
| --- | --- | --- | --- |
| Smoke | Spark-04 to Spark-02 | 133,145 / 128,999 | 108 / 0 |
| Smoke | Spark-02 to Spark-03 | 131,072 / 131,072 | 0 / 0 |
| Smoke | Spark-03 to Spark-04 | 130,935 / 131,209 | 0 / 0 |
| Failover | Spark-04 to Spark-02 | 133,724 / 128,420 | 1,860 / 4,876 |
| Failover | Spark-02 to Spark-03 | 131,776 / 130,368 | 1,633 / 1,514 |
| Failover | Spark-03 to Spark-04 | 131,264 / 130,880 | 61 / 223 |
| Negotiated | Spark-04 to Spark-02 | 131,072 / 131,072 | 0 / 0 |
| Negotiated | Spark-02 to Spark-03 | 131,384 / 130,760 | 21 / 8 |
| Negotiated | Spark-03 to Spark-04 | 131,072 / 131,072 | 0 / 0 |

Every receiver verified 256 MiB of data. The three sender port-ready
intervals overlapped by at least 1,878.1 ms for smoke, 1,902.0 ms for
failover, and 1,783.1 ms for negotiated. In failover, Spark-04 recorded
127 injected drops and reported both EV failure and recovery on rail 1.
In the negotiated run, each edge completed eight 32 MiB WRITEIMMs with
registered memory, eight immediate completions at the receiver, and one
MPR update at each peer.

### Three-Spark verification with Spark-01

After recabling, both rails passed link-local reachability checks on
Spark-04 → Spark-02 → Spark-01 → Spark-04. The staged reroute was
installed on all three hosts, and the `rkp64` account verified 18 scoped
passwordless commands on Spark-01, 28 on Spark-02, and 30 on Spark-04.

Smoke, injected failover, and negotiated WRITEIMM/Dynamic MPR passed in
that order. Before each profile, the three Git worktrees matched
`32d67ee5464821c0ad19fbc8e35caa1a5bd3927f` and the installed binaries
matched SHA-256 `edc9f659265dbc55e53506fcbfbf29419f34fe3c3599b00f0781e1ebc1f5b1d0`.

| Profile | Spark-04 → Spark-02 RX segments | Spark-02 → Spark-01 RX segments | Spark-01 → Spark-04 RX segments |
| --- | --- | --- | --- |
| Smoke | 131,933 / 130,211 | 131,139 / 131,005 | 131,142 / 131,002 |
| Failover | 132,751 / 129,393 | 130,693 / 131,451 | 134,502 / 127,642 |
| Negotiated | 131,204 / 130,940 | 131,123 / 131,021 | 131,796 / 130,348 |

Each entry lists rail 0 and rail 1. Each receiver verified 256 MiB.
The sender intervals overlapped by at least 1,875.6 ms, 1,998.9 ms, and
1,811.6 ms in profile order. The failover sender on Spark-04 recorded
127 injected drops with failure and recovery on rail 1. The negotiated
profile completed eight 32 MiB WRITEIMMs per edge and applied one
Dynamic MPR update at each endpoint.

### Spark-04 to Spark-02 link recovery and jumbo MTU

The two-rail link-recovery profile completed a 768 MiB WRITE using the
installed `fb32475` build at PMTU 1024. Spark-04's outgoing rail 1
(`enP2p1s0f0np0`, PCI `0002:01:00.0`) was taken administratively down
and restored during the transfer. Independent mlx5 vport counters
recorded 40,985,020 transmit bytes on rail 0 during the 258.7 ms sampled
down interval, while rail 1 recorded 94 bytes. Rail 1 carried another
7,517,246 bytes before the WRITE completed. Spark-02 verified all
786,432 segments, receiving 422,871 on rail 0 and 363,561 on rail 1.
Spark-04 carrier returned on rail 1 and Spark-02 recorded no peer
carrier transitions. This verifies recovery from a local NIC
administrative transition on the optical test edge.

The negotiated PMTU 4096 hardware run on `fb32475` stopped while
setting up Spark-02's mlx5 receive queue. The driver required 4,186
bytes plus 128 bytes of headroom while the pool supplied 4,310 bytes.
The pool now reserves the Ethernet FCS in addition to the configured
frame and headroom. The application regression checks that reservation.
The corrected build passed independent jumbo packet replay and the Linux
TAP test, which negotiated PMTU 4096 and counted 128 full-sized packets
per rail. The hardware run restored all four ports to MTU 1500.
On the replacement ring, the same revision and checksum were verified
before each pair run. The 768 MiB link-recovery test passed again at
PMTU 1024. During the sampled 288.7 ms administrative outage, rail 0
transmitted 43,138,402 bytes and rail 1 recorded 94 bytes. The restored
rail then transmitted another 4,399,594 bytes before completion. Spark-02
verified 428,292 segments on rail 0 and 358,140 on rail 1, with zero
peer carrier transitions.

The corrected mlx5 build opened both ports and negotiated PMTU 4096
on both jumbo attempts. The sender scheduled 3,732 and 3,467 segments
respectively before retries exhausted, and the receiver logged mlx5
receive completion queue errors on both attempts. The second attempt also
logged a transmit queue error on Spark-04. Both cleanups restored MTU 1500
on the four test ports. Those units could not write driver dumps into
their read-only working directories.

With `rx_vec_en=0` on both rails, two hardware runs on the same pin
completed 256 MiB at negotiated PMTU 4096. In the validated run,
Spark-02 checked all 65,536 segments and eight immediate completions.
It placed 33,040 segments on rail 0 and 32,496 on rail 1. Both endpoints
applied one Dynamic MPR update. The Spark-02 physical frame-size
counter for 4,096 to 8,191 bytes increased by 70,866, including
retransmitted traffic. The four ports returned to MTU 1500 with
carrier up. [DPDK 23.11 mlx5 receive modes](https://doc.dpdk.org/guides-23.11/nics/mlx5.html#rx-burst-functions)

The validated run still reported mlx5 completion queue errors and
5,395 sender retries. DPDK input error counters on rail 0 and rail 1
increased by 8,071 and 13,105 at the sender and 10,118 and 12,062 at the
receiver. The first scalar run's journal tail lost startup messages,
so the verification run captured port readiness and PMTU negotiation
live before collecting terminal results. The current units provide a
writable working directory under systemd's private runtime directory.
An administrator retrieved 240 dump headers. All reported mlx5 CQE
syndrome `0x04` (local protection error). The headers include 235 RX
errors across both endpoints and five TX errors on Spark-04. On RX,
214 headers reported WQE counters from 1,024 through 1,047 with
1,024 configured descriptors. [Linux mlx5 completion status](https://github.com/torvalds/linux/blob/master/drivers/infiniband/hw/mlx5/cq.c#L281-L283)

Both hosts started at zero reserved hugepages. The comparison changed
EAL backing from `--no-huge` on 4 KiB Linux pages to 64 temporary
2 MiB pages per host. The pinned binary, PMTU 4096, scalar mlx5 Rx,
and traffic profile were identical. Both endpoint processes exited
successfully. Spark-02 verified 256 MiB in 65,536 segments, split
33,588 / 31,948 across the two rails, with eight immediate
completions and one Dynamic MPR update per endpoint. All four DPDK
ports reported zero input and output errors. Sender retries fell
from 5,395 to 296. This supports a memory-backing dependency.
The exact failed registration in the 4 KiB configuration remains
unverified.
[DPDK Linux hugepage requirements](https://doc.dpdk.org/guides-23.11/linux_gsg/sys_reqs.html#use-of-hugepages-in-the-linux-environment)

The jumbo harness restored all four ports to MTU 1500. Both
`ExecStopPost` cleanup hooks failed with `EBUSY` at the private
backing path after DPDK exited. Each host retained 64 free 2 MiB
pages with zero reservations in use. Neither backing directory was
a mount point in the host namespace. The admin update released
that run's idle pages and removed the per-service `ReadWritePaths`
bind mount. The cleanup hook now releases the reservation before
removing its backing directory.
[systemd filesystem protection](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html#ProtectSystem=)

The corrected run verified the same three Git hashes and binary
checksums. Spark-02 verified 256 MiB at negotiated PMTU 4096, with
33,166 / 32,370 segments across both rails and eight immediate
completions. Both endpoints recorded one Dynamic MPR update. The
receiver's physical frame-size counter for 4,096 to 8,191 bytes
increased by 65,679. Sender retries were 85 / 58. All four DPDK
ports recorded zero input and output errors, missed packets, and
buffer allocation failures. Both services exited successfully.
The test automatically released all 64 temporary hugepages per
host, removed the backing and reservation-state directories, and
restored the four test ports to MTU 1500 with carrier up.

### Independent hardware interoperability

Hardware interoperability is blocked because the available lab
equipment includes no NIC that implements OCP MRC 1.0 in hardware.
Availability of a fabric supporting hardware SRv6, ECN, and
switch-generated trimming is unconfirmed. The Polatis double ring
supplies optical connectivity. Independent software packet replay,
the Python memif peer, and Linux SRv6 fabric tests provide protocol
evidence. Interoperability with an external MRC endpoint and hardware
forwarding behavior remain unverified.
