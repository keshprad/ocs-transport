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

    def run_case(mode):
        def command(role, cpu, directory):
            return eal_args(binary, role, cpu, directory) + ["--", role, str(directory), mode]

        tx, rx, _ = run_pair(command)
        require(tx["role"] == "tx" and rx["role"] == "rx",
                f"Unexpected endpoint roles: {tx}, {rx}")
        return tx, rx

    tx, rx = run_case("simplex")
    require(tx["qpn"] != rx["qpn"] and tx["bytes"] == rx["bytes"] == 1038
            and tx["completions"] == rx["completions"] == 2,
            f"Unexpected public unidirectional QP results: {tx}, {rx}")
    print(json.dumps({"case": "public_qp_over_dpdk", "tx": tx, "rx": rx}))

    tx, rx = run_case("duplex")
    require(len(set(tx["qpns"] + rx["qpns"])) == 4,
            f"The connected QPs need distinct QPNs: {tx}, {rx}")
    for result in (tx, rx):
        require(result["bytes"] == 3 * 2049 and result["send_completions"] == 3
                and result["recv_completions"] == 3 and result["reset_qpn"] == result["qpns"][0],
                f"Unexpected public bidirectional QP results: {result}")
    print(json.dumps({"case": "public_bidirectional_qps_over_shared_dpdk_ports", "tx": tx, "rx": rx}))

    tx, rx = run_case("wire_error")
    require(tx["qpn"] != rx["qpn"] and tx["qp_fatal"] and rx["qp_fatal"]
            and not tx["cq_error"] and rx["cq_error"],
            f"Unexpected public async events after receiver CQ overflow: {tx}, {rx}")
    print(json.dumps({"case": "public_async_events_on_unexpected_wire_error", "tx": tx, "rx": rx}))


if __name__ == "__main__":
    main()
