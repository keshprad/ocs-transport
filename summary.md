# DGX Spark two-path RDMA results

Bulk transfers reached approximately **185 Gbit/s in each direction**. With traffic flowing in both directions simultaneously, the combined native averages reached approximately **370 Gbit/s total**. All **192 per-path measurements** and **72 combined bandwidth trials** completed, with no recorded counter alerts.

Completed: **November 9, 2026, 10:00 PM EST** (America/New_York).
Run: `20261110T021852Z-4cacd141-client`. Runner commit: `b17a867`; its source hash matches the recorded endpoints.

## Key findings

- At 1 MiB, unidirectional throughput was **185.17 Gbit/s client → server** and **185.18 Gbit/s server → client**.
- At 1 MiB with simultaneous traffic, each direction sustained **185.08 Gbit/s**, for **370.16 Gbit/s total**. This is approximately **92.5%** of the nominal 200 Gbit/s rate in each direction.
- At 1 GiB, unidirectional throughput remained **183.82 Gbit/s** in either direction. Bidirectional throughput fell to **151.18 Gbit/s per direction**, or **302.36 Gbit/s total**. The cause of this decrease has not been established.
- At 64 B, typical latency was **2.53 µs on path 0** and **2.26 µs on path 1**. These are half-round-trip ping-pong measurements.
- The 64 B bandwidth results are asymmetric: client → server throughput on path 1 is substantially higher than on path 0; reverse throughput is similar on both paths. This run does not establish the cause.

## Combined bandwidth

All values below are **Gbit/s**, shown as the median of three repetitions. Client is `spark-7e64`; server is `spark-28a3`. Unidirectional columns were measured in separate tests. Bidirectional columns were measured with both directions active.

| Message size per path | Unidir. client → server | Unidir. server → client | Bidir. client → server | Bidir. server → client | Bidir. total |
|---|---:|---:|---:|---:|---:|
| 64 B | 6.93 | 3.01 | 6.97 | 3.09 | 10.02 |
| 16 KiB | 185.00 | 185.00 | 183.76 | 183.78 | 367.52 |
| 1 MiB | 185.17 | 185.18 | 185.08 | 185.08 | 370.16 |
| 4 MiB | 185.18 | 185.18 | 185.10 | 185.10 | 370.20 |
| 16 MiB | 185.16 | 185.16 | 185.12 | 185.12 | 370.22 |
| 64 MiB | 185.12 | 185.12 | 185.12 | 185.12 | 370.22 |
| 256 MiB | 185.12 | 185.12 | 185.12 | 185.12 | 370.22 |
| 1 GiB | 183.82 | 183.82 | 151.18 | 151.18 | 302.36 |

For each bandwidth trial, per-path native averages are added before computing statistics across repetitions. Directional medians can therefore differ slightly from the median bidirectional total when added together. The displayed precision is inherited from native output; identical displayed repetitions do not establish zero variation.

## Per-path latency

Values are **µs**, shown as the median of perftest’s `t_typical` across three repetitions. Each path was measured separately with the other path idle. Latencies are never added together. At 1 GiB, approximately 81,600 µs is 81.6 ms and includes transfer time.

| Message size | Path 0 | Path 1 |
|---|---:|---:|
| 64 B | 2.53 | 2.26 |
| 16 KiB | 4.89 | 4.48 |
| 1 MiB | 81.14 | 80.84 |
| 4 MiB | 311.89 | 311.59 |
| 16 MiB | 1,235.51 | 1,234.92 |
| 64 MiB | 4,953.16 | 4,952.28 |
| 256 MiB | 19,799.53 | 19,798.02 |
| 1 GiB | 81,597.00 | 81,701.28 |

## Per-path bandwidth

These rates were measured while both paths were active. Values are median Gbit/s across three repetitions.

### Unidirectional

| Message size | Path 0 client → server | Path 0 server → client | Path 1 client → server | Path 1 server → client |
|---|---:|---:|---:|---:|
| 64 B | 1.47 | 1.51 | 5.46 | 1.50 |
| 16 KiB | 92.50 | 92.50 | 92.50 | 92.50 |
| 1 MiB | 92.58 | 92.59 | 92.59 | 92.59 |
| 4 MiB | 92.59 | 92.59 | 92.59 | 92.59 |
| 16 MiB | 92.58 | 92.58 | 92.58 | 92.58 |
| 64 MiB | 92.56 | 92.56 | 92.56 | 92.56 |
| 256 MiB | 92.56 | 92.56 | 92.56 | 92.56 |
| 1 GiB | 91.91 | 91.91 | 91.91 | 91.91 |

### Bidirectional

| Message size | Path 0 client → server | Path 0 server → client | Path 1 client → server | Path 1 server → client |
|---|---:|---:|---:|---:|
| 64 B | 1.43 | 1.54 | 5.54 | 1.55 |
| 16 KiB | 91.88 | 91.89 | 91.88 | 91.89 |
| 1 MiB | 92.54 | 92.54 | 92.54 | 92.54 |
| 4 MiB | 92.55 | 92.55 | 92.55 | 92.55 |
| 16 MiB | 92.56 | 92.56 | 92.56 | 92.56 |
| 64 MiB | 92.56 | 92.56 | 92.56 | 92.56 |
| 256 MiB | 92.56 | 92.56 | 92.56 | 92.56 |
| 1 GiB | 75.59 | 75.59 | 75.59 | 75.59 |

## Configuration and interpretation

| Setting | Recorded value |
|---|---|
| Client / server | `spark-7e64` / `spark-28a3` |
| Paths | Two logical RDMA paths over the connected QSFP port |
| Perftest | `ib_write_bw` and `ib_write_lat`, version 6.30 on both hosts |
| Transfer buffers | Host memory |
| Transport | Reliable Connected (RC) |
| Bandwidth queue configuration | One QP, transmit depth 128, inline size 0, native CQ moderation defaults |
| Ethernet / RDMA MTU | 1500 / 1024 bytes on both paths and hosts |
| Bandwidth timing | Native warm-up, then 20 seconds total: 5-second start margin, 10-second sample, 5-second end margin |
| Repetitions | Three for each size, mode, direction, and path |
| Execution | Detached systemd user services |

| Path | Interface / RDMA device | Client IP | Server IP |
|---|---|---|---|
| 0 | `enp1s0f1np1` / `rocep1s0f1` | `192.168.200.13` | `192.168.200.12` |
| 1 | `enP2p1s0f1np1` / `roceP2p1s0f1` | `192.168.201.13` | `192.168.201.12` |

Bandwidth trials wait for all selected listeners, then launch connectors in a batch and wait for every path to finish. Each native process owns its own sampling window. Combined results are a **sum of concurrent native averages**, rather than a measurement over an exactly shared time interval.
Client launch skew had a median of **1.465 ms** and maximum of **2.155 ms**; server launch skew had a median of **1.282 ms** and maximum of **2.161 ms**. These values describe process launch timing, not sampling-window alignment.

Message sizes are per path: a 1 GiB bandwidth trial sends 1 GiB messages on each active path. It does not split one 1 GiB message across the two paths. Latency below 1 MiB uses 10,000 measured exchanges; larger sizes use per-path calibration targeting 15 seconds, with a minimum of 100 exchanges. Each latency repetition has a separate warm-up invocation.

No counter alerts were recorded for either host or path in any mode.

## Comparison with the earlier single-path run

The earlier completed run used one logical RDMA path. The table shows the observed difference between the two runs; it is not a controlled experiment isolating every environmental variable.

| Measurement | Earlier single path | Current two paths | Ratio |
|---|---:|---:|---:|
| 1 MiB, client → server | 109.13 Gbit/s | 185.17 Gbit/s | 1.70× |
| 1 MiB, bidirectional total | 212.06 Gbit/s | 370.16 Gbit/s | 1.75× |
| 1 GiB, client → server | 108.23 Gbit/s | 183.82 Gbit/s | 1.70× |
| 1 GiB, bidirectional total | 180.39 Gbit/s | 302.36 Gbit/s | 1.68× |

## Source data

The completed two-path run's reports, CSV measurements, configuration, counters, and consolidated run records are committed alongside this summary. Individual per-process `local/` and `peer/` directories remain on the benchmark machine and are ignored by Git. Other runs, archives, and transient console logs are also excluded. The retained `runs.jsonl` includes native commands, output, and parsed statistics.

- [Generated report with repetition ranges](results/20261110T021852Z-4cacd141-client/summary.md)
- [192 per-path measurements](results/20261110T021852Z-4cacd141-client/results.csv)
- [72 combined bandwidth trials and launch timing](results/20261110T021852Z-4cacd141-client/bandwidth-combined.csv)
- [Completed trials, native commands, output, and parsed statistics](results/20261110T021852Z-4cacd141-client/runs.jsonl)
- [Negotiated test settings](results/20261110T021852Z-4cacd141-client/config.json)
- [Both hosts and both paths](results/20261110T021852Z-4cacd141-client/endpoints.json)
- [Completion status](results/20261110T021852Z-4cacd141-client/status.json)
- [Runner instructions](README.md)

The earlier single-path comparison uses run `20261110T005026Z-e9e74deb-client`, retained in the local archive. Its summarized values are reproduced in the comparison table above.
