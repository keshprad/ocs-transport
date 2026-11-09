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
