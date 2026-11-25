#!/usr/bin/env python3
"""Check two DPDK memif links against the emulator's scheduling and reorder code."""

import argparse
import csv
import json
import sys
from pathlib import Path

from memif_pair import MACS, eal_args, require, run_pair


BYTES = 4 * 1024 * 1024
FAULT_SEQ = 512
def command(binary, role, cpu, directory, drop_after):
    other = "tx" if role == "rx" else "rx"
    args = eal_args(binary, role, cpu, directory)
    args += ["--", role, str(BYTES), "1", "0", MACS[other][0], "1", MACS[other][1],
             "--rto-us", "50000", "--timeout-ms", "5000",
             "--trace", str(directory / f"{role}.csv")]
    if role == "tx" and drop_after is not None:
        args += ["--drop-path-after", str(drop_after)]
    return args


def read_trace(path):
    with path.open(newline="") as stream:
        return [(row["event"], int(row["seq"]), int(row["path"]))
                for row in csv.DictReader(stream)]


def run_transfer(binary, drop_after=None):
    def build(role, cpu, directory):
        return command(binary, role, cpu, directory, drop_after)

    def traces(directory):
        return read_trace(directory / "tx.csv"), read_trace(directory / "rx.csv")

    tx, rx, (sends, arrivals) = run_pair(build, traces)
    return tx, rx, sends, arrivals


def check_reorder(rx, trace, flow_stats):
    for event, seq, path in trace:
        require(event == "receive", f"Unexpected receiver event {event}")
        flow_stats.observe(seq, path)
    reference = flow_stats.to_dict()
    require(reference["received"] == rx["packets"], "Transfer is incomplete")
    require(reference["duplicates"] == rx["duplicates"], "Duplicate counts differ")
    require(reference["reorder_max"] == rx["reorder_max"], "Maximum reorder distance differs")
    reorder_count = reference["received"] - reference["reorder_hist"].get(0, 0)
    require(reorder_count == rx["reordered"], "Reordered packet counts differ")
    require([reference["per_plane_recv"].get(path, 0) for path in range(2)]
            == rx["received"], "Receive counts by path differ")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", type=Path, required=True, help="Local srv6-mrc-emulator checkout")
    default_binary = Path(__file__).resolve().parents[1] / "mrc_bench"
    parser.add_argument("--binary", type=Path, default=default_binary)
    options = parser.parse_args()
    require((options.emulator / "srv6_mrc" / "policy.py").is_file(),
            "The emulator path must contain srv6_mrc/policy.py")
    sys.path.insert(0, str(options.emulator.resolve()))

    from srv6_mrc.mrc.ev_state import EVStateConfig, EVStateTable
    from srv6_mrc.policy import EvSpray, HealthAwareMrc
    from srv6_mrc.reorder import FlowStats
    from srv6_mrc.topo import FlowKey

    binary = options.binary.resolve()
    require(binary.is_file(), f"Build the endpoint first: {binary}")
    flow = FlowKey("2001:db8::1", "2001:db8::2", 9000, 9001)

    class TwoPathTopology:
        planes = 2
        spines_per_plane = 1

        @staticmethod
        def select_spines_for_addrs(src, dst, count):
            return (0,)

    topology = TwoPathTopology()
    ev_spray = EvSpray(topology=topology)
    tx, rx, sends, arrivals = run_transfer(binary)
    check_reorder(rx, arrivals, FlowStats(flow))
    require(len(sends) == tx["packets"], "Healthy transfer retried or dropped a packet")
    for expected, (event, seq, path) in enumerate(sends):
        require((event, seq, path) == ("send", expected, ev_spray.pick(expected, flow)),
                f"Healthy packet {expected} differs from the emulator's path choice")
    print(json.dumps({"case": "healthy", "tx": tx, "rx": rx}))

    tx, rx, sends, arrivals = run_transfer(binary, FAULT_SEQ)
    check_reorder(rx, arrivals, FlowStats(flow))
    require(rx["reordered"] > 0, "Fault did not exercise out-of-order placement")
    dropped = {seq for event, seq, path in sends if event == "drop" and path == 1}
    retried_on_survivor = {seq for event, seq, path in sends if event == "retry" and path == 0}
    require(dropped and dropped <= retried_on_survivor,
            "Sender did not retransmit all dropped packets on the surviving path")

    table = EVStateTable(tenants=("green",), num_planes=2, num_paths=1,
                         cfg=EVStateConfig(loss_demote_consecutive=1, min_active_evs=1))
    reference_policy = HealthAwareMrc(table=table, tenant="green", topology=topology)
    table.record_loss_window("green", 1, 0, seen=0, expected=len(dropped))
    steady_routes = [(seq, path) for event, seq, path in sends if event == "send"][-256:]
    require(all(path == reference_policy.pick(seq, flow) == 0 for seq, path in steady_routes),
            "Post-fault traffic did not converge on the same path as the emulator")
    print(json.dumps({"case": "path_1_blackhole", "tx": tx, "rx": rx}))


if __name__ == "__main__":
    main()
