# RDMA benchmark results

Status: COMPLETE — both endpoint audits, log integrity, cleanup, and both JOB_DONE acknowledgments verified.

For bandwidth, forward means spark-7e64 writes to spark-28a3; reverse means spark-28a3 writes to spark-7e64. Latency direction labels identify client placement: spark-7e64 for forward, spark-28a3 for reverse; latency is measured through a ping-pong exchange. Bidirectional bandwidth is the combined throughput of both directions. Concurrent values sum the two path results from the same overlapping test case, not sequential measurements.

Host-memory RDMA, one QP per path, Ethernet MTU 1500, active RDMA MTU 1024. Bandwidth cases request 30 seconds; stability requests 300 seconds. Latency uses 100,000 iterations. Raw command metadata and stdout/stderr are authoritative.

## Bandwidth

| Test | Paths | Bytes | Direction | Runs | Median Gb/s | Min–max Gb/s |
|---|---|---:|---|---:|---:|---:|
| bw | 1 | 4096 | bidir | 3 | 113.59 | 113.46–113.64 |
| bw | 1 | 4096 | forward | 3 | 104.03 | 103.97–104.05 |
| bw | 1 | 4096 | reverse | 3 | 13.89 | 13.63–14.10 |
| bw | 1 | 65536 | bidir | 3 | 118.42 | 118.40–118.42 |
| bw | 1 | 65536 | forward | 3 | 108.93 | 108.93–108.94 |
| bw | 1 | 65536 | reverse | 3 | 13.52 | 13.48–13.55 |
| bw | 1 | 1048576 | bidir | 3 | 118.53 | 118.53–118.54 |
| bw | 1 | 1048576 | forward | 3 | 109.12 | 109.11–109.12 |
| bw | 1 | 1048576 | reverse | 3 | 13.26 | 13.26–13.28 |
| bw | 1 + 2 | 4096 | bidir | 3 | 199.76 | 199.47–201.41 |
| bw | 1 + 2 | 4096 | forward | 3 | 183.86 | 183.85–183.87 |
| bw | 1 + 2 | 4096 | reverse | 3 | 27.53 | 27.36–27.76 |
| bw | 1 + 2 | 65536 | bidir | 3 | 204.29 | 204.26–204.35 |
| bw | 1 + 2 | 65536 | forward | 3 | 184.60 | 184.60–184.60 |
| bw | 1 + 2 | 65536 | reverse | 3 | 26.43 | 26.43–26.50 |
| bw | 1 + 2 | 1048576 | bidir | 3 | 204.26 | 204.26–204.28 |
| bw | 1 + 2 | 1048576 | forward | 3 | 184.74 | 184.74–184.74 |
| bw | 1 + 2 | 1048576 | reverse | 3 | 26.52 | 26.51–26.53 |
| bw | 2 | 4096 | bidir | 3 | 113.59 | 113.28–113.61 |
| bw | 2 | 4096 | forward | 3 | 104.01 | 103.96–104.13 |
| bw | 2 | 4096 | reverse | 3 | 14.18 | 13.94–14.26 |
| bw | 2 | 65536 | bidir | 3 | 118.30 | 118.27–118.43 |
| bw | 2 | 65536 | forward | 3 | 108.92 | 108.91–108.92 |
| bw | 2 | 65536 | reverse | 3 | 13.37 | 13.35–13.39 |
| bw | 2 | 1048576 | bidir | 3 | 118.40 | 118.36–118.40 |
| bw | 2 | 1048576 | forward | 3 | 109.12 | 109.12–109.13 |
| bw | 2 | 1048576 | reverse | 3 | 13.19 | 13.19–13.20 |
| stability | 1 + 2 | 65536 | bidir | 1 | 204.23 | 204.23–204.23 |

## Latency

The table reports the median across repetitions of perftest’s reported typical latency. This is a half-round-trip estimate; raw outputs retain minimum, maximum, average, deviation, and percentile columns.

| Path | Bytes | Direction | Runs | Median typical µs | Min–max typical µs |
|---|---:|---|---:|---:|---:|
| 1 | 8 | forward | 3 | 1.460 | 1.460–1.470 |
| 1 | 8 | reverse | 3 | 1.460 | 1.440–1.480 |
| 1 | 64 | forward | 3 | 1.460 | 1.460–1.470 |
| 1 | 64 | reverse | 3 | 1.450 | 1.450–1.460 |
| 1 | 4096 | forward | 3 | 3.510 | 3.490–3.590 |
| 1 | 4096 | reverse | 3 | 3.500 | 3.460–3.580 |
| 2 | 8 | forward | 3 | 1.480 | 1.470–1.490 |
| 2 | 8 | reverse | 3 | 1.470 | 1.460–1.490 |
| 2 | 64 | forward | 3 | 1.480 | 1.460–1.480 |
| 2 | 64 | reverse | 3 | 1.470 | 1.460–1.470 |
| 2 | 4096 | forward | 3 | 3.530 | 3.500–3.620 |
| 2 | 4096 | reverse | 3 | 3.630 | 3.540–3.700 |

## Evidence

- `matrix-results.csv`: individual primary endpoint results.
- `primary-completion-audit.json`: required cases and verification status.
- `matrix-v1-*.json`, `.stdout`, `.stderr`: exact benchmark commands and raw output.
- `agent-messages.jsonl`: coordination and readiness history.
- Discovery, baseline, diagnostic, and final-counter logs remain alongside the benchmark logs.


## Findings and verification

The RDMA connection works on both paths. Throughput is strongly asymmetric: for concurrent 64 KiB writes, the median was **184.60 Gb/s from spark-7e64 to spark-28a3**, **26.43 Gb/s in reverse**, and **204.29 Gb/s combined bidirectional**. The cause of the asymmetry was not established. CPU/load/PCIe context is retained; no tuning changes were made during the baseline.

The five-minute concurrent bidirectional stability case completed with both endpoints successful, **204.23 Gb/s combined average**, and **301.289 seconds** of overlapping client execution. Small-message typical latency medians were 1.45–1.48 µs; 4 KiB medians were 3.50–3.63 µs.

Coverage verified independently against the requested matrix: 81 bandwidth cases, 36 latency cases, one second-path smoke case, and one stability case. The initial successful first-path smoke test is retained separately. All 294 matrix endpoint command/result records and 28 concurrent client intervals passed audit.

Both peer archives were SHA-256 verified, then all 3,557 main-inventory files and 11 transfer-supplement files were independently checked with no mismatches. Raw archives, extracted snapshots, inventory files, and transfer receipts are retained.

## Counter changes

Across each recorded baseline-to-final interval, the only nonzero error-related deltas found were the following raw PHY lane counters. Other captured counters matching error/drop/discard/CRC/fault/link-down/FEC/pause names had zero deltas; no captured counters decreased. Do not sum these lane counters across interfaces or infer a cause of the throughput asymmetry from them. The intervals also include setup and control traffic.

| Host | Interface | rx_err_lane_0_phy delta | rx_err_lane_1_phy delta |
|---|---|---:|---:|
| spark-7e64 | enp1s0f1np1 | 354,745 | 347,987 |
| spark-7e64 | enP2p1s0f1np1 | 354,746 | 347,993 |
| spark-28a3 | enp1s0f1np1 | 206,922 | 10,859,931 |
| spark-28a3 | enP2p1s0f1np1 | 206,922 | 10,859,923 |

## Cleanup and evidence

Both benchmark-created NetworkManager profiles and their temporary IPv4 addresses/routes were removed. Original first-path addresses and Wi-Fi default routes were preserved. NetworkManager briefly resumed existing secondary-interface DHCP profiles; those existing profiles were not modified. No perftest processes or RDMA-CM test listeners remained after testing. Final coordination-service shutdown and both JOB_DONE acknowledgments are recorded in `FINAL_AUDIT.json` once complete.

Key verification files:

- `manifest-requirements-audit.json`: independent scope/combinations check.
- `paired-completion-audit.json`: both endpoints and concurrent timing.
- `peer-inventory-verification.json`: individual-file integrity.
- `primary-counter-comparison.json` and `peer-counter-comparison-verified.json`: complete counter deltas and raw source references.
- `peer-cleanup-verification.json` and `local-postrun-complete.json`: network cleanup evidence.
- `RUN_NOTES.md`: methodology, failed attempts, and configuration history.
- `imported-peer-spark-28a3-20261108T044117813137Z/spark-28a3/`: peer raw logs.
- `imported-peer-spark-28a3-20261108T044157394028Z/spark-28a3/`: transfer evidence supplement.
