"""Recompute the three- and four-Spark memfd ring measurements using the archived collector and raw logs."""

import argparse
import json
import math
from pathlib import Path
import re
from statistics import median
import subprocess
import sys
import tarfile
import tempfile


RINGS = Path(__file__).resolve().parent
FOUR_SPARK = RINGS / "2026-11-28/full-duplex-four-spark-sg"
THREE_SPARK = RINGS / "2026-11-27/full-duplex-sg-per-peer"


def read_json(path):
    return json.loads(path.read_text())


def check(equal, message):
    if not equal:
        raise ValueError(message)


def unpack(archive, target):
    with tarfile.open(archive) as source:
        source.extractall(target, filter="data")


def replay_trial(controller, paths, raw, number, gates):
    endpoints = {}
    starts, ends, durations = [], [], []
    for tx, rx in paths:
        for endpoint in (tx, rx):
            log = controller.ring.log_path(raw, number, endpoint)
            lines = log.read_text().splitlines()
            fields = controller.bw.parse_result(endpoint, lines)
            device, = [line.removeprefix("DPDK port 0: ") for line in lines
                       if line.startswith("DPDK port 0: ")]
            if endpoint.role == "rx":
                parent, = [re.fullmatch(r"PEER_PCIE parent=(\S+) vf=(\S+) interface=\S+", line)
                           for line in lines if line.startswith("PEER_PCIE ")]
                check(parent and parent.groups() == (endpoint.pci, device), f"{log}: RX VF mismatch")
            else:
                check(device == endpoint.pci, f"{log}: TX PF mismatch")
            endpoints[endpoint] = dict(fields, device=device, physical_function=endpoint.pci,
                                       cpu_ids=list(endpoint.cpus))

        sender = endpoints[tx]
        gate = gates[tx.host]
        offset = int(sender["start_ns"]) - gate["sender_gate_ns"] + controller.ring.GATE_LEAD_NS
        start = [gate[key] + offset for key in ("controller_sent_ns", "controller_received_ns")]
        duration = int(sender["elapsed_ns"])
        starts.append(start)
        ends.append([bound + duration for bound in start])
        durations.append(duration)

    trial = controller.ring.summarize(gates, endpoints, paths)
    shortest = max(max(durations), max(lo for lo, hi in ends) - min(hi for lo, hi in starts))
    longest = max(hi for lo, hi in ends) - min(lo for lo, hi in starts)
    trial["aggregate_payload_gbps_bounds"] = [8 * trial["verified_payload_bytes"] / ns
                                              for ns in (longest, shortest)]
    return trial


def replay_four(controller, raw, report, work, controller_dir, expected):
    trials = []
    for recorded in report["trials"]:
        number = recorded["attempt"]
        gates = read_json(raw / f"{number}-gate-timing.json")
        trial = replay_trial(controller, controller.edges(), raw, number, gates)
        trial["attempt"] = number
        check(trial == recorded, f"Trial {number}: endpoint or timing data differs from results.json")
        trials.append(trial)
        for host in gates:
            filename = f"{number}-{host}-gate.log"
            (work / filename).symlink_to(raw / filename)

    check(bool(trials), "No completed trials")
    (work / "results.json").write_text(json.dumps({"trials": trials}))
    subprocess.run([sys.executable, controller_dir / "measure_shared.py", work],
                   stdout=subprocess.DEVNULL, check=True)

    active, batch = [], []
    print("Trial   Forward   Reverse    Active     Batch   Verified")
    for trial in trials:
        n = trial["attempt"]
        shared = read_json(work / f"{n}-shared.json")
        if expected is not None:
            check(shared == read_json(raw / f"{n}-shared.json"), f"Trial {n}: port samples differ")
        flow = shared["large_rx_frame_payload_gbps"]
        total = shared["ring_gbps"]
        whole = median(trial["aggregate_payload_gbps_bounds"])
        forward = sum(flow[edge] for edge in report["forward"])
        reverse = sum(rate for edge, rate in flow.items() if edge not in report["forward"])
        check(math.isclose(forward + reverse, total), f"Trial {n}: direction totals differ")
        gib = trial["verified_payload_bytes"] // 2**30
        if expected is not None:
            recorded, = [row for row in expected["trials"] if row["attempt"] == n]
            check(flow == recorded["directed_gbps"] and whole == recorded["whole_gbps"]
                  and total == recorded["active_gbps"] and gib == recorded["verified_gib"],
                  f"Trial {n}: throughput.json differs")
        print(f"{n:5} {forward:9.2f} {reverse:9.2f} {total:9.2f} {whole:9.2f} {gib:6} GiB")
        active.append(total)
        batch.append(whole)
    if expected is not None:
        check(median(active) == expected["active_gbps"] and median(batch) == expected["whole_gbps"],
              "Published medians differ")
    print(f"Median aggregate payload Gb/s: active {median(active):.2f}, whole batch {median(batch):.2f}")
    if report.get("incomplete_trials"):
        print("Incomplete attempts:", [r["attempt"] for r in report["incomplete_trials"]])


def replay_three(controller, raw):
    report = read_json(THREE_SPARK / "results.json")
    bounds, datapath_ns, app_ns = [], [], []
    verified = 0
    for number, recorded in enumerate(report["trials"], 1):
        trial = replay_trial(controller, controller.peer.edges(), raw, number, recorded["gate"])
        check(trial == recorded, f"Trial {number}: endpoint or timing data differs from results.json")
        bounds.append(trial["aggregate_payload_gbps_bounds"])
        verified += trial["verified_payload_bytes"]
        for edge in trial["links"].values():
            tx = edge["tx"]
            app = tx["application_transfer"]
            datapath_ns.append(int(tx["elapsed_ns"]))
            app_ns.append(app["start_ns"] + app["elapsed_ns"] - int(tx["start_ns"]))
    aggregate = median([median(side) for side in zip(*bounds)])
    print(f"Ring {aggregate:.1f} Gb/s, {verified // 2**30} GiB verified across {len(bounds)} trials")
    print(f"Median sender time from datapath start ({len(datapath_ns)} observations): "
          f"datapath {median(datapath_ns) / 1e9:.2f} s, application {median(app_ns) / 1e9:.2f} s")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--three-spark", action="store_true", help="replay the per-peer three-Spark run")
    mode.add_argument("--run", type=Path, help="replay a fresh ring4.py output directory")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        unpack(FOUR_SPARK / "controller.tar.gz", work)
        sys.path.insert(0, str(work / "controller"))
        import ring4 as controller

        if args.three_spark:
            raw = work / "logs"
            unpack(THREE_SPARK / "endpoints.tar.gz", raw)
            replay_three(controller, raw)
            return
        if args.run:
            raw = args.run.resolve()
            report, expected = read_json(raw / "results.json"), None
        else:
            raw = work / "logs"
            unpack(FOUR_SPARK / "endpoints.tar.gz", raw)
            report = read_json(FOUR_SPARK / "results.json")
            expected = read_json(FOUR_SPARK / "throughput.json")
        output = work / "recomputed"
        output.mkdir()
        replay_four(controller, raw, report, output, work, expected)


if __name__ == "__main__":
    main()
