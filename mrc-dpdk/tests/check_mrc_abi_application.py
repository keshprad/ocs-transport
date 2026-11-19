#!/usr/bin/env python3
"""Exercise the public OCP MRC API across two DPDK memif rails."""

import argparse
import json
from pathlib import Path

from memif_pair import eal_args, require, run_pair


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary", type=Path,
        default=Path(__file__).resolve().parent / "mrc_abi_application",
    )
    args = parser.parse_args()
    binary = args.binary.resolve()
    require(binary.is_file(), f"Build tests/mrc_abi_application first: {binary}")

    def command(role, cpu, directory):
        return eal_args(binary, role, cpu, directory) + ["--", role, str(directory)]

    tx, rx, _ = run_pair(command)
    require(tx["role"] == "tx" and rx["role"] == "rx" and tx["qpn"] != rx["qpn"]
            and tx["bytes"] == rx["bytes"] == 1038
            and tx["completions"] == rx["completions"] == 2,
            f"Unexpected public QP results: {tx}, {rx}")
    print(json.dumps({"case": "public_qp_over_dpdk", "tx": tx, "rx": rx}))


if __name__ == "__main__":
    main()
