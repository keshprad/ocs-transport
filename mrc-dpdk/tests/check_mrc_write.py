#!/usr/bin/env python3
"""Exercise the OCP-format endpoint over local DPDK memif links."""

import argparse
import json
import os
import re
import select
import socket
import subprocess
import tempfile
import time
from pathlib import Path

from memif_pair import MACS, collect, eal_args, require, run_pair, start


def transfer(binary, size, *options, drop_after=None, drop_final_acks=None,
             trim_after=None, ipv6=False, work_requests=1, posted_receives=None,
             expected_errors=None, negotiate=False, srv6=False, tx_options=(), rx_options=(),
             sender_started=None, writes=None, rail_count=2):
    work = str(size)
    repeat = ("--work-requests", str(work_requests))
    total_bytes = size * work_requests
    if writes is not None:
        work = ",".join(str(length) if immediate is None else f"{length}@{immediate}"
                        for length, immediate in writes)
        repeat = ()
        work_requests = len(writes)
        total_bytes = sum(length for length, _ in writes)
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
        args = eal_args(binary, role, cpu, directory, rail_count=rail_count)
        args += ["--", role, work, local_ip, peer_ip]
        for rail, mac in enumerate(MACS[peer][:rail_count]):
            args += [str(rail), mac]
        args += ["--ack-timeout", "16", *repeat, *options]
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

    tx, rx, _ = run_pair(command, expected_errors=expected_errors, sender_started=sender_started,
                         rail_count=rail_count)
    if expected_errors:
        return tx, rx
    require(tx["bytes"] == rx["bytes"] == total_bytes, "Incorrect completed byte count")
    require(tx["packets"] == sum(rx["received"]), "Receiver missed or double-counted a segment")
    require(tx["transport_acks"] > 0, "Requester did not receive a semantic transport ACK")
    require(tx["write_completions"] == work_requests, "Requester CQ did not report every WRITE")
    return tx, rx


def check_rail_counts(binary):
    for rail_count in (1, 2, 3, 4, 8):
        tx, rx = transfer(binary, 1048573, "--immediate", "305419896",
                          "--initial-psn", "16777214", rail_count=rail_count)
        for counters in (tx, rx):
            require(all(len(counters[key]) == rail_count
                        for key in ("sent", "received", "retries", "probe_sent")),
                    f"Wrong counter array lengths for {rail_count} rails: {counters}")
        require(tx["sent"] == rx["received"] and all(tx["sent"]) and not any(tx["retries"]),
                f"Healthy WRITEIMM did not use all {rail_count} rails: {tx}, {rx}")
        require(rx["immediate_completions"] == 1,
                f"WRITEIMM did not complete across PSN wrap with {rail_count} rails: {rx}")
        print(json.dumps({"case": f"write_imm_wrap_{rail_count}_rails", "tx": tx, "rx": rx}))

    tx, rx = transfer(binary, 1048576, rail_count=8, tx_options=("--deny-path-mask", "0x80"))
    require(tx["sent"] == rx["received"] and all(tx["sent"][:7]) and tx["sent"][7] == 0,
            f"The deny mask did not exclude only rail 7: {tx}, {rx}")
    require(tx["probe_sent"][7] == 0 and any(tx["probe_sent"][:7]),
            f"Unexpected probe routing with rail 7 denied: {tx}")
    print(json.dumps({"case": "denied_rail_7", "tx": tx, "rx": rx}))

    mac = MACS["rx"][0]
    too_many_rails = []
    for rail in range(33):
        too_many_rails.extend((str(rail), f"02:00:00:00:00:{0x10 + rail:02x}"))
    cases = (
        ("no_rails", ()),
        ("incomplete_rail_pair", ("0",)),
        ("duplicate_port", ("0", mac, "0", MACS["rx"][1])),
        ("more_than_32_rails", too_many_rails),
        ("deny_mask_outside_rail_count", ("0", mac, "--deny-path-mask", "2")),
    )
    cpu = min(os.sched_getaffinity(0))
    for name, args in cases:
        with tempfile.TemporaryDirectory(prefix="mrc-") as temporary:
            directory = Path(temporary)
            command = eal_args(binary, "tx", cpu, directory, rail_count=8)
            command += ["--", "tx", "13", "192.0.2.1", "192.0.2.2", *args]
            with start(command, directory) as process:
                try:
                    collect(process, "Usage:")
                finally:
                    if process.poll() is None:
                        process.kill()
        print(json.dumps({"case": f"reject_{name}"}))


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


def check_controller_ev_events(binary, controller, bad_only):
    with tempfile.TemporaryDirectory(prefix="mrc-ev-events-") as directory:
        store = Path(directory)
        operation = "--watch-ev-bad" if bad_only else "--watch-ev-events"
        with subprocess.Popen(["unshare", "-Ur", str(controller), operation, str(store)],
                              text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as monitor:
            try:
                require(select.select([monitor.stdout], [], [], 4)[0], "EV event CQ did not become ready")
                profile_id = monitor.stdout.readline().strip()
                if not profile_id and monitor.poll() is not None:
                    _, stderr = monitor.communicate(timeout=1)
                    raise RuntimeError(f"EV event controller exited before setup\n{stderr}")
                require(profile_id.isdecimal() and int(profile_id) > 0, "Invalid EV event profile ID")
                tx, rx = transfer(binary, 16777216, "--ack-timeout", "13", drop_after=512,
                                  tx_options=("--restore-path-after", "2048",
                                              "--profile-dir", str(store), "--profile-id", profile_id))
                require(tx["ev_failed_mask"] & 2 and tx["ev_recovered_mask"] & 2 and rx["received"][1] > 256,
                        f"EV event case did not exercise failure and recovery: {tx}, {rx}")
                (store / "tx.finished").touch()
                stdout, stderr = monitor.communicate(timeout=4)
                require(monitor.returncode == 0, f"EV event controller failed\n{stdout}\n{stderr}")
            finally:
                if monitor.poll() is None:
                    monitor.kill()
                    monitor.communicate()

    events = json.loads(stdout)
    expected = [{"state": "bad", "port": 2, "value": 0xC410, "drop_count": 0}]
    if not bad_only:
        expected.append({"state": "good", "port": 2, "value": 0xC410, "drop_count": 0})
    require(events == expected, f"Unexpected EV events for {operation}: {events}")
    print(json.dumps({"case": "ev_events_bad_only" if bad_only else "ev_events_failover",
                      "events": events, "tx_bytes": tx["bytes"], "rx_rail_packets": rx["received"]}))


def check_reliability_nacks(binary):
    for name, reason, size, count, limit in (
        ("no_bitmap", 0x06, 8192, 1, ("--rx-ooo-capacity", "0")),
        ("no_packet_buffer", 0x07, 5120, 2, ("--rx-staging-capacity", "0")),
        ("no_work_slot", 0x0A, 1024, 4, ("--rx-work-capacity", "1")),
    ):
        tx, rx = transfer(binary, size, "--exp-retries", "10", work_requests=count,
                          drop_after=1, rx_options=limit)
        expected = 1 << reason
        require(tx["nack_received_mask"] == rx["nack_sent_mask"] == expected,
                f"{name} did not generate the required NACK: {tx}, {rx}")
        require(tx["retries"][0] > 0 and tx["write_completions"] == count,
                f"{name} did not recover the rejected packet: {tx}, {rx}")
        print(json.dumps({"case": name, "tx": tx, "rx": rx}))

    tx, rx = transfer(binary, 1024, "--timeout-ms", "500", "--mpr", "1",
                      "--linear-retries", "1", "--exp-retries", "0",
                      tx_options=("--initial-psn", "128"),
                      expected_errors={"tx": "NACK-RX=0x800", "rx": "NACK-TX=0x800"})
    require("Retry limit exhausted for PSN 128" in tx and "RX stalled after 0 of 1" in rx and
            "state=ERROR work-errors=1 first-status=retry-exceeded work-flushed=0" in tx,
            f"An out-of-window packet was accepted or the retry limit was bypassed: {tx}, {rx}")
    print(json.dumps({"case": "outside_receive_window"}))

    tx, _ = transfer(binary, 1024, "--immediate", "23", work_requests=4, drop_after=1,
                     rx_options=("--rx-cq-capacity", "1"),
                     expected_errors={"tx": "Unexpected remote event at PSN",
                                      "rx": "NACK-TX=0x2000000"})
    require("NACK-RX=0x2000000" in tx and "Retry limit exhausted" not in tx and
            "state=ERROR work-errors=1 first-status=remote-operation" in tx,
            f"UNEXP_EVENT did not terminate the requester: {tx}")
    print(json.dumps({"case": "unexpected_completion_queue_overrun"}))


def check_path_mtu(binary):
    for pmtu, ipv6 in ((256, False), (512, True)):
        writes = [(0, None), (pmtu + 1, 0xffffffff), (2 * pmtu, None),
                  (pmtu - 1, 7), (0, 0)]
        tx, rx = transfer(binary, 0, "--pmtu", str(pmtu), writes=writes, ipv6=ipv6,
                          tx_options=("--initial-psn", "16777214"),
                          rx_options=("--initial-psn", "16777214"))
        require(tx["pmtu"] == rx["pmtu"] == pmtu and tx["packets"] == rx["packets"] == 7
                and rx["immediate_completions"] == 3,
                f"Wrong PMTU, segmentation, or WRITEIMM completions: {tx}, {rx}")
        print(json.dumps({"case": f"manual_pmtu_{pmtu}", "tx": tx, "rx": rx}))

    writes = [(0, None), (8197, 42), (49165, None), (513, 23), (0, 0)]
    for tx_pmtu, rx_pmtu in ((512, 256), (256, 512)):
        rx_options = ["--pmtu", str(rx_pmtu)]
        if rx_pmtu == 512:
            # This resize is beyond the packet count at the responder's
            # offered PMTU and occurs after it agrees to 256-byte segments.
            rx_options += ["--mpr", "8", "--mpr-change-at", "160", "--mpr-change-to", "1"]
        tx, rx = transfer(binary, 0, "--dynamic-mpr", "1", writes=writes, negotiate=True,
                          tx_options=("--pmtu", str(tx_pmtu)), rx_options=rx_options)
        expected_packets = sum(max(1, (length + 255) // 256) for length, _ in writes)
        require(tx["pmtu"] == rx["pmtu"] == 256 and tx["packets"] == rx["packets"] == expected_packets
                and tx["registered_memory"] and rx["registered_memory"] and rx["immediate_completions"] == 3,
                f"Negotiated PMTU did not rebuild mixed WRITE geometry: {tx}, {rx}")
        if rx_pmtu == 512:
            require(tx["mpr_updates"] == rx["mpr_updates"] == 1,
                    f"Packet-indexed resize did not use negotiated geometry: {tx}, {rx}")
        print(json.dumps({"case": f"negotiated_pmtu_{tx_pmtu}_to_{rx_pmtu}", "tx": tx, "rx": rx}))

    invalid = "Packet-indexed options do not fit 3 packets at PMTU 256"
    transfer(binary, 513, "--dynamic-mpr", "1", negotiate=True, drop_after=3,
             tx_options=("--pmtu", "512"),
             rx_options=("--pmtu", "256", "--mpr", "8", "--mpr-change-at", "3", "--mpr-change-to", "1"),
             expected_errors={"tx": invalid, "rx": invalid})
    print(json.dumps({"case": "reject_packet_index_after_pmtu_negotiation"}))


def check_partition_membership(binary):
    full, limited = "0xa345", "0x2345"
    for name, tx_key, rx_key, ipv6 in (
        ("full_to_limited", full, limited, False),
        ("limited_to_full", limited, full, True),
    ):
        tx, rx = transfer(binary, 131085, "--immediate", "23", ipv6=ipv6,
                          tx_options=("--p-key", tx_key), rx_options=("--p-key", rx_key))
        require(tx["sacks"] > 0 and rx["immediate_completions"] == 1 and
                rx["port_status_updates"] > 0 and tx["port_status_replies"] > 0,
                f"{name} did not complete data, reliability feedback, and Port Status: {tx}, {rx}")
        print(json.dumps({"case": f"pkey_{name}", "tx": tx, "rx": rx}))

    for name, tx_key, rx_key in (
        ("both_limited", limited, limited),
        ("different_partitions", full, "0xa346"),
    ):
        tx, rx = transfer(binary, 13, "--timeout-ms", "1200",
                          "--linear-retries", "1", "--exp-retries", "0",
                          tx_options=("--p-key", tx_key), rx_options=("--p-key", rx_key),
                          expected_errors={"tx": "Retry limit exhausted", "rx": "RX stalled after 0 of 1"})
        require("state=ERROR work-errors=1 first-status=retry-exceeded" in tx,
                f"{name} did not report a retry error: {tx}")
        paths = re.findall(r"RX frames=(\d+) matched=(\d+) decode-rejected=(\d+) other-peer=(\d+)", rx)
        require(len(paths) == 2 and any(int(frames) > 0 for frames, _, _, _ in paths) and
                all(int(frames) == int(other) and matched == rejected == "0"
                    for frames, matched, rejected, other in paths) and
                "data RX=0/0 SACK=0 transport-ACK=0" in rx,
                f"{name} did not reject the received frames by partition membership: {rx}")
        print(json.dumps({"case": f"pkey_{name}"}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path,
                        default=Path(__file__).resolve().parents[1] / "mrc_write")
    parser.add_argument("--controller", type=Path, help="Path to mrc_ctl_profile")
    parser.add_argument("--event-controller", type=Path,
                        help="Path to test_mrc_ctl_abi for cross-process EV event verification")
    parser.add_argument("--pkey-only", action="store_true", help="Run only the P_Key membership cases")
    parser.add_argument("--pmtu-only", action="store_true", help="Run only the path MTU cases")
    parser.add_argument("--rails-only", action="store_true", help="Run only the runtime rail cases")
    args = parser.parse_args()
    binary = args.binary.resolve()
    controller = (args.controller or binary.with_name("mrc_ctl_profile")).resolve()
    require(binary.is_file(), f"Build mrc_write first: {binary}")
    if args.pkey_only:
        check_partition_membership(binary)
        return
    if args.pmtu_only:
        check_path_mtu(binary)
        return
    if args.rails_only:
        check_rail_counts(binary)
        return
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
    require(tx["dropped"] > 0 and tx["retries"][0] > 0 and rx["received"][1] <= 512 and
            tx["ev_failed_mask"] & 2 and not (tx["ev_recovered_mask"] & 2),
            f"Blackholed path 1 did not stay failed or path 0 did not recover delivery: {tx}, {rx}")
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

    writes = [(0, None), (8196, 4294967295), (3075, None), (0, 0),
              (1025, None), (2055, 77), (13, None)]
    tx, rx = transfer(binary, 0, "--max-wimm-inflight", "1", "--udp-port", "4971",
                      writes=writes, negotiate=True, posted_receives=3, drop_after=1,
                      rx_options=("--max-wimm-inflight", "1"))
    expected_packets = sum(max(1, (length + 1023) // 1024) for length, _ in writes)
    require(tx["packets"] == expected_packets and tx["write_completions"] == len(writes) and
            rx["immediate_completions"] == 3 and tx["registered_memory"] and rx["registered_memory"],
            f"Mixed WRITE and WRITEIMM sequence failed: {tx}, {rx}")
    require(tx["dropped"] > 0 and tx["retries"][0] > 0 and rx["reorder_max"] > 0,
            f"Mixed work did not exercise reordering and retransmission: {tx}, {rx}")
    print(json.dumps({"case": "mixed_write_sizes_and_receive_sequences", "tx": tx, "rx": rx}))

    tx, _ = transfer(binary, 1024, "--immediate", "23", posted_receives=0, work_requests=3,
                     expected_errors={"tx": "syndrome 0x63", "rx": "MRC QP state=ERROR"})
    require("state=ERROR work-errors=1 first-status=remote-operation work-flushed=2" in tx,
            f"WRITEIMM without receive credit did not fail and flush the send queue: {tx}")
    print(json.dumps({"case": "write_imm_without_receive_credit"}))

    tx, _ = transfer(binary, 1024, work_requests=3,
                     tx_options=("--remote-key", "0x111111"),
                     expected_errors={"tx": "syndrome 0x62", "rx": "MRC QP state=ERROR"})
    require("state=ERROR work-errors=1 first-status=remote-access work-flushed=2" in tx,
            f"Invalid rkey did not report remote access error and flush the send queue: {tx}")
    print(json.dumps({"case": "invalid_remote_key"}))

    check_reliability_nacks(binary)

    tx, rx = transfer(binary, 8195, "--immediate", "99", negotiate=True, work_requests=8,
                      rx_options=("--service-time", "1"))
    require(rx["immediate_completions"] == 8, "Negotiated WRITEIMM sequence was incomplete")
    require(tx["registered_memory"] and rx["registered_memory"],
            "Negotiated transfer did not use provider-registered memory")
    print(json.dumps({"case": "negotiated_qp", "tx": tx, "rx": rx}))

    check_path_mtu(binary)
    check_partition_membership(binary)
    check_rail_counts(binary)

    for ipv6, case in ((True, "srv6_write_imm_trim_nack"), (False, "srv6_ipv4_write_imm_trim_nack")):
        tx, rx = transfer(binary, 41989, "--immediate", "22", trim_after=16,
                          ipv6=ipv6, srv6=True)
        require(rx["trimmed"] > 0 and tx["trim_nacks"] > 0 and rx["immediate_completed"],
                f"Preserved SRv6 wrapper or outer trim recovery failed: {tx}, {rx}")
        print(json.dumps({"case": case, "tx": tx, "rx": rx}))

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
    require(tx["ev_failed_mask"] & 2 and tx["ev_recovered_mask"] & 2 and rx["received"][1] > 256,
            f"Path 1 did not rejoin the active set after a correlated probe: {tx}, {rx}")
    print(json.dumps({"case": "recovered_ev", "tx": tx, "rx": rx}))

    if args.event_controller:
        check_controller_ev_events(binary, args.event_controller.resolve(), bad_only=False)
        check_controller_ev_events(binary, args.event_controller.resolve(), bad_only=True)

    change = ("--dynamic-mpr", "1", "--mpr", "8", "--mpr-change-at", "65",
              "--mpr-change-to", "1")
    tx, rx = transfer(binary, 262144, "--immediate", "99", "--max-wimm-inflight", "2",
                      "--ack-timeout", "13", negotiate=True, work_requests=8,
                      tx_options=("--dynamic-mpr", "1", "--mpr", "8"), rx_options=change)
    require(tx["mpr_updates"] == rx["mpr_updates"] == 1,
            f"Peers did not apply the negotiated receive-window shrink: {tx}, {rx}")
    require(tx["registered_memory"] and rx["registered_memory"] and rx["immediate_completions"] == 8,
            f"Negotiated WRITEIMM did not complete across the receive-window change: {tx}, {rx}")
    print(json.dumps({"case": "dynamic_mpr_shrink_during_writeimm", "tx": tx, "rx": rx}))

    packets = 255 * 128 + 1
    tx, rx = transfer(binary, packets * 256, "--immediate", "99", "--pmtu", "256", "--mpr", "255",
                      "--ack-timeout", "20", "--dynamic-mpr", "1", negotiate=True,
                      rx_options=("--mpr", "255", "--mpr-change-at", "512", "--mpr-change-to", "32"))
    require(tx["packets"] == rx["packets"] == packets and tx["mpr_updates"] == rx["mpr_updates"] == 1
            and tx["registered_memory"] and rx["registered_memory"] and rx["immediate_completed"],
            f"Full-range negotiated WRITEIMM failed: {tx}, {rx}")
    print(json.dumps({"case": "maximum_negotiated_mpr", "tx": tx, "rx": rx}))

    tx, rx = transfer(binary, 1048576, negotiate=True, rx_options=change)
    require(tx["mpr_updates"] == rx["mpr_updates"] == 0,
            "The receive window changed without bilateral capability negotiation")
    print(json.dumps({"case": "dynamic_mpr_not_negotiated", "tx": tx, "rx": rx}))


if __name__ == "__main__":
    main()
