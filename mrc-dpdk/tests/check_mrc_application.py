#!/usr/bin/env python3
"""Drive the DPDK requester with application-owned work and completions."""

import argparse
import json
from pathlib import Path

from memif_pair import MACS, eal_args, require, run_pair


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--application", type=Path, default=root / "tests" / "mrc_endpoint_application")
    parser.add_argument("--receiver", type=Path, default=root / "mrc_write")
    args = parser.parse_args()
    require(args.application.is_file() and args.receiver.is_file(),
            "Build tests/mrc_endpoint_application and mrc_write first")

    def command(role, cpu, directory):
        if role == "tx":
            return eal_args(args.application.resolve(), role, cpu, directory)
        return eal_args(args.receiver.resolve(), role, cpu, directory) + [
            "--", "rx", "13,1025@84,0@85", "192.0.2.2", "192.0.2.1",
            "0", MACS["tx"][0], "1", MACS["tx"][1],
            "--initial-psn", "16777215", "--mpr", "1", "--max-wimm-inflight", "1",
            "--ack-timeout", "16", "--timeout-ms", "2000",
        ]

    tx, rx, _ = run_pair(command)
    require(tx["bytes"] == rx["bytes"] == 1038 and tx["write_completions"] == 3 and
            rx["immediate_completions"] == 2 and sum(rx["received"]) == 4,
            f"Unexpected application transfer results: {tx}, {rx}")
    print(json.dumps({"case": "application_posts_to_running_qp", "tx": tx, "rx": rx}))

    def link_command(role, cpu, directory):
        if role == "tx":
            return eal_args(args.application.resolve(), role, cpu, directory) + ["--", "link_transition"]
        return eal_args(args.receiver.resolve(), role, cpu, directory) + [
            "--", "rx", "4096,4096,4096", "192.0.2.2", "192.0.2.1",
            "0", MACS["tx"][0], "1", MACS["tx"][1],
            "--initial-psn", "16777215", "--mpr", "1", "--max-wimm-inflight", "1",
            "--ack-timeout", "16", "--timeout-ms", "5000",
        ]

    tx, rx, _ = run_pair(link_command)
    require(tx["bytes"] == rx["bytes"] == 12288 and tx["write_completions"] == 3 and
            tx["sent_by_round"] == [[2, 2], [4, 0], [2, 2]] and rx["received"] == [8, 4],
            f"Unexpected link transition results: {tx}, {rx}")
    print(json.dumps({"case": "application_rail_link_transition", "tx": tx, "rx": rx}))


if __name__ == "__main__":
    main()
