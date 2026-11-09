# Directional split run

Start with [RESULTS.md](RESULTS.md) and [directional-results.csv](directional-results.csv).

- Primary raw measurements: `*-r1-p1-client-*` and corresponding r1–r3, p1–p2 files, with exact command, hostname, UTC timestamps, exit code, stdout and stderr.
- Peer raw measurements: `imported-peer-spark-28a3-20261108T223754596981Z/spark-28a3/`.
- Transfer receipts/evidence: `imported-peer-spark-28a3-20261108T223810161735Z/spark-28a3/`, original archives and their `.receipt.json`/`.import.json` files.
- `protocol-proposal.json`: exact planned commands, ports, settings and sequencing.
- `agent-messages.jsonl`: handshake, assignment/ACK/READY/results/DONE/acceptance, and completion exchange.
- `measurement-audit.json`: endpoint commands, reciprocal rows, concurrency, file inventory and cleanup verification.
- `local-counter-deltas.json` and `peer-counter-deltas-verified.json`: raw before/after source references and all deltas.
- `FINAL_AUDIT.json`: final completion gate including service shutdown and both JOB_DONE.
- `RUN.json`: run ID, benchmark-created profile UUID and processed IDs preventing duplicate execution.

All earlier benchmark results remain unchanged. Failed attempts, including the initial source-tag lookup, are preserved. RX labels mean exchanged peer TX reports, not independent receive-side measurements. Exact scripts are retained for reproducibility; rerunning requires a fresh coordination agreement and setup.
