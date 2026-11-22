#!/usr/bin/env python3
"""Check public capability-to-endpoint Service Time wiring during live traffic."""
import argparse
import json
from pathlib import Path

from memif_pair import eal_args, require, run_pair


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path,
                        default=Path(__file__).resolve().with_name("mrc_public_service_time"))
    args = parser.parse_args()

    def command(role, cpu, directory):
        return eal_args(args.binary, role, cpu, directory) + [
            "--", role, str(directory), "rx_flow_control"]

    tx, rx, _ = run_pair(command)
    for result, unsupported_index in ((tx, 0), (rx, 1)):
        require(result["service_time_dest_unsupported_qpn"] == result["qpns"][unsupported_index]
                and result["service_time_data_qps"] == result["service_time_probe_qps"] == 2,
                f"Missing directional Service Time wire reports: {result}")
    print(json.dumps({"case": "public_service_time_capability_wiring", "tx": tx, "rx": rx}))


if __name__ == "__main__":
    main()
