# Concurrent bidirectional RDMA directional split

Status: COMPLETE

Host-memory RDMA writes; both paths concurrent; 65,536-byte messages; 30-second duration; three repetitions; Ethernet MTU 1500, RDMA MTU 1024; one QP per path; default affinity/tuning. Both endpoints used `-b --report-both --report_gbits`.

## Aggregate rates (Gb/s)

| Repetition | spark-7e64 → spark-28a3 | spark-28a3 → spark-7e64 | Sum of displayed directions | Reported combined |
|---|---:|---:|---:|---:|
| 1 | 184.54 | 19.78 | 204.32 | 204.32 |
| 2 | 184.54 | 20.00 | 204.54 | 204.55 |
| 3 | 184.54 | 19.72 | 204.26 | 204.26 |
| Median | 184.54 | 19.78 | 204.32 | 204.32 |

## Each path (Gb/s)

| Rep | Path | 7e64 TX / 28a3 RX | 7e64 RX / 28a3 TX | Combined |
|---|---|---:|---:|---:|
| 1 | 1 | 92.27 | 9.89 | 102.16 |
| 1 | 2 | 92.27 | 9.89 | 102.16 |
| 2 | 1 | 92.27 | 9.95 | 102.23 |
| 2 | 2 | 92.27 | 10.05 | 102.32 |
| 3 | 1 | 92.27 | 9.86 | 102.13 |
| 3 | 2 | 92.27 | 9.86 | 102.13 |

## Interpretation and verification

A 0.01 Gb/s difference between displayed directional sums and combined reports can arise from perftest rounding. The directional rates come from these bidirectional runs, not earlier one-way tests. Perftest labels `Local results` as this endpoint’s outgoing RDMA-write report and `Remote results` as the exchanged peer outgoing report. Thus RX denotes peer-reported incoming throughput, not an independent receive-side hardware measurement. Opposite endpoint rows, including iteration counts and message rates, must match exactly; the combined row must equal the directional sum within printed rounding.

Source: [perftest 24.01.0-0.38 write_bw.c](https://github.com/linux-rdma/perftest/blob/24.01.0-0.38/src/write_bw.c#L417-L437) and matching saved source outputs. `print_full_bw_report` adds both reports for the combined row.

The prior ~204.29 Gb/s combined result is a separate measurement; this run measures its directional split under the same settings. Directional asymmetry remains observable; this run does not establish its cause.

Raw primary endpoint logs are `*-rN-pN-client-*`; peer raw logs are in imported archive directories. `protocol-proposal.json`, `agent-messages.jsonl`, per-repetition acceptance files, baseline/final counters, archive receipts, inventory checks and FINAL_AUDIT.json provide the audit trail.

## Counter changes

| Host | Interface | PHY lane 0 error delta | PHY lane 1 error delta |
|---|---|---:|---:|
| spark-7e64 | enp1s0f1np1 | 27,086 | 29,819 |
| spark-7e64 | enP2p1s0f1np1 | 27,086 | 29,825 |
| spark-28a3 | enp1s0f1np1 | 21,238 | 1,774,373 |
| spark-28a3 | enP2p1s0f1np1 | 21,238 | 1,774,400 |

Other captured error/drop/discard/CRC/fault/link-down/FEC/pause counters did not increase. Raw RDMA counters and deltas are preserved. These intervals include setup/control traffic; PHY lane counts are not proof of packet loss or the cause of asymmetry, and should not be added across interfaces.

## Completion evidence

Both peer archives and all 242 listed file hashes verified. All 12 endpoint commands, exit codes, directional rows, and three overlapping execution intervals audited. Both benchmark-created unsaved profiles were removed; original first-path addresses and default routes preserved. `measurement-audit.json` contains the measurement checks; `FINAL_AUDIT.json` records final handshake and service shutdown.
