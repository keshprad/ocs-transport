# OCP MRC 1.0 conformance status

The conformance target is the [OCP Multipath Reliable Connection 1.0 specification](https://www.opencompute.org/documents/ocp-mrc-1-0-pdf/). The SRv6 emulator is a reference for path behavior and reorder metrics. An independent MRC endpoint or qualified packet captures are needed to verify wire interoperability.

| Scope | Verified in the current slice | Remaining evidence or implementation |
| --- | --- | --- |
| §§6.1–6.4, 10.1.2, request and completion semantics | A QP with configurable or TCP-negotiated QPNs, PSN, static MPR, partition, and memory attributes carries mixed WRITE and WRITEIMM requests with independent lengths and receive sequence numbers. The responder validates per-WR geometry and the registered region, consumes posted receive credits, and emits WRITEIMM completions in order. With zero posted receive credits, the sender receives Remote Operational Error. Negotiated transfers use a protection-domain MR registry. Posted WRs retain their source leases through semantic acknowledgement or flush, and the responder retains destination leases through ordered completion. A local provider QP holds bounded send and receive queues, supports RESET/INIT/RTR/RTS/ERROR, and publishes ordered success, error, and flush completions. A Linux completion channel signals a pollable FD for armed CQ events across multiple CQs and waits for acknowledgement of fetched events when destroying a CQ. Verified remote access errors and WRITEIMM without receive credits over memif. The optional shared library implements the OCP context and CQ/channel signatures using a real libibverbs context, including WC field translation, blocking and nonblocking event retrieval, and parent lifetime checks. Tests bind a real verbs MR to the software PD using its original keys and verify local posting, remote lookup, and independent lease lifetimes. | Public libmrc QP ABI and MR integration, device and QP attributes, asynchronous events, and provider interoperability for out-of-band attributes. |
| §§6.2.2, 7.5.5, wire format | Independent byte vectors for IPv4/UDP, IPv6/UDP, and preserved SRv6 envelopes, iCRC, WRITE with immediate and TSETH, AETH, SETH, NETH, and PETH. | Independent wire interoperability, transit routing on an independent SRv6 fabric, and remaining edge-case packet validation. |
| §§7.2, 7.4–7.5, packet reliability | 24-bit PSN and window checks, wrap, selective SACK bitmaps, separate AETH, duplicate suppression, retransmit marking, logical timers, SACK-driven fast loss, reliability probe request/response matching and RTT sampling (probe exchanges observed on the Spark pair), and separately classified trimmed packets with TRIM NACK recovery. Negotiated Dynamic MPR advertises each change in SACKs and gates both first transmissions and retries, while preserving outstanding receive state. Over memif, all five mandatory Table 7-3 NACK reasons were generated and handled, including retry-budget exhaustion and terminal CQ overrun. | Independent NACK interoperability and recovery after an actual port transition. Both Sparks verified a 16 MiB WRITE after an injected path-1 blackhole. Provider QP errors and typed send completions are verified for retry exhaustion and UNEXP_EVENT. Trim NACK generation and service time are capability-gated. |
| §§7.3, 7.4.7–7.4.8, 7.5.6, endpoint operations | ERTH/EETH byte fixtures and a two-path Port Status Update exchange using reserved QPN 2 and echoed tokens, observed over memif and the Spark pair. | Physical port transitions, out-of-order status handling, and optional EV probe initiation. |
| §8, congestion control | Nominal per-packet accounting, modulo 24-bit feedback, congestion-window admission and RTT/ECN-driven NSCC adjustments. | Full negotiated per-QP control, deployment measurements, and optional receiver penalty and quick adaptation. Verify ECN and trimming on a packet-switching fabric or with controlled injection. |
| §§9–10, EVs and APIs | Two paths with configurable UDP source-port EVs, IPv6 flow-label reflection, preserved or already decapsulated SRv6 receive, and alternate-path retransmission. A persistent local controller binds an online profile to an active QP and applies administrative deny/allow changes during transfer. Active-only rotation distinguishes denied, congestion-skipped, and failed EVs. A fresh correlated probe can restore a failed EV. | Controller events and OCP application/controller interfaces. Verify supported optional routing and EV modes separately. |

## Specification questions for interoperability

- Section 7.5.5.2 gives UDP destination 4971 in prose. Table 7-9 and standard RoCEv2 use 4791. The endpoint defaults to 4791 and accepts an explicit `--udp-port`.
- Sections 7.5.1 and 7.5.5 describe the PDC IDs differently. The implementation uses the requestor's QPN low 16 bits as SPDCID and the responder's QPN low 16 bits as DPDCID, following Tables 7-6, 7-15, and 7-17.
- Table 6-5 specifies whole-WR DMA length in every RETH. The endpoint uses that value and validates each payload within the registered region.
- Polatis connects Ethernet links optically. The Spark double ring can verify path use and recovery but does not itself supply ECN marks or packet trimming.

## Verification

From `mrc-dpdk/`, run `make check` for wire, QP, work-request, congestion, connection negotiation, provider resource and QP lifecycle, EV, controller storage, and receive-window tests without DPDK. Build with `make` and run `python3 tests/check_mrc_write.py` for the IPv4 and IPv6 endpoints over two local DPDK memif links. This checks short and zero-length WRITEs, striping, separate ACK semantics, mixed WRITE and WRITEIMM lengths and receive sequencing across loss and PSN wrap, ordered WRITEIMM completions, receive-credit exhaustion, bad-rkey errors, ordered error and flush completions, node port-status updates, correlated reliability probes, TCP QP negotiation, PSN wrap, packet loss, probe-driven path restoration, a live controller update with custom IPv6 EVs, negotiated receive-window shrink, mandatory reliability NACKs, and synthetic trimming of bare IP and preserved SRv6 frames.

Use Spark-04 as the implementation source. After pushing the exact clean commit, run `tests/verify_spark_revisions.sh --sync "$(git rev-parse HEAD)"` to advance the detached test worktrees on Spark-02 and Spark-03. Run the same command without `--sync` immediately before each live test. The script checks that all three worktrees are clean and at the requested commit.

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

mlx5 setup requires `CAP_NET_RAW` and `CAP_NET_ADMIN` or an administrator-run endpoint. [DPDK mlx5 privilege documentation](https://doc.dpdk.org/guides-23.11/platform/mlx5.html#run-as-non-root)
