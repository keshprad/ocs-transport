#!/usr/bin/env python3
"""Exercise the public OCP MRC API across two DPDK memif rails."""

import argparse
import json
import subprocess
import tempfile
import time
from pathlib import Path

from memif_pair import eal_args, require, run_pair


def control(controller, operation, store):
    result = subprocess.run(["unshare", "-Ur", str(controller), operation, store],
                            capture_output=True, text=True, timeout=4)
    require(result.returncode == 0,
            f"Controller {operation} failed: {result.stdout}\n{result.stderr}")
    return result.stdout.strip()


def check_live_profile(binary, controller):
    with tempfile.TemporaryDirectory(prefix="mrc-abi-ev-") as store:
        profile_id = control(controller, "--seed-denied", store)
        require(profile_id.isdecimal() and int(profile_id) > 0,
                f"Invalid controller test profile ID: {profile_id!r}")

        def command(role, cpu, directory):
            return eal_args(binary, role, cpu, directory) + [
                "--", role, str(directory), "ev_profile", store, profile_id,
            ]

        def unblock(sender):
            ready = Path(store) / "rx.profile-blocked"
            deadline = time.monotonic() + 4
            while not ready.exists():
                if sender.poll() is not None:
                    stdout, stderr = sender.communicate()
                    raise RuntimeError(f"Sender exited before both EVs were blocked: {stdout}\n{stderr}")
                require(time.monotonic() < deadline,
                        "Timed out checking that denied EVs blocked completion and remote placement")
                time.sleep(0.01)
            control(controller, "--unblock", store)

        tx, rx, _ = run_pair(command, sender_started=unblock)
        require(tx["role"] == "tx" and rx["role"] == "rx" and tx["qpn"] != rx["qpn"]
                and tx["bytes"] == rx["bytes"] == 1038
                and tx["completions"] == rx["completions"] == 2,
                f"Controller did not unblock the public QP: {tx}, {rx}")
        print(json.dumps({"case": "public_qp_live_ev_update", "tx": tx, "rx": rx}))


def check_duplex(tx, rx, completions=3):
    require(len(set(tx["qpns"] + rx["qpns"])) == 4,
            f"The connected QPs need distinct QPNs: {tx}, {rx}")
    for result in (tx, rx):
        require(result["bytes"] == completions * 2049 and result["send_completions"] == completions
                and result["recv_completions"] == completions and result["reset_qpn"] == result["qpns"][0],
                f"Unexpected public bidirectional QP results: {result}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary", type=Path,
        default=Path(__file__).resolve().parent / "mrc_abi_application",
    )
    parser.add_argument("--controller", type=Path,
                        help="Path to test_mrc_ctl_abi for the live EV, NSCC window, and Quick Adapt cases")
    args = parser.parse_args()
    binary = args.binary.resolve()
    require(binary.is_file(), f"Build tests/mrc_abi_application first: {binary}")

    def run_case(mode, *options):
        def command(role, cpu, directory):
            return eal_args(binary, role, cpu, directory) + ["--", role, str(directory), mode, *options]

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
    check_duplex(tx, rx)
    print(json.dumps({"case": "public_bidirectional_qps_over_shared_dpdk_ports", "tx": tx, "rx": rx}))

    tx, rx = run_case("rx_flow_control")
    check_duplex(tx, rx, completions=8)
    for result, unsupported_index in ((tx, 0), (rx, 1)):
        require(result["service_time_dest_unsupported_qpn"] == result["qpns"][unsupported_index]
                and result["service_time_data_qps"] == result["service_time_probe_qps"] == 2,
                f"Missing directional service-time reports: {result}")
    print(json.dumps({"case": "public_qp_receiver_flow_control", "tx": tx, "rx": rx}))

    tx, rx = run_case("wire_error")
    require(tx["qpn"] != rx["qpn"] and tx["qp_fatal"] and rx["qp_fatal"]
            and not tx["cq_error"] and rx["cq_error"],
            f"Unexpected public async events after receiver CQ overflow: {tx}, {rx}")
    print(json.dumps({"case": "public_async_events_on_unexpected_wire_error", "tx": tx, "rx": rx}))

    if args.controller:
        controller = args.controller.resolve()
        require(controller.is_file(), f"Build tests/test_mrc_ctl_abi first: {controller}")
        check_live_profile(binary, controller)
        with tempfile.TemporaryDirectory(prefix="mrc-abi-cc-") as store:
            profile_id = control(controller, "--seed-cc", store)
            require(profile_id.isdecimal() and int(profile_id) > 0,
                    f"Invalid controller test CC profile ID: {profile_id!r}")
            tx, rx = run_case("cc_profile", store, profile_id)
        check_duplex(tx, rx)
        print(json.dumps({"case": "public_qp_nscc_window", "tx": tx, "rx": rx}))

        with tempfile.TemporaryDirectory(prefix="mrc-abi-qa-") as store:
            profile_id = control(controller, "--seed-qa", store)
            require(profile_id.isdecimal() and int(profile_id) > 0,
                    f"Invalid controller test QA profile ID: {profile_id!r}")
            tx, rx = run_case("qa_profile", store, profile_id)
        require(tx["qpn"] != rx["qpn"] and tx["bytes"] == rx["bytes"] == 4450
                and tx["completions"] == rx["completions"] == 4
                and tx["baseline_burst"] == 3 and tx["capped_burst"] == 1
                and tx["delayed_sack_us"] >= 10000,
                f"Unexpected public Quick Adapt results: {tx}, {rx}")
        print(json.dumps({"case": "public_qp_nscc_quick_adapt", "tx": tx, "rx": rx}))


if __name__ == "__main__":
    main()
