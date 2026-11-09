# Run notes

## Scope and interpretation

- Host-memory RDMA using perftest; no GPU-memory or NCCL benchmark was requested in this run.
- Path 1: `rocep1s0f1` / `enp1s0f1np1`, existing IPv4 addresses `169.254.135.137` (spark-7e64) and `169.254.238.243` (spark-28a3).
- Path 2: `roceP2p1s0f1` / `enP2p1s0f1np1`, temporary IPv4 addresses `192.168.201.12` and `192.168.201.13` respectively.
- Ethernet MTU 1500 and RDMA MTU 1024 were retained. No affinity, CPU governor, queue-depth, or NIC tuning changes were made during the baseline matrix.
- Concurrent-path cases launch two independent perftest processes; their client execution intervals are audited before reporting summed throughput. Do not sum results from separate sequential cases or duplicate results from both hosts.
- Bidirectional bandwidth is combined throughput across both directions. Latency is perftest's half-round-trip estimate; forward/reverse labels identify which host is the client, not a synchronized one-way measurement.
- The deterministic matrix contains 119 cases and 147 endpoint jobs per host. An initial successful path-1 smoke test predates this matrix; the matrix includes a path-2 smoke test.

## Coordination and logging

Native Codex cross-host messaging returned `AppServerManager RPC is unavailable` in both directions. Agents instead coordinate through an authenticated, temporary mailbox bound to the primary Spark's copper-link address on port 12080. The mailbox does not execute messages. Each agent interprets assignments within the user-authorized plan.

Both sides agree on the manifest digest, then use per-test readiness and completion barriers. The peer records explicit assignment acknowledgments. Control traffic uses the same copper connection; bulk log transfers are deferred until measurements stop. Raw stdout, stderr, command arrays, hostnames, UTC timestamps, and exit statuses are retained. Peer initial metadata is preserved separately from its completion metadata.

## Configuration events and failed attempts

Initial sandbox-restricted discovery failed to see some network/RDMA state. Host-level retries succeeded; both attempts remain saved.

Both Sparks initially started smoke-test servers. The primary's redundant server was deliberately terminated after verifying its process identity. The primary then connected as client to the peer server and measured 108.93 Gb/s with a successful exit.

Direct temporary address additions required the user's sudo password. The user added both path-2 addresses and agents verified them. Later, both addresses disappeared while NetworkManager managed the disconnected secondary interfaces. The precise cause is not proven because privileged NetworkManager journal records were unavailable. The phase handoff detected the missing direct route and failed its connectivity check before launching path-2 benchmarks.

Both agents then created unsaved NetworkManager profiles with autoconnect disabled, no default route, manual IPv4 addresses, IPv6 disabled, and MTU 1500. Both direct routes and pings passed. These profiles are temporary and must be deleted during cleanup:

- spark-7e64: `rdma-benchmark-temp-20261108`, UUID `f0cbbd7c-f1b6-4285-9ff1-f397645e6b7d`.
- spark-28a3: `rdma-benchmark-path2-28a3`, UUID `2ebec186-c595-400c-a716-7ab7af5ca36e`.

## Counter interpretation

Discovery already showed historical PHY counters. Baseline-to-final differences include setup and coordination within the recorded interval, not exclusively benchmark traffic. Report counter names and deltas directly; do not infer packet loss or a cause of directional throughput differences from PHY lane counts alone. Separate phase-2 baseline snapshots are also retained after the temporary profiles were activated.

## Completion gate

The result remains incomplete until both hosts' endpoint records and concurrent timing are audited, peer archive integrity is verified, counter changes are summarized, temporary profiles and benchmark processes are cleaned up, and both agents post LOCAL_COMPLETE followed by JOB_DONE. Coordination services are then shut down and their final logs retained.
