# DGX Spark RDMA benchmark results

This repository preserves a sanitized snapshot of the consolidated results from
spark-7e64 and spark-28a3. The original evidence remains locally in `rdma-results/`
and is excluded from Git.

- [Initial benchmark report](versioned-results/rdma-results/RESULTS.md): 119 cases, 294 endpoint records.
- [Directional split report](versioned-results/rdma-results/directional-split-20261108T222809Z-13438dc9/RESULTS.md): three concurrent bidirectional repetitions, 12 endpoint records.
- [Initial raw-log index](versioned-results/rdma-results/README.md).
- [Directional raw-log index](versioned-results/rdma-results/directional-split-20261108T222809Z-13438dc9/README.md).
- [Snapshot manifest](versioned-results/SNAPSHOT-MANIFEST.json): original and sanitized SHA-256 values for each included file, redaction indicators and excluded files.

## Physical topology clarification

“Path 1” (`rocep1s0f1` / `enp1s0f1np1`) and “Path 2”
(`roceP2p1s0f1` / `enP2p1s0f1np1`) are two logical RDMA interfaces
of the same physical QSFP port. Both used the single copper cable between the
Sparks. They are not two physical cables. The two logical interfaces correspond
to the NIC's two PCIe Gen 5 x4 connections to the processor.
See [NVIDIA's port mapping](https://docs.nvidia.com/dgx/dgx-spark/spark-clustering.html#full-correspondence-table).

The measured median bidirectional split was 184.54 Gb/s from spark-7e64 to
spark-28a3 and 19.78 Gb/s in reverse, totaling 204.32 Gb/s. Incoming rates in
perftest's report are exchanged peer TX reports, not independently measured RX
rates. The cause of the directional asymmetry remains unproven.

## Redaction and integrity

Mailbox credentials were replaced with `[REDACTED_MAILBOX_TOKEN]` wherever they
occurred, including embedded request logs. Standalone key files, original compressed
archives and Python caches are excluded. Extracted peer logs from both runs are
included. Original archives and untouched evidence remain in the local source directory.

Historical inventories, archive hashes and audit files describe the original evidence
as verified during the runs. Redacted files intentionally no longer match those
original hashes. Use `SNAPSHOT-MANIFEST.json` to verify this committed snapshot.
Raw logs may retain original absolute paths; use the relative indexes above to browse
this copy. Historical services and temporary profiles were cleaned up; scripts are
retained as evidence, not as an instruction to restart them.

`tools/export_results.py` creates a fresh snapshot and refuses to overwrite one.
