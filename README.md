# DGX Spark RDMA baseline

`rdma_bench.py` coordinates `ib_write_bw` and `ib_write_lat` on two Linux hosts.
Start one Python process on each Spark; the runner handles the test schedule,
listener readiness, role reversal, warm-ups, calibration, timeouts, and reporting.
It uses no SSH and needs no synchronized clocks.

Python only orchestrates. Compiled perftest C binaries issue the RDMA operations
and measure bandwidth/latency. Process elapsed time is saved for diagnostics and
is **never** used as a performance measurement. No C++/CUDA build is required.

This is a **host-memory RDMA** baseline. DGX Spark's unified physical memory does
not make every CUDA allocation suitable for NIC access. GPU-buffer compatibility
and GPU/application synchronization are separate experiments. See
[NVIDIA's DGX Spark RDMA guidance](https://nvidia.custhelp.com/app/answers/detail/a_id/5780).

## Prerequisites

On both machines:

- Python 3.10 or newer, `ip` (iproute2), `ibv_devinfo` (ibverbs-utils), and perftest
  binaries on `PATH`. There are no Python package dependencies.
- An active Ethernet RDMA interface with an IPv4 address and a matching RoCEv2
  GID. Each host may have a different interface, device, or GID index.
- Matching perftest versions and the **same copy** of `rdma_bench.py` on both
  hosts. Use the current stable release **26.07.8**, which reports tool version
  **6.30**. The older Ubuntu package reports `6.20` and has a native warm-up hang;
  the runner rejects versions older than the upstream fix in `6.26`.
- Permission to open the RDMA device and register the largest selected buffers.
  The runner records the locked-memory limit; native registration failures are
  reported with logs. It does not change resource limits or network settings.
- TCP connectivity on coordination port **18516** and perftest port **18515** in
  both directions, plus the existing RoCE data path. The scripts bind source
  addresses and verify routes through the selected interface. `/proc` process
  socket information must be readable for listener readiness checks.

The runner expects a trusted two-host test network. It does not install packages,
configure the link, or change MTUs, drivers, routing, or congestion settings.

## Build current perftest

On **both hosts**, from this updated checkout:

```bash
sudo apt-get install build-essential curl pkg-config \
  libibverbs-dev librdmacm-dev libibumad-dev libpci-dev
bash tools/build_perftest.sh
```

The helper pins the official [26.07.8 release](https://github.com/linux-rdma/perftest/releases/tag/26.07.8)
(latest stable checked November 9, 2026) and verifies the release archive's
SHA-256. It builds the two WRITE tools in `.perftest/bin/`, which the runner
selects automatically before searching `PATH`. The build itself needs no sudo
and does not replace `/usr/bin` binaries. Logs and release metadata are saved
under `.perftest/`. Build independently on each host; both must use matching
versions. `PERFTEST_BUILD_JOBS` optionally controls build parallelism (default 2).

This release includes the [upstream native warm-up fix](https://github.com/linux-rdma/perftest/commit/766c13bdb5169efc38e1e5da8180ce6407f7379e):
request completions for warm-up writes, then restore the original send flags
before measurement. The runner keeps `--perform_warm_up` enabled, including
smoke tests, without forcing different CQ moderation or a different posting API.

## Start the benchmark

The current two-path setup uses one QSFP cable with two logical PCIe paths:

| Path | Interface | Server (`spark-28a3`) | Client (`spark-7e64`) |
|---|---|---|---|
| 0 | `enp1s0f1np1` | `192.168.200.12/24` | `192.168.200.13/24` |
| 1 | `enP2p1s0f1np1` | `192.168.201.12/24` | `192.168.201.13/24` |

Spark exposes two independent PCIe Gen 5 x4 paths to its NIC. Using only one
logical RDMA device does not exercise the whole physical port. Both selected
paths carry bandwidth traffic concurrently. See [NVIDIA's port mapping](https://docs.nvidia.com/dgx/dgx-spark/spark-clustering.html#full-correspondence-table).
Keep the same path order on both hosts; peer addresses are exchanged over the
primary path's control connection. The runner discovers each device and GID.

Pull the same updated checkout on both machines. The new control protocol is
incompatible with older runner copies. Build perftest as described above.

Long runs should use user services so terminal closure cannot stop them. Once
on each machine, enable services to survive logout:

```bash
sudo loginctl enable-linger "$USER"
```

On the **server**, from the checkout:

```bash
bash tools/run_bench_service.sh server \
  --interface enp1s0f1np1 --ip 192.168.200.12 \
  --path enP2p1s0f1np1=192.168.201.12 --connect-timeout 600
```

Then on the **client/controller**:

```bash
bash tools/run_bench_service.sh client 192.168.200.12 \
  --interface enp1s0f1np1 --ip 192.168.200.13 \
  --path enP2p1s0f1np1=192.168.201.13 --all
```

The helper returns immediately and appends output to
`results/service-server.log` or `results/service-client.log`. Watch with
`tail -f results/service-client.log`; closing the viewer leaves the benchmark
running. Stop explicitly with `systemctl --user stop ocs-rdma-client` (or
`ocs-rdma-server` on the server). A failed launch does not replace an existing
service. Finished units are collected; the saved logs and `status.json` retain
the outcome.

Omit `--path` on both machines to retain the original single-path baseline.
The helper passes its arguments to `python3 rdma_bench.py`; direct foreground
invocation remains available for short diagnostics.

Choose any combination of `--unidirectional-write-bw`,
`--bidirectional-write-bw`, and `--write-latency` instead of `--all`.
The client selects test settings. Local interface/IP settings and output
directories belong on each host. The control port must match on both hosts;
native perftest uses the base port plus twice the path index (18515 and 18517
by default). The control port must differ from all native ports.

For every bandwidth trial, all selected native listeners must be ready before
any connector starts. Writers then launch in one batch, and both paths must
finish before the next trial. Reverse unidirectional trials swap native roles
on both paths. No Python clock is used to calculate throughput.

Each perftest process still owns its own duration timer and sampling window.
Combined throughput is explicitly the sum of concurrent native averages, not a
measurement over an exactly shared sampling interval. Per-host launch skew is
retained for review; it does not measure warm-up or sampling-window skew.

Latency runs one path at a time with the other path idle, with separate
calibration and warm-up for each size/path. Latency is reported per path and
never added together. Each process still sends the full requested message
size; sizes are per path, not a striped total divided between them.

### Preview without benchmarking

Run on either machine with the appropriate role:

```bash
python3 rdma_bench.py --client 192.168.200.12 --all \
  --ip 192.168.200.13 --path enP2p1s0f1np1=192.168.201.13 --dry-run
python3 rdma_bench.py --server --ip 192.168.200.12 \
  --path enP2p1s0f1np1=192.168.201.12 --dry-run
```

Dry-run inspects the local device and tool capabilities, prints commands and run
counts, and creates no results or network connections. Server dry-run previews
all modes by default; the actual server follows the client's selection. Peer
discovery, route checks, and cross-host version checks happen in a real run.
Large latency `-n` values are placeholders until calibration.

For a shorter first end-to-end check, start the server normally, then run:

```bash
bash tools/run_bench_service.sh client 192.168.200.12 --all \
  --interface enp1s0f1np1 --ip 192.168.200.13 \
  --path enP2p1s0f1np1=192.168.201.13 \
  --sizes 64B 1MiB --repetitions 1 --duration 5
```

With two paths this performs 16 measured path runs and six combined bandwidth
trials, plus smoke tests, calibration, and warm-ups.
After native warm-up, each five-second bandwidth run excludes a one-second
start margin, measures for three seconds, and excludes a final one-second margin.
It validates orchestration but does not establish support for the full 1 GiB
sweep. Restart both scripts for the full suite afterward.

### Discovery overrides

The runner selects the IPv4-mapped RoCEv2 GID belonging to the chosen interface
and address. It fails on missing or ambiguous matches. Overrides must still
match that interface/address:

```bash
python3 rdma_bench.py --server --interface enp1s0f1np1 \
  --ip 192.168.200.12 --device rocep1s0f1 --rdma-port 1 --gid-index 3
```

The local discovery currently resolves to `rocep1s0f1`, port 1, GID 3. Do not
assume those identifiers on the other Spark. `ip -4 address`, `ibdev2netdev`, and
`ibv_devinfo` can help diagnose setup issues.

## Test matrix

Every mode uses these sizes:

| Message size | Unidirectional | Bidirectional | Latency |
|---|---|---|---|
| 64 B | Both directions sequentially | Simultaneous | Ping-pong |
| 16 KiB | Both directions sequentially | Simultaneous | Ping-pong |
| 1 MiB | Both directions sequentially | Simultaneous | Ping-pong |
| 4 MiB | Both directions sequentially | Simultaneous | Ping-pong |
| 16 MiB | Both directions sequentially | Simultaneous | Ping-pong |
| 64 MiB | Both directions sequentially | Simultaneous | Ping-pong |
| 256 MiB | Both directions sequentially | Simultaneous | Ping-pong |
| 1 GiB | Both directions sequentially | Simultaneous | Ping-pong |

Three repetitions on each path give **96 measured path runs**: 48
unidirectional, 24 bidirectional, and 24 latency. Two paths therefore produce
**192 per-path CSV rows** (96 unidirectional, 48 bidirectional, 48 latency), plus
**72 combined bandwidth rows** (48 unidirectional and 24 bidirectional).
Modes run in that order regardless of flag order.
Keep other traffic and GPU/CPU workloads idle during measurements.

### Bandwidth

- RC, one QP, transmit depth 128, inline size zero, explicit size/device/port/GID.
- `--perform_warm_up -D 20 -f 5 --report_gbits -N`: native warm-up, then five
  seconds of start-margin traffic, ten seconds measured, and five seconds of
  end-margin traffic. Both margins are excluded from the result.
  `--duration` sets the total run length; each margin is `floor(duration / 4)`
  seconds, explicitly matching perftest 6.20's duration default. Peak calculation
  is disabled; report **average Gbit/s**.
- Native warm-up uses the same QP and buffers as measurement. Smoke tests run
  native warm-up followed by 100 exchanges; their measurements are excluded
  from the report.
- Bidirectional output uses `--report-both`; parse aggregate, local, and remote
  rows separately. Never divide aggregate throughput by two to infer directions.
- Reports use script client/server identities consistently, even as native roles
  swap. A reported 200 Gb/s physical link is a nominal reference, not a required
  payload throughput. Bidirectional aggregate includes both directions.

### Latency

- `ib_write_lat`, RC, inline size zero, fixed native client/server orientation.
- Below 1 MiB (64 B and 16 KiB in the default sweep): 1,000 separate warm-up
  exchanges followed by 10,000 measured exchanges per repetition.
- At 1 MiB and larger: first calibrate with 100 exchanges. Choose
  `max(100, ceil(15,000,000 / (2 × reported_average_us)))` measured exchanges,
  targeting 15 seconds of native ping-pong execution. Reuse this count across
  repetitions. Each repetition has a separate warm-up of
  `max(10, ceil(measured_count × 0.1))` exchanges.
- Warm-ups are separate native invocations; they exercise the system path but
  do not retain the next invocation's allocated buffers. Bandwidth's native
  warm-up runs within its measured invocation and retains its buffers.
- Preserve native min/max, typical, average, standard deviation, p99, and p99.9.
  The summary uses **t_typical** (perftest's median) and takes its median across
  repetitions. It does not merge all latency samples into one distribution.
- WRITE latency is reported as **half round-trip ping-pong time**. At large
  sizes it includes serialized message transfer; it is not independently
  measured one-way completion, GPU-visible completion, or latency under load.

Before measurements, each selected mode smoke-tests its smallest and largest
sizes with 100 exchanges. Unsupported sizes, changed sizes/iteration counts,
unrecognized output, or native failures abort the suite rather than silently
omitting cases. The full sweep will take tens of minutes; startup, allocation,
registration, built-in warm-ups, and calibration add to measurement time.

## Results and failures

Each host prints its unique directory under `results/` (override with
`--output-dir`). The **client** directory contains the combined report:

| File | Contents |
|---|---|
| `summary.md` | Combined bandwidth and per-path bandwidth/latency tables with median and min–max |
| `results.csv` | One row per measured path run, including `path_index`, in Gbit/s and µs |
| `bandwidth-combined.csv` | One row per complete bandwidth trial, summed across paths, with per-host launch skew |
| `runs.jsonl` | Completed grouped trials; client/server records are under `paths` keyed by index |
| `config.json`, `endpoints.json` | Negotiated configuration and both hosts' metadata |
| `status.json` | Running, complete, or failed; error and measured-run count |
| `local/path-N/`, `peer/path-N/` | Per-path native logs, parsed statistics, PIDs, and exact command arguments |
| `counters-*.json`, `counter-changes-*.json` | Before/after snapshots, deltas, and error/drop/reset observations |

A failed path aborts its siblings and the peer. An incomplete bandwidth trial
adds neither per-path nor combined CSV rows; earlier complete trials remain.
Per-path rates are summed within each repetition before summary statistics are
computed, rather than summing medians from different repetitions.

The server keeps its own native logs and local counter snapshots. Counter
observations do not erase measurements or impose an unestablished performance
threshold. Missing counters are recorded explicitly. A successful process exit
means the suite completed, not that throughput meets a target.

Ctrl-C or SIGTERM on either script aborts the other endpoint and terminates
owned native process groups. Native failures, disconnects, readiness failures,
and execution timeouts also stop the suite. Partial logs and completed CSV rows
remain available. There is no automatic retry/resume that might hide failures;
restart both scripts for a new run.

Timeout controls:

- `--connect-timeout 300`: local wait for initial peer connection.
- `--startup-timeout 30`: client-negotiated native listener readiness timeout.
- `--test-timeout 180`: client-negotiated timeout per native invocation, including
  allocation and registration. Increase it if large buffers need longer startup.

## Development checks

```bash
python3 -m unittest discover -s tests -v
```

Tests use temporary fake perftest executables and real local sockets. They check
the full single-path and 192-run dual-path matrices, output parsing and units,
calibration, direction mapping, listener readiness barriers across both paths,
readiness without probe connections, partial results, timeouts, and SIGTERM
cleanup of all four native processes. They also check incomplete-path failures,
per-trial aggregation, isolated per-path latency, native warm-up, and releases without
the warm-up fix. **They do not measure or certify RDMA hardware performance.**

The 6.30 build was validated on `spark-7e64` with hardware discovery, a dry-run,
and six native RDMA loopback checks: 64 B/1 MiB WRITE smoke tests, five-second
64 B unidirectional/bidirectional bandwidth runs, and 64 B/1 MiB latency tests.
Native bandwidth warm-up stayed enabled throughout. These are functional checks
on one host, not two-host performance results. Real two-Spark validation requires
launching the scripts on both machines as described above.

Native output semantics were checked against
[the selected release's upstream source](https://github.com/linux-rdma/perftest/tree/26.07.8/src),
particularly `write_bw.c`, `perftest_parameters.c`, and
`perftest_communication.c`.
