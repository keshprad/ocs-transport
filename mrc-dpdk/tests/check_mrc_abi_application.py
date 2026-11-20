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


def unblock_profile(controller, operation, store, sender):
    ready = Path(store) / "rx.profile-blocked"
    deadline = time.monotonic() + 4
    while not ready.exists():
        if sender.poll() is not None:
            stdout, stderr = sender.communicate()
            raise RuntimeError(f"Sender exited before both EVs were blocked: {stdout}\n{stderr}")
        require(time.monotonic() < deadline,
                "Timed out checking that denied EVs blocked completion and remote placement")
        time.sleep(0.01)
    control(controller, operation, store)


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
            unblock_profile(controller, "--unblock", store, sender)

        tx, rx, _ = run_pair(command, sender_started=unblock)
        require(tx["role"] == "tx" and rx["role"] == "rx" and tx["qpn"] != rx["qpn"]
                and tx["bytes"] == rx["bytes"] == 1038
                and tx["completions"] == rx["completions"] == 2,
                f"Controller did not unblock the public QP: {tx}, {rx}")
        print(json.dumps({"case": "public_qp_live_ev_update", "tx": tx, "rx": rx}))


def check_endpoint_operations(binary, format_name):
    def command(role, cpu, directory):
        args = eal_args(binary, role, cpu, directory) + [
            "--", role, str(directory), "endpoint_ops", format_name,
        ]
        if role == "tx":
            # EAL treats the namespaced user as root and otherwise uses /var/run.
            args = ["unshare", "-Ur", "env", f"RUNTIME_DIRECTORY={directory}", *args]
        return args

    tx, rx, _ = run_pair(command)
    require(tx["role"] == "tx" and rx["role"] == "rx"
            and tx["format"] == rx["format"] == format_name
            and tx["endpoint_packets"] == rx["endpoint_packets"] == 7,
            f"Unexpected connectionless endpoint operation batch: {tx}, {rx}")
    print(json.dumps({"case": f"public_endpoint_ops_{format_name}", "tx": tx, "rx": rx}))


def check_duplex(tx, rx, completions=3):
    require(len(set(tx["qpns"] + rx["qpns"])) == 4,
            f"The connected QPs need distinct QPNs: {tx}, {rx}")
    for result in (tx, rx):
        require(result["bytes"] == completions * 2049 and result["send_completions"] == completions
                and result["recv_completions"] == completions and result["reset_qpn"] == result["qpns"][0],
                f"Unexpected public bidirectional QP results: {result}")


def check_srv6(tx, rx, rx_decapped, rx_advanced):
    require(tx["qpn"] != rx["qpn"] and tx["bytes"] == rx["bytes"] == 1038
            and tx["completions"] == rx["completions"] == 2
            and tx["srv6_probe_rails"] == rx["srv6_probe_rails"] == 2
            and tx["srv6_decapped_rails"] == 1 and rx["srv6_decapped_rails"] == rx_decapped
            and tx["srv6_advanced_rails"] == 0 and rx["srv6_advanced_rails"] == rx_advanced
            and tx["srv6_wrong_sid_probes"] == 0 and rx["srv6_wrong_sid_probes"] == 1,
            f"Unexpected public SRv6 QP results: {tx}, {rx}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary", type=Path,
        default=Path(__file__).resolve().parent / "mrc_abi_application",
    )
    parser.add_argument("--controller", type=Path,
                        help="Path to test_mrc_ctl_abi for privileged controller and endpoint cases")
    focused = parser.add_mutually_exclusive_group()
    focused.add_argument("--blackout-only", action="store_true", help="Run only the two-rail blackout cases")
    focused.add_argument("--last-hop-trim-only", action="store_true", help="Run only the last-hop trim case")
    focused.add_argument("--service-level-dscp-only", action="store_true", help="Run only the service-level DSCP case")
    args = parser.parse_args()
    binary = args.binary.resolve()
    require(binary.is_file(), f"Build tests/mrc_abi_application first: {binary}")

    def run_case(mode, *options, sender_started=None):
        def command(role, cpu, directory):
            return eal_args(binary, role, cpu, directory) + ["--", role, str(directory), mode, *options]

        tx, rx, _ = run_pair(command, sender_started=sender_started)
        require(tx["role"] == "tx" and rx["role"] == "rx",
                f"Unexpected endpoint roles: {tx}, {rx}")
        return tx, rx

    def check_blackouts():
        for mode, completion, recovered in (
            ("blackout_finite", "retry-exceeded", False),
            ("blackout_infinite", "success", True),
        ):
            tx, rx = run_case(mode)
            require(tx["qpn"] != rx["qpn"] and tx["completion"] == completion
                    and rx["blocked_writes"] == [1, 1] and rx["failed_probe_rails"] == 3
                    and rx["recovered"] == recovered,
                    f"Unexpected requester or rail behavior during {mode}: {tx}, {rx}")
            print(json.dumps({"case": f"public_qp_{mode}", "tx": tx, "rx": rx}))

    def check_last_hop_trim(service_level_dscp=False):
        mode = "service_level_dscp" if service_level_dscp else "last_hop_trim"
        tx, rx = run_case(mode)
        require(tx["qpn"] != rx["qpn"] and tx["completions"] == rx["completions"] == 1
                and tx["nack_reason"] == 2 and tx["original_udp_len"] == 64
                and tx["nack_rail"] == rx["marked_rail"] and rx["marked_rail"] != rx["retry_rail"],
                f"Unexpected last-hop trim recovery: {tx}, {rx}")
        if service_level_dscp:
            require(tx["control_dscp"] == 48 and rx["data_dscp"] == 26
                    and rx["retry_dscp"] == 34 and rx["probe_rails"] == 3,
                    f"Unexpected service-level traffic classes: {tx}, {rx}")
        print(json.dumps({"case": f"public_qp_{mode}", "tx": tx, "rx": rx}))

    if args.last_hop_trim_only or args.service_level_dscp_only:
        check_last_hop_trim(service_level_dscp=args.service_level_dscp_only)
        return

    if args.blackout_only:
        check_blackouts()
        return

    tx, rx = run_case("simplex")
    require(tx["qpn"] != rx["qpn"] and tx["bytes"] == rx["bytes"] == 1038
            and tx["completions"] == rx["completions"] == 2,
            f"Unexpected public unidirectional QP results: {tx}, {rx}")
    print(json.dumps({"case": "public_qp_over_dpdk", "tx": tx, "rx": rx}))

    tx, rx = run_case("inline")
    require(tx["qpn"] != rx["qpn"] and tx["bytes"] == rx["bytes"] == 1038
            and tx["completions"] == rx["completions"] == 2
            and rx["inline_fragment_retransmitted"],
            f"Unexpected public inline WRITE results: {tx}, {rx}")
    print(json.dumps({"case": "public_inline_write_retransmission", "tx": tx, "rx": rx}))

    for mode in ("srv6", "srv6_transit"):
        tx, rx = run_case(mode)
        check_srv6(tx, rx, rx_decapped=2 if mode == "srv6" else 0,
                   rx_advanced=int(mode == "srv6_transit"))
        print(json.dumps({"case": f"public_qp_{mode}", "tx": tx, "rx": rx}))

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

    for mode, event, completion in (
        ("wire_request_error", "QP_REQ_ERR", "REM_INV_REQ_ERR"),
        ("wire_access_error", "QP_ACCESS_ERR", "REM_ACCESS_ERR"),
    ):
        tx, rx = run_case(mode)
        require(tx["qpn"] != rx["qpn"] and tx["qp_event"] == "QP_FATAL"
                and tx["completion"] == completion and tx["completions"] == 1
                and rx["qp_event"] == event and rx["completion"] == "WR_FLUSH_ERR"
                and rx["completions"] == 2,
                f"Unexpected public async events or completions after {mode}: {tx}, {rx}")
        print(json.dumps({"case": f"public_async_events_on_{mode}", "tx": tx, "rx": rx}))

    check_blackouts()
    check_last_hop_trim()
    check_last_hop_trim(service_level_dscp=True)

    if args.controller:
        controller = args.controller.resolve()
        require(controller.is_file(), f"Build tests/test_mrc_ctl_abi first: {controller}")
        for format_name in ("udp", "stev", "srv6", "shared"):
            check_endpoint_operations(binary, format_name)
        check_live_profile(binary, controller)
        for mode, seed, expected in (
            ("generated_ev_udp", "--seed-gen", [0xC405, 0xC410]),
            ("generated_ev_stev", "--seed-gen-stev", [0x0A00E499, 0x0A20E499]),
            ("generated_ev_custom_stev", "--seed-gen-custom-stev", [0x1AB28CC1, 0x1AB28CC2]),
        ):
            with tempfile.TemporaryDirectory(prefix="mrc-abi-gen-") as store:
                profile_id = control(controller, seed, store)
                require(profile_id.isdecimal() and int(profile_id) > 0,
                        f"Invalid generated EV profile ID: {profile_id!r}")
                tx, rx = run_case(mode, store, profile_id)
            require(tx["qpn"] != rx["qpn"] and tx["bytes"] == rx["bytes"] == 1038
                    and tx["completions"] == rx["completions"] == 2
                    and tx["rail_entropy"] == rx["rail_entropy"] == expected,
                    f"Unexpected generated EVs on the two rails: {tx}, {rx}")
            print(json.dumps({"case": f"public_qp_{mode}", "tx": tx, "rx": rx}))

        for mode, seed in (
            ("explicit_ev_srv6", "--seed-explicit-srv6"),
            ("generated_ev_srv6_srh", "--seed-generated-srv6-srh"),
        ):
            with tempfile.TemporaryDirectory(prefix="mrc-abi-srv6-") as store:
                profile_id = control(controller, seed, store)
                require(profile_id.isdecimal() and int(profile_id) > 0,
                        f"Invalid SRv6 EV profile ID: {profile_id!r}")

                def unblock(sender):
                    unblock_profile(controller, "--unblock-explicit-srv6", store, sender)

                tx, rx = run_case(mode, store, profile_id,
                                  sender_started=unblock if mode == "explicit_ev_srv6" else None)
            check_srv6(tx, rx, rx_decapped=0, rx_advanced=2)
            require(tx["rail_entropy"] == rx["rail_entropy"] == [0x0100C000, 0x0101C001],
                    f"Unexpected reflected tokens for SRv6 profile: {tx}, {rx}")
            print(json.dumps({"case": f"public_qp_{mode}", "tx": tx, "rx": rx}))

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
