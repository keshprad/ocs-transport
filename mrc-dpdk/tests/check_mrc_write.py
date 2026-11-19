#!/usr/bin/env python3
"""Exercise the OCP-format endpoint over two local DPDK memif links."""

import argparse
import json
import os
import select
import socket
import subprocess
import tempfile
import time
from pathlib import Path

from memif_pair import MACS, eal_args, require, run_pair


def transfer(binary, size, *options, drop_after=None, drop_final_acks=None,
             trim_after=None, ipv6=False, work_requests=1, posted_receives=None,
             expected_errors=None, negotiate=False, srv6=False, tx_options=(), rx_options=(),
             sender_started=None):
    oob_port = None
    if negotiate:
        with socket.socket() as reserve:
            reserve.bind(("127.0.0.1", 0))
            oob_port = str(reserve.getsockname()[1])
    def command(role, cpu, directory):
        tx = role == "tx"
        peer = "rx" if tx else "tx"
        addresses = ({"tx": "2001:db8::1", "rx": "2001:db8::2"} if ipv6 else
                     {"tx": "192.0.2.1", "rx": "192.0.2.2"})
        local_ip, peer_ip = addresses[role], addresses[peer]
        args = eal_args(binary, role, cpu, directory)
        args += ["--", role, str(size), local_ip, peer_ip, "0", MACS[peer][0],
                 "1", MACS[peer][1], "--ack-timeout", "16",
                 "--work-requests", str(work_requests), *options]
        if srv6:
            sids = {"tx": "2001:db8:aaaa::1", "rx": "2001:db8:bbbb::3"}
            args += ["--srv6-local", sids[role], "--srv6-peer", sids[peer], "--srv6-srh", "1"]
        if negotiate:
            args += ["--oob-host", "127.0.0.1", "--oob-port", oob_port]
            if tx:
                args += ["--requestor-qpn", "0x140101", "--initial-psn", "77",
                         "--p-key", "0x2345"]
            else:
                args += ["--responder-qpn", "0x250102", "--initial-psn", "16777212",
                         "--remote-address", "0x123456781000",
                         "--p-key", "0xa345", "--mpr", "1", "--max-wimm-inflight", "2"]
        if tx and drop_after is not None:
            args += ["--drop-path-after", str(drop_after)]
        if tx and trim_after is not None:
            args += ["--trim-path-after", str(trim_after)]
        if not tx and drop_final_acks is not None:
            args += ["--drop-final-acks", str(drop_final_acks)]
        if not tx and posted_receives is not None:
            args += ["--posted-receives", str(posted_receives)]
        return args + list(tx_options if tx else rx_options)

    tx, rx, _ = run_pair(command, expected_errors=expected_errors, sender_started=sender_started)
    if expected_errors:
        return tx, rx
    require(tx["bytes"] == rx["bytes"] == size * work_requests, "Incorrect completed byte count")
    require(tx["packets"] == sum(rx["received"]), "Receiver missed or double-counted a segment")
    require(tx["transport_acks"] > 0, "Requester did not receive a semantic transport ACK")
    require(tx["write_completions"] == work_requests, "Requester CQ did not report every WRITE")
    return tx, rx


def controlled_ev_profile(binary, controller):
    with tempfile.TemporaryDirectory(prefix="mrc-ev-") as directory:
        def control(*args, check=True):
            return subprocess.run([controller, directory, *args], text=True, capture_output=True,
                                  check=check, timeout=3, env={**os.environ, "LC_ALL": "C"})

        control("create", "42")
        control("configure", "42", "explicit", "1", "0x7233e499", "0x8433e49a")
        control("online", "42")
        control("deny", "42", "2")
        require("42 online explicit count=2 min_active=1 denied=0x00000002" in
                control("show", "42").stdout, "Profile configuration did not persist")

        def enable_path(sender):
            ready = b"MRC EV profile 42 active, deny mask 0x3\n"
            output = bytearray()
            deadline = time.monotonic() + 3
            while ready not in output:
                remaining = deadline - time.monotonic()
                require(remaining > 0 and select.select([sender.stderr], [], [], remaining)[0],
                        f"Sender did not acquire the EV profile: {output.decode(errors='replace')}")
                chunk = os.read(sender.stderr.fileno(), 4096)
                require(chunk, f"Sender exited before acquiring the profile: {output.decode(errors='replace')}")
                output.extend(chunk)
            offline = control("offline", "42", check=False)
            require(offline.returncode != 0 and "Device or resource busy" in offline.stderr,
                    "The active QP did not hold the online profile")
            control("allow", "42", "2")

        tx, rx = transfer(binary, 1048576, ipv6=True,
                          tx_options=("--profile-dir", directory, "--profile-id", "42",
                                      "--deny-path-mask", "1"),
                          sender_started=enable_path)
        require(tx["ev_admin_updates"] == 1 and tx["sent"] == rx["received"] == [0, 1024],
                f"Live EV update did not unblock only path 1: {tx}, {rx}")
        require(tx["probe_sent"][0] == 0 and tx["probe_sent"][1] > 0 and tx["probe_replies"] > 0,
                f"Probes did not correlate the configured IPv6 entropy: {tx}")
        control("offline", "42")
        control("remove", "42")
        require(not control("list").stdout, "Profile remained after removal")
        return tx, rx


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path,
                        default=Path(__file__).resolve().parents[1] / "mrc_write")
    parser.add_argument("--controller", type=Path, help="Path to mrc_ctl_profile")
    args = parser.parse_args()
    binary = args.binary.resolve()
    controller = (args.controller or binary.with_name("mrc_ctl_profile")).resolve()
    require(binary.is_file(), f"Build mrc_write first: {binary}")
    require(controller.is_file(), f"Build mrc_ctl_profile first: {controller}")

    tx, rx = transfer(binary, 13)
    require(tx["packets"] == 1 and not rx["immediate_completed"], "WRITE ONLY failed")
    print(json.dumps({"case": "write_only", "tx": tx, "rx": rx}))

    tx, rx = transfer(binary, 0, "--requestor-qpn", "0x120101",
                      "--responder-qpn", "0x240102", "--mpr", "1",
                      "--remote-key", "0x2468", "--p-key", "0xd111",
                      "--remote-address", "0x123456789000", "--udp-port", "4971")
    require(tx["packets"] == 1 and sum(rx["received"]) == 1, "Zero-length WRITE ONLY failed")
    print(json.dumps({"case": "zero_length_write", "tx": tx, "rx": rx}))

    tx, rx = transfer(binary, 13, "--immediate", "17", drop_final_acks=1)
    require(rx["dropped_transport_acks"] == 1 and tx["sacks"] > 0,
            "Final ACK fault did not leave the sender with a SACK")
    require(sum(tx["retries"]) == 1 and rx["duplicates"] == 1 and rx["immediate_completed"],
            "SACK alone completed the operation or the final ACK was not recovered idempotently")
    print(json.dumps({"case": "write_imm_lost_transport_ack", "tx": tx, "rx": rx}))

    tx, rx = transfer(binary, 4194304)
    require(all(sent >= 1920 for sent in tx["sent"]),
            f"Healthy traffic was not distributed over both paths: {tx}")
    require(tx["sacks"] > 0 and tx["retries"] == [0, 0], "Healthy transfer did not receive SACKs")
    require(rx["port_status_updates"] > 0 and tx["port_status_replies"] > 0,
            "Node-scoped port status was not acknowledged")
    require(rx["probe_requests"] > 0 and tx["probe_replies"] > 0,
            "Reliability probe SACK did not echo any in-flight probe ID")
    print(json.dumps({"case": "write_striped", "tx": tx, "rx": rx}))

    tx, rx = transfer(binary, 4194301, "--immediate", "305419896",
                      "--initial-psn", "16777214", drop_after=512)
    require(tx["dropped"] > 0 and tx["retries"][0] >= tx["dropped"] and
            tx["retries"][1] == 0 and tx["ev_failed"] > 0 and tx["ev_recovered"] == 0,
            "Blackhole did not recover the dropped segments through path 0")
    require(rx["reorder_max"] > 0 and rx["immediate_completed"],
            "Reordered WRITEIMM did not complete after placement")
    print(json.dumps({"case": "write_imm_wrap_blackhole", "tx": tx, "rx": rx}))

    tx, rx = transfer(binary, 1048576, trim_after=64)
    require(rx["trimmed"] > 0 and rx["trim_nacks"] > 0 and tx["trim_nacks"] > 0,
            f"Trim traffic did not generate NACKs: {tx}, {rx}")
    require(tx["retries"][0] >= rx["trimmed"] and rx["duplicates"] == 0,
            "Trimmed packets were placed or did not recover over the surviving link")
    print(json.dumps({"case": "write_trim_nack", "tx": tx, "rx": rx}))

    tx, rx = transfer(binary, 1048576, "--immediate", "23", trim_after=64, ipv6=True)
    require(rx["trimmed"] > 0 and tx["trim_nacks"] > 0 and rx["immediate_completed"],
            f"IPv6 WRITEIMM did not recover from trimmed data: {tx}, {rx}")
    print(json.dumps({"case": "ipv6_write_imm_trim_nack", "tx": tx, "rx": rx}))

    tx, rx = transfer(binary, 8195, "--immediate", "4294967290", "--max-wimm-inflight", "2",
                      "--mpr", "1", work_requests=32, drop_after=6)
    require(rx["immediate_completions"] == 32 and tx["dropped"] > 0 and rx["reorder_max"] > 0,
            "WRITEIMM sequence failed under receive-credit limits and out-of-order placement")
    print(json.dumps({"case": "ordered_write_imm_completions", "tx": tx, "rx": rx}))

    transfer(binary, 1024, "--immediate", "23", posted_receives=0,
             expected_errors={"tx": "syndrome 0x63", "rx": "MRC path 0: RX frames="})
    print(json.dumps({"case": "write_imm_without_receive_credit"}))

    tx, rx = transfer(binary, 8195, "--immediate", "99", negotiate=True, work_requests=8)
    require(rx["immediate_completions"] == 8, "Negotiated WRITEIMM sequence was incomplete")
    require(tx["registered_memory"] and rx["registered_memory"],
            "Negotiated transfer did not use provider-registered memory")
    print(json.dumps({"case": "negotiated_qp", "tx": tx, "rx": rx}))

    tx, rx = transfer(binary, 41989, "--immediate", "22", trim_after=16,
                      ipv6=True, srv6=True)
    require(rx["trimmed"] > 0 and tx["trim_nacks"] > 0 and rx["immediate_completed"],
            f"Preserved SRv6 wrapper or outer trim recovery failed: {tx}, {rx}")
    print(json.dumps({"case": "srv6_write_imm_trim_nack", "tx": tx, "rx": rx}))

    tx, rx = transfer(binary, 1048576, tx_options=("--deny-path-mask", "2"))
    require(tx["sent"] == [1024, 0] and rx["received"] == [1024, 0],
            f"Data was sent on a denied entropy value: {tx}, {rx}")
    require(tx["probe_sent"][0] > 0 and tx["probe_sent"][1] == 0,
            "The denied entropy value was probed or no active path was probed")
    print(json.dumps({"case": "denied_ev", "tx": tx, "rx": rx}))

    tx, rx = controlled_ev_profile(binary, controller)
    print(json.dumps({"case": "controlled_ev_profile", "tx": tx, "rx": rx}))

    tx, rx = transfer(binary, 16777216, "--ack-timeout", "13", drop_after=512,
                      tx_options=("--restore-path-after", "2048"))
    require(tx["ev_failed"] > 0 and tx["ev_recovered"] > 0 and rx["received"][1] > 256,
            f"Path 1 did not rejoin the active set after a correlated probe: {tx}, {rx}")
    print(json.dumps({"case": "recovered_ev", "tx": tx, "rx": rx}))

    change = ("--dynamic-mpr", "1", "--mpr", "8", "--mpr-change-at", "65",
              "--mpr-change-to", "1")
    tx, rx = transfer(binary, 4194304, negotiate=True, tx_options=("--dynamic-mpr", "1"),
                      rx_options=change)
    require(tx["mpr_updates"] == rx["mpr_updates"] == 1,
            f"Peers did not apply the negotiated receive-window shrink: {tx}, {rx}")
    print(json.dumps({"case": "dynamic_mpr_shrink", "tx": tx, "rx": rx}))

    tx, rx = transfer(binary, 1048576, negotiate=True, rx_options=change)
    require(tx["mpr_updates"] == rx["mpr_updates"] == 0,
            "The receive window changed without bilateral capability negotiation")
    print(json.dumps({"case": "dynamic_mpr_not_negotiated", "tx": tx, "rx": rx}))


if __name__ == "__main__":
    main()
