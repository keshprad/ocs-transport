# OCP MRC 1.0 conformance status

The conformance target is the [OCP Multipath Reliable Connection 1.0 specification](https://www.opencompute.org/documents/ocp-mrc-1-0-pdf/). The SRv6 emulator is a reference for path behavior and reorder metrics. An independent MRC endpoint or qualified packet captures are needed to verify wire interoperability.

| Scope | Verified in the current slice | Remaining evidence or implementation |
| --- | --- | --- |
| §§6.1–6.4, 10.1.2, request and completion semantics | A statically matched QP carries one WRITE or WRITE with immediate. All fragments carry RETH and are placed out of order in one checked host-memory region. The sender retires packets on a semantic transport ACK and the receiver reports immediate completion only after all fragments are placed. | Out-of-band QP attribute exchange, multiple in-flight work requests with ordered completions, registered-memory and receive-queue resource management, and the full semantic error and QP-state transitions. |
| §§6.2.2, 7.5.5, wire format | Independent byte vectors for the IPv4/UDP envelope, iCRC, WRITE with immediate and TSETH, AETH, SETH, NETH, and PETH. | IPv6 and SRv6 modes, hardware interoperability, and edge-case packet validation and control-plane dispatch. |
| §§7.2, 7.4–7.5, packet reliability | 24-bit PSN and window checks, wrap, selective SACK bitmaps, separate AETH, duplicate suppression, retransmit marking, logical timers, SACK-driven fast loss, reliability probe responses, and separately classified trimmed packets with TRIM NACK recovery. Static receive window is 1,024 packets. | Full mandatory NACK generation and error transitions, plus verification of proactive probes under lost feedback. Dynamic MPR, trim NACK generation, and service time are capability-gated. |
| §§7.3, 7.4.7–7.4.8, 7.5.6, endpoint operations | Independent ERTH and EETH byte fixtures cover encoding and decoding. | Port Status Update state and endpoint request/response correlation. EV probes are optional. |
| §8, congestion control | Nominal per-packet byte accounting, modulo 24-bit feedback, congestion-window admission, and RTT and ECN feedback classification. | Adaptive NSCC window updates and per-QP control. Verify ECN and trimming on a packet-switching fabric or with controlled injection. |
| §§9–10, EVs and APIs | Two IPv4 paths with fixed UDP source-port EVs and alternate-path retransmission. | EV profile management, state and recovery events, packet entropy attribution, and OCP application/controller interfaces. Verify supported optional routing and EV modes separately. |

## Specification questions for interoperability

- Section 7.5.5.2 gives UDP destination 4971 in prose. Table 7-9 and standard RoCEv2 use 4791. The endpoint defaults to 4791 and accepts an explicit `--udp-port`.
- Sections 7.5.1 and 7.5.5 describe the PDC IDs differently. The implementation uses the requestor's QPN low 16 bits as SPDCID and the responder's QPN low 16 bits as DPDCID, following Tables 7-6, 7-15, and 7-17.
- Table 6-5 specifies whole-WR DMA length in every RETH. The endpoint uses that value and validates each payload within the registered region.
- Polatis connects Ethernet links optically. The Spark double ring can verify path use and recovery but does not itself supply ECN marks or packet trimming.

## Verification

From `mrc-dpdk/`, run `make check` for wire, QP, and congestion accounting tests without DPDK. Build with `make` and run `python3 tests/check_mrc_write.py` for the IPv4/UDP endpoint over two local DPDK memif links. This checks short and zero-length WRITEs, striping, separate ACK semantics, WRITE with immediate, PSN wrap, packet loss, and synthetic trimming.

Use Spark-04 as the implementation source. After pushing the exact clean commit, run `tests/verify_spark_revisions.sh --sync "$(git rev-parse HEAD)"` to advance the detached test worktrees on Spark-02 and Spark-03. Run the same command without `--sync` immediately before each live test. The script checks that all three worktrees are clean and at the requested commit.

The first physical test pair is Spark-04 and Spark-02 using the two direct circuits. mlx5 setup requires `CAP_NET_RAW` and `CAP_NET_ADMIN` or an administrator-run endpoint. [DPDK mlx5 privilege documentation](https://doc.dpdk.org/guides-23.11/platform/mlx5.html#run-as-non-root)
