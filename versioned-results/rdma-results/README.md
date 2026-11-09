# RDMA benchmark artifacts

Start with [RESULTS.md](RESULTS.md) for the complete measurements, interpretation, counter changes, and evidence links. [matrix-results.csv](matrix-results.csv) contains individual primary-endpoint results.

## Raw results

- Primary Spark (`spark-7e64`): timestamped discovery/setup/cleanup logs and `matrix-v1-*-spark-7e64-*` files directly in this directory. Each matrix job has `.json` command metadata, `.stdout`, and `.stderr`.
- Peer Spark (`spark-28a3`): [imported raw snapshot](imported-peer-spark-28a3-20261108T044117813137Z/spark-28a3/). Each matrix job has initial `.json`, `.json.completion.json`, `.stdout`, and `.stderr`.
- Peer transfer evidence: [supplement](imported-peer-spark-28a3-20261108T044157394028Z/spark-28a3/).
- Original peer archives remain as `peer-spark-28a3-*.tar.gz`, with receiver receipts and import manifests alongside them.
- `agent-messages.jsonl` preserves the shared coordination history. `completion-acknowledgments.json` extracts both agents' completion acknowledgments.

## Verification

- `FINAL_AUDIT.json`: final requirement-by-requirement completion gate.
- `manifest-requirements-audit.json`: all requested combinations and repetitions.
- `paired-completion-audit.json`: 294 endpoint records and 28 concurrent timing checks.
- `peer-inventory-verification.json`: 3,568 individual peer-file hashes.
- `primary-counter-comparison.json` and `peer-counter-comparison-verified.json`: raw-counter deltas and source paths.
- `RUN_NOTES.md`: methodology, failed attempts, temporary configuration, and cleanup protocol.

The initial path-1 smoke test is retained separately from the 119-case matrix. Failed discovery, configuration, handoff, and audit attempts are preserved; successful retries do not overwrite them.

Benchmark and coordination scripts are retained for reproducibility. Their temporary network profiles and services are cleaned up at completion, so running the scripts again requires a new coordinated setup.
