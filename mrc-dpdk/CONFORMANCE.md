# OCP MRC 1.0 conformance status

The conformance target is the [OCP Multipath Reliable Connection 1.0 specification](https://www.opencompute.org/documents/ocp-mrc-1-0-pdf/). The SRv6 emulator is a reference for path behavior and reorder metrics. An independent MRC endpoint or qualified packet captures are needed to verify wire interoperability.

| Scope | Verified in the current slice | Remaining evidence or implementation |
| --- | --- | --- |
| §§6.1–6.4, 10.1.2, request and completion semantics | A QP with configurable or TCP-negotiated QPNs, PSN, static MPR, partition, and memory attributes carries multiple WRITEs. The responder validates per-WR geometry and the registered region, consumes posted receive credits, and emits WRITEIMM completions in order. With zero posted receive credits, the sender receives Remote Operational Error. Negotiated transfers use a protection-domain MR registry and hold destination leases through ordered completion. Requester and responder completions pass through bounded CQs. | Complete QP and CQ lifecycle APIs, provider interoperability for out-of-band attributes, and the remaining semantic errors. |
| §§6.2.2, 7.5.5, wire format | Independent byte vectors for IPv4/UDP, IPv6/UDP, and preserved SRv6 envelopes, iCRC, WRITE with immediate and TSETH, AETH, SETH, NETH, and PETH. | Hardware interoperability, transit routing on an independent SRv6 fabric, and remaining edge-case packet validation. |
| §§7.2, 7.4–7.5, packet reliability | 24-bit PSN and window checks, wrap, selective SACK bitmaps, separate AETH, duplicate suppression, retransmit marking, logical timers, SACK-driven fast loss, reliability probe request/response matching and RTT sampling, and separately classified trimmed packets with TRIM NACK recovery. Negotiated Dynamic MPR advertises each change in SACKs and gates both first transmissions and retries, while preserving outstanding receive state. | Physical verification of probes and full mandatory NACK generation and error transitions. Trim NACK generation and service time are capability-gated. |
| §§7.3, 7.4.7–7.4.8, 7.5.6, endpoint operations | ERTH/EETH byte fixtures and a two-path Port Status Update exchange using reserved QPN 2 and echoed tokens. | Physical port transitions, out-of-order status handling, and optional EV probe initiation. |
| §8, congestion control | Nominal per-packet accounting, modulo 24-bit feedback, congestion-window admission and RTT/ECN-driven NSCC adjustments. | Full negotiated per-QP control, deployment measurements, and optional receiver penalty and quick adaptation. Verify ECN and trimming on a packet-switching fabric or with controlled injection. |
| §§9–10, EVs and APIs | Two paths with fixed UDP source-port EVs, IPv6 flow-label reflection, preserved or already decapsulated SRv6 receive, and alternate-path retransmission. Active-only rotation distinguishes denied, congestion-skipped, and failed EVs. A fresh correlated probe can restore a failed EV. | Persistent EV profile management, controller events, and OCP application/controller interfaces. Verify supported optional routing and EV modes separately. |

## Specification questions for interoperability

- Section 7.5.5.2 gives UDP destination 4971 in prose. Table 7-9 and standard RoCEv2 use 4791. The endpoint defaults to 4791 and accepts an explicit `--udp-port`.
- Sections 7.5.1 and 7.5.5 describe the PDC IDs differently. The implementation uses the requestor's QPN low 16 bits as SPDCID and the responder's QPN low 16 bits as DPDCID, following Tables 7-6, 7-15, and 7-17.
- Table 6-5 specifies whole-WR DMA length in every RETH. The endpoint uses that value and validates each payload within the registered region.
- Polatis connects Ethernet links optically. The Spark double ring can verify path use and recovery but does not itself supply ECN marks or packet trimming.

## Verification

From `mrc-dpdk/`, run `make check` for wire, QP, work-request, congestion, connection negotiation, provider resource, EV, and receive-window tests without DPDK. Build with `make` and run `python3 tests/check_mrc_write.py` for the IPv4 and IPv6 endpoints over two local DPDK memif links. This checks short and zero-length WRITEs, striping, separate ACK semantics, ordered WRITEIMM completions, receive-credit exhaustion, node port-status updates, correlated reliability probes, TCP QP negotiation, PSN wrap, packet loss, probe-driven path restoration, negotiated receive-window shrink, and synthetic trimming of bare IP and preserved SRv6 frames.

Use Spark-04 as the implementation source. After pushing the exact clean commit, run `tests/verify_spark_revisions.sh --sync "$(git rev-parse HEAD)"` to advance the detached test worktrees on Spark-02 and Spark-03. Run the same command without `--sync` immediately before each live test. The script checks that all three worktrees are clean and at the requested commit.

The first physical test pair is Spark-04 and Spark-02 using the two direct circuits. mlx5 setup requires `CAP_NET_RAW` and `CAP_NET_ADMIN` or an administrator-run endpoint. [DPDK mlx5 privilege documentation](https://doc.dpdk.org/guides-23.11/platform/mlx5.html#run-as-non-root)
