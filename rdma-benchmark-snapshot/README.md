# Sanitized RDMA benchmark evidence — spark-28a3

This is a separate Git snapshot of the locally available evidence in `../rdma-results`. The original evidence remains unchanged and is excluded from Git. Reports, raw measurement streams, exact command metadata, timestamps, exit codes, interface/RDMA counters, scripts, protocol records, completion proofs and audits are included under `evidence/`.

## Physical topology

**The two tested paths are logical RDMA interfaces sharing one physical direct-attach copper connection between the Sparks. They are not two independent physical copper links.**

| Logical path | RDMA device | Ethernet interface |
|---|---|---|
| 1 | `rocep1s0f1` | `enp1s0f1np1` |
| 2 | `roceP2p1s0f1` | `enP2p1s0f1np1` |

“Concurrent” means two benchmark processes using these logical paths at once. Earlier evidence may use informal wording such as “two cable paths”; the topology clarification above takes precedence. Historical evidence is retained as recorded, apart from credential redaction. Aggregate rates must not be described as the capacity of two independent cables.

## Measurements

The initial matrix contains 119 cases: 81 bandwidth cases, 36 latency cases, one second-path smoke test and one five-minute stability case. The follow-up directional run contains three 30-second repetitions with 64 KiB messages on both logical paths concurrently, using bidirectional RDMA WRITE and `--report-both`.

| Follow-up median, aggregate over the two logical paths | Gb/s |
|---|---:|
| `spark-7e64` → `spark-28a3` | 184.54 |
| `spark-28a3` → `spark-7e64` | 19.78 |
| Combined bidirectional | 204.32 |

The directional rows were cross-checked against the opposite endpoint. Perftest's remote row is the exchanged peer transmit report, not an independent receive measurement. The repeated directional asymmetry and increases in PHY lane counters are observations; these runs do not establish their cause. Default affinity/tuning and Ethernet MTU 1500 / RDMA MTU 1024 were retained. Temporary NetworkManager profiles were removed after testing.

Detailed initial results: `evidence/FINAL-RESULTS.txt` and `evidence/local-summary-20261108T043933Z.txt`. Detailed directional results: `evidence/directional-split-20261108T222809Z-13438dc9/RESULTS.txt`. Completion proofs, raw outputs and counter deltas are alongside those reports.

## Extracted peer evidence and limits

`extracted-peer/spark-7e64/` contains deduplicated peer mailbox records, including completion/exit records, directional raw measurement rows, audits and coordination messages. Embedded output contents are extracted when actually present; a remote filename is retained as a reference and is never treated as file contents.

The full original peer archives were consolidated on `spark-7e64` during testing and are **not present on this Spark**. This local commit therefore includes the peer records recoverable from this Spark's mailbox evidence, not a complete second copy of the other host's original streams. The provenance and hash of each source mailbox file are recorded in the manifest.

## Sanitization and provenance

`manifest.json` records original and sanitized SHA-256 hashes and sizes for each exported source file, plus original hashes and reasons for exclusions. Derived peer files have payload hashes and their source-message/source-file provenance. Unchanged files have identical original and sanitized hashes. Historical hashes inside evidence remain historical claims; use the manifest for the sanitized snapshot's actual hashes.

Mailbox token values are replaced with `REDACTED_MAILBOX_TOKEN` everywhere detected in text, including scripts and nested JSON. Embedded zlib/base64 runner payloads are inspected for credentials. Standalone key files, original credential-bearing archives and Python caches are excluded. Credential fingerprints are hashes, not token values. `sanitization-audit.json` records verification counts; the manifest does not hash itself. Git protects the manifest, this documentation and the sanitizer script.

The sanitizer rechecks every original file hash after export and verifies every sanitized/derived payload hash. It refuses to overwrite an existing payload. To reproduce into a fresh directory from the preserved originals:

```sh
python3 rdma-benchmark-snapshot/tools/sanitize.py --source rdma-results --output /tmp/rdma-snapshot-reproduction
```

This repository is local only. Nothing was pushed.
