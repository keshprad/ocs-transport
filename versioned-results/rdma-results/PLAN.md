# RDMA connection test plan

Workspace: `/home/keshav/Documents/ocs-transport`

All raw command outputs and benchmark results from both Sparks will be saved in this single directory: `/home/keshav/Documents/ocs-transport/rdma-results`.

## Starting context

The previous inspection of `spark-7e64` found two active RDMA devices: `rocep1s0f1` (network interface `enp1s0f1np1`) and `roceP2p1s0f1` (network interface `enP2p1s0f1np1`). Both reported 200 Gb/s links, and `perftest` was installed. These observations must be rechecked. No configuration changes or traffic tests have been performed as part of this plan.

Access to the second Spark and its matching interfaces still needs to be established.

## Test sequence

| Step | Action | Purpose |
|---|---|---|
| 1. Inspect both Sparks | Record interface mappings, IP addresses, routes, link speeds, MTUs, RDMA state, and perftest versions. Identify the peer's reachable address. | Identify devices and paths to test. |
| 2. Establish connectivity | Choose unused subnets and add temporary test IPs where needed. Verify routing and ping over each intended interface. | Confirm each test uses the copper connection. |
| 3. Run an RDMA smoke test | Run a short `ib_write_bw` server/client pair on each path. | Confirm RDMA connections and data transfers work. |
| 4. Measure bandwidth | Test each path individually, then both concurrently. Use 4 KiB, 64 KiB, and 1 MiB messages, with 30-second runs. | Measure sustained throughput and aggregate capacity. |
| 5. Test directionality | Swap client/server, then run separate bidirectional tests. | Check performance differences by direction. |
| 6. Measure latency | Run `ib_write_lat` with 8 B, 64 B, and 4 KiB messages while bandwidth tests are idle. | Measure small-message latency and variability. |
| 7. Check repeatability and stability | Repeat performance cases three times; run a five-minute sustained transfer. Capture interface error counters before and after. | Check consistency, failures, and link errors. |
| 8. Summarize | Save raw logs and a table of bandwidth, latency, run-to-run range, and counter changes. | Produce a reproducible baseline. |

## Raw-output capture requirements

- Save all discovery, configuration, and benchmark command outputs from both Sparks in this directory.
- Capture stdout, stderr, exact commands, timestamps, hostnames, and exit codes.
- Preserve failed attempts as well as successful runs; do not overwrite earlier results.
- Use filenames that identify the host, test, direction, message size, and repetition where applicable.
- Preserve the raw logs alongside the final summary so every reported result can be traced to its source.

## Measurement scope and interpretation

- Begin with host-memory RDMA. GPU communication and NCCL tests are a possible follow-up for distributed inference or training.
- Report measured throughput separately from advertised link speed.
- Sum concurrent-path bandwidth only from tests running together.
- Label `ib_write_lat` latency as the tool's half-round-trip estimate.
- Keep other workloads idle during baseline measurements and use compatible perftest versions and matching options on both ends.
- Record temporary configuration changes and restore the original configuration after testing.

## Next action

Perform read-only discovery on both Sparks before configuring addresses or generating benchmark traffic.
