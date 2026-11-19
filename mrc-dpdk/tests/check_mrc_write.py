#!/usr/bin/env python3
"""Exercise the OCP-format endpoint over two local DPDK memif links."""

import argparse
import json
from pathlib import Path

from memif_pair import MACS, eal_args, require, run_pair


def transfer(binary, size, *options, drop_after=None, drop_final_acks=None,
             trim_after=None, ipv6=False, work_requests=1, posted_receives=None, expected_errors=None):
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
        if tx and drop_after is not None:
            args += ["--drop-path-after", str(drop_after)]
        if tx and trim_after is not None:
            args += ["--trim-path-after", str(trim_after)]
        if not tx and drop_final_acks is not None:
            args += ["--drop-final-acks", str(drop_final_acks)]
        if not tx and posted_receives is not None:
            args += ["--posted-receives", str(posted_receives)]
        return args

    tx, rx, _ = run_pair(command, expected_errors=expected_errors)
    if expected_errors:
        return tx, rx
    require(tx["bytes"] == rx["bytes"] == size * work_requests, "Incorrect completed byte count")
    require(tx["packets"] == sum(rx["received"]), "Receiver missed or double-counted a segment")
    require(tx["transport_acks"] > 0, "Requester did not receive a semantic transport ACK")
    return tx, rx


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path,
                        default=Path(__file__).resolve().parents[1] / "mrc_write")
    binary = parser.parse_args().binary.resolve()
    require(binary.is_file(), f"Build mrc_write first: {binary}")

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
    require(tx["sent"] == [2048, 2048], f"Healthy striping mismatch: {tx}")
    require(tx["sacks"] > 0 and tx["retries"] == [0, 0], "Healthy transfer did not receive SACKs")
    require(rx["port_status_updates"] > 0 and tx["port_status_replies"] > 0,
            "Node-scoped port status was not acknowledged")
    print(json.dumps({"case": "write_striped", "tx": tx, "rx": rx}))

    tx, rx = transfer(binary, 4194301, "--immediate", "305419896",
                      "--initial-psn", "16777214", drop_after=512)
    require(tx["dropped"] > 0 and tx["retries"][0] >= tx["dropped"],
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
             expected_errors={"tx": "syndrome 0x63", "rx": "Protocol error"})
    print(json.dumps({"case": "write_imm_without_receive_credit"}))


if __name__ == "__main__":
    main()
