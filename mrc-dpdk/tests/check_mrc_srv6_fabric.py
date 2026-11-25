#!/usr/bin/env python3
"""Run the OCP MRC endpoint through two Linux SRv6 forwarding planes."""

import argparse
import ipaddress
import json
import os
import subprocess
import struct
import sys
import tempfile
import time
from contextlib import ExitStack
from pathlib import Path

from memif_pair import require


INNER = {"tx": "2001:db8:3::11", "rx": "2001:db8:3::42"}
SID = {"tx": "2001:db8:2::11", "rx": "2001:db8:2::42"}
ROLES = ("tx", "rx")
MANAGEMENT = {"tx": "198.51.100.1", "rx": "198.51.100.2"}
OOB_PORT = "46720"
BYTES = 256 * 1024
LINK_TEST_BYTES = 4 * 1024 * 1024
JUMBO_PMTU = 4096
JUMBO_BYTES = 1024 * 1024 + 173
JUMBO_LINK_MTU = "4500"


def run(*command):
    result = subprocess.run(command, capture_output=True, text=True, timeout=5)
    require(result.returncode == 0, f"{' '.join(command)} failed:\n{result.stderr}")
    return result.stdout


def namespace_command(namespace, *command):
    return ["nsenter", "-t", str(namespace.pid), "-n", *command]


def namespace_run(namespace, *command):
    return run(*namespace_command(namespace, *command))


def wait_for(predicate, process, failure):
    deadline = time.monotonic() + 5
    while not predicate():
        require(process.poll() is None and time.monotonic() < deadline, failure())
        time.sleep(0.01)


def stop(process):
    if process.poll() is None:
        process.kill()
    process.wait(timeout=5)


def mac(role, rail, router=False):
    return f"02:00:00:00:{1 if router else 0:02x}:{16 * ROLES.index(role) + rail:02x}"


def open_namespace(resources):
    parent_ns = Path("/proc/self/ns/net").readlink()
    process = subprocess.Popen(["unshare", "-n", "sleep", "infinity"])
    resources.callback(stop, process)
    network = Path(f"/proc/{process.pid}/ns/net")
    wait_for(lambda: network.exists() and network.readlink() != parent_ns, process,
             lambda: "Failed to start a private fabric namespace")
    return process


def build_management(endpoints):
    run("ip", "link", "add", "tx-mgmt", "type", "veth", "peer", "name", "rx-mgmt")
    for role in ROLES:
        port, endpoint = f"{role}-mgmt", endpoints[role]
        run("ip", "link", "set", "dev", port, "netns", str(endpoint.pid))
        namespace_run(endpoint, "ip", "address", "add", f"{MANAGEMENT[role]}/30", "dev", port)
        namespace_run(endpoint, "ip", "link", "set", "dev", port, "up")


def connect_tap(endpoint, tap, ethernet):
    bridge = f"{tap}-bridge"
    namespace_run(endpoint, "ip", "link", "add", bridge, "mtu", JUMBO_LINK_MTU, "type", "bridge")
    # The DPDK TAP PMD opens a multiqueue device with packet information.
    namespace_run(endpoint, "ip", "tuntap", "add", "dev", tap, "mode", "tap", "pi", "multi_queue")
    namespace_run(endpoint, "ip", "link", "set", "dev", tap, "mtu", JUMBO_LINK_MTU, "master", bridge, "up")
    namespace_run(endpoint, "ip", "link", "set", "dev", ethernet, "master", bridge)
    namespace_run(endpoint, "ip", "link", "set", "dev", bridge, "up")


def build_rail(resources, endpoints, rail, decap_rail, jumbo):
    router = open_namespace(resources)
    namespace_run(router, "sysctl", "-qw", "net.ipv6.conf.all.forwarding=1")
    link_settings = ("mtu", JUMBO_LINK_MTU, "up") if jumbo else ("up",)
    for role in ROLES:
        port, remote = f"{role}{rail}", f"to-{role}"
        ethernet = f"{port}-link" if jumbo else port
        address = () if jumbo else ("address", mac(role, rail))
        run("ip", "link", "add", ethernet, *address, "type", "veth",
            "peer", "name", remote, "address", mac(role, rail, router=True),
            "netns", str(router.pid))
        endpoint = endpoints[role]
        run("ip", "link", "set", "dev", ethernet, "netns", str(endpoint.pid))
        namespace_run(endpoint, "ip", "link", "set", "dev", ethernet, *link_settings)
        if jumbo:
            connect_tap(endpoint, port, ethernet)
        namespace_run(router, "ip", "link", "set", "dev", remote, *link_settings)
        namespace_run(router, "sysctl", "-qw", f"net.ipv6.conf.{remote}.seg6_enabled=1")

    def forward(destination, role):
        dev = f"to-{role}"
        namespace_run(router, "ip", "-6", "route", "add", f"{destination}/128", "dev", dev)
        namespace_run(router, "ip", "-6", "neigh", "replace", destination, "lladdr",
                      mac(role, rail), "nud", "permanent", "dev", dev)

    forward(SID["tx"], "tx")
    stage = f"2001:db8:1::{rail + 1}"
    namespace_run(router, "ip", "-6", "route", "add", f"{stage}/128", "encap", "seg6local",
                  "action", "End", "count", "dev", "to-tx")
    if rail != decap_rail:
        forward(SID["rx"], "rx")
    else:
        forward(INNER["rx"], "rx")
        namespace_run(router, "ip", "-6", "route", "add", f"{SID['rx']}/128", "encap",
                      "seg6local", "action", "End.DX6", "nh6", INNER["rx"], "count", "dev", "to-rx")
    return router, stage


def route_stats(router, sid):
    routes = json.loads(namespace_run(router, "ip", "-6", "-j", "-s", "route", "show", "exact", f"{sid}/128"))
    require(len(routes) == 1 and "stats64" in routes[0], f"Missing SRv6 counters for {sid}: {routes}")
    return routes[0]["stats64"]


def add_ingress_chain(router):
    namespace_run(router, "nft", "add", "table", "netdev", "mrc_fabric")
    namespace_run(router, "nft", "add", "chain", "netdev", "mrc_fabric", "ingress",
                  "{ type filter hook ingress device to-tx priority 0; policy accept; }")


def mark_ecn(router, decapsulates=False):
    add_ingress_chain(router)
    namespace_run(router, "nft", "add", "rule", "netdev", "mrc_fabric", "ingress",
                  "ip6", "ecn", "ect0", "numgen", "inc", "mod", "16", "15", "counter", "ip6", "ecn", "set", "ce")
    # A second rule observes the resulting packet bits, not just the number
    # selected by the marking policy. Neither rule models queue-driven WRED.
    namespace_run(router, "nft", "add", "rule", "netdev", "mrc_fabric", "ingress",
                  "ip6", "ecn", "ce", "counter")
    if decapsulates:
        # This kernel's End.DX6 removes outer CE without copying it inward.
        # Apply the RFC 6040 CE merge explicitly for this fixture's fixed
        # IPv6/SRH/IPv6 stack. The inner ECN mask must be nonzero: Not-ECT
        # packets are never converted to CE by this rule.
        namespace_run(router, "nft", "add", "counter", "netdev", "mrc_fabric", "decap_propagated")
        namespace_run(router, "nft", "add", "rule", "netdev", "mrc_fabric", "ingress",
                      "ip6", "saddr", SID["tx"], "ip6", "nexthdr", "43", "ip6", "ecn", "ce",
                      "@nh,512,16", "&", "0x30", "!=", "0",
                      "counter", "name", "decap_propagated",
                      "@nh,512,16", "set", "@nh,512,16", "|", "0x30")
        namespace_run(router, "nft", "add", "chain", "netdev", "mrc_fabric", "after_decap",
                      "{ type filter hook egress device to-rx priority 0; policy accept; }")
        namespace_run(router, "nft", "add", "counter", "netdev", "mrc_fabric", "decap_ce")
        namespace_run(router, "nft", "add", "rule", "netdev", "mrc_fabric", "after_decap",
                      "ip6", "saddr", INNER["tx"], "ip6", "daddr", INNER["rx"],
                      "ip6", "ecn", "ce", "counter", "name", "decap_ce")
    namespace_run(router, "nft", "add", "chain", "netdev", "mrc_fabric", "feedback",
                  "{ type filter hook ingress device to-rx priority 0; policy accept; }")
    for rail in range(2):
        name = f"skip_ev_{rail}"
        namespace_run(router, "nft", "add", "counter", "netdev", "mrc_fabric", name)
        # This fixture returns a single-container SRH: outer IPv6 (40), SRH
        # (24), inner IPv6 (40), UDP (8), BTH (12), then SETH. Check the SACK
        # opcode, SKIP mark, and reflected request EV independently of ingress.
        entropy = ((0x0100 + rail) << 16) | (0xC000 + rail)
        namespace_run(router, "nft", "add", "rule", "netdev", "mrc_fabric", "feedback",
                      "ip6", "saddr", SID["rx"], "ip6", "nexthdr", "43",
                      "@nh,896,8", "0xdc", "@nh,1000,8", "&", "0x60", "==", "0x20",
                      "@nh,1024,32", hex(entropy), "counter", "name", name)


def ecn_observation(router, decapsulates=False):
    table = json.loads(namespace_run(router, "nft", "-j", "list", "table", "netdev", "mrc_fabric"))
    rules = [item["rule"] for item in table["nftables"]
             if "rule" in item and item["rule"]["chain"] == "ingress"]
    require(len(rules) == (3 if decapsulates else 2), f"Missing ECN marking/observation rules: {table}")
    require(any(expr.get("mangle") == {"key": {"payload": {"protocol": "ip6", "field": "ecn"}},
                                      "value": "ce"} for expr in rules[0]["expr"]),
            f"Installed switch rule does not set CE: {table}")
    counts = [next(expr["counter"]["packets"] for expr in rule["expr"] if "counter" in expr)
              for rule in rules[:2]]
    require(counts[0] > 0 and counts[0] == counts[1],
            f"Selected packets did not acquire CE on this switch: {counts}")
    named = {item["counter"]["name"]: item["counter"]["packets"]
             for item in table["nftables"] if "counter" in item}
    result = {"policy": "deterministic_every_16th_ect0", "marked": counts[0],
              "ce_after_policy": counts[1], "skip_sacks_by_ev": [named[f"skip_ev_{rail}"] for rail in range(2)]}
    if decapsulates:
        # Read back the Not-ECT exclusion rather than relying on the command
        # succeeding; payload offset 512 is the inner IPv6 header after SRH.
        matches = [expr["match"] for expr in rules[2]["expr"] if "match" in expr]
        guard = {"op": "!=", "left": {"&": [{"payload": {"base": "nh", "offset": 512, "len": 16}}, 48]},
                 "right": 0}
        require(guard in matches,
                f"CE propagation lacks its inner Not-ECT guard: {rules[2]}")
        require(named["decap_propagated"] == named["decap_ce"] == counts[0],
                f"CE was not preserved into each decapsulated packet: {named}, {counts}")
        result.update(decap_policy="explicit_ect_inner_ce_merge", decap_propagated=named["decap_propagated"],
                      ce_after_decap=named["decap_ce"])
    return result


def check_ecn_decap_policy():
    """Exercise the configured merge with all four inner ECN codepoints.

    These synthetic IPv6 packets contain no MRC traffic and run in separate
    namespaces so their counters cannot satisfy the actual transfer checks.
    This checks preservation only; it does not claim full RFC 6040 behavior
    for an outer-CE/inner-Not-ECT combination (which would require dropping).
    """
    with ExitStack() as resources:
        endpoints = {role: open_namespace(resources) for role in ROLES}
        router, stage = build_rail(resources, endpoints, 1, 1, False)
        mark_ecn(router, decapsulates=True)
        namespace_run(router, "nft", "add", "counter", "netdev", "mrc_fabric", "decap_not_ect")
        namespace_run(router, "nft", "add", "rule", "netdev", "mrc_fabric", "after_decap",
                      "ip6", "saddr", INNER["tx"], "ip6", "daddr", INNER["rx"],
                      "ip6", "ecn", "not-ect", "counter", "name", "decap_not_ect")
        address = lambda value: ipaddress.IPv6Address(value).packed
        ethernet = bytes.fromhex(mac("tx", 1, True).replace(":", ""))
        ethernet += bytes.fromhex(mac("tx", 1).replace(":", "")) + b"\x86\xdd"
        srh = struct.pack("!BBBBBBH", 41, 2, 4, 1, 0, 0, 0) + address(SID["rx"])
        packets = []
        for inner_ecn in range(4):
            inner = struct.pack("!IHBB16s16s", (6 << 28) | (inner_ecn << 20), 8, 17, 64,
                                address(INNER["tx"]), address(INNER["rx"]))
            inner += struct.pack("!HHHH", 43210, 43211, 8, 0)
            outer = struct.pack("!IHBB16s16s", (6 << 28) | (3 << 20), len(srh) + len(inner),
                                43, 64, address(SID["tx"]), address(stage))
            packets.append((ethernet + outer + srh + inner).hex())
        namespace_run(endpoints["tx"], sys.executable, "-c",
                      "import socket,sys; s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW); "
                      "s.bind((sys.argv[1],0)); [s.send(bytes.fromhex(p)) for p in sys.argv[2:]]",
                      "tx1", *packets)
        deadline = time.monotonic() + 2
        while True:
            table = json.loads(namespace_run(router, "nft", "-j", "list", "table", "netdev", "mrc_fabric"))
            counts = {item["counter"]["name"]: item["counter"]["packets"]
                      for item in table["nftables"] if "counter" in item}
            if counts["decap_ce"] + counts["decap_not_ect"] == 4:
                break
            require(time.monotonic() < deadline, f"Missing ECN guard test packets: {counts}")
            time.sleep(0.01)
        require(counts["decap_propagated"] == counts["decap_ce"] == 3 and counts["decap_not_ect"] == 1,
                f"ECT merge corrupted Not-ECT or lost CE: {counts}")
        return {"inner_not_ect_preserved": 1, "inner_ect_or_ce_preserved_as_ce": 3}


def ingress_packets(router):
    rules = json.loads(namespace_run(router, "nft", "-j", "list", "chain", "netdev", "mrc_fabric", "ingress"))
    counters = [expr["counter"] for item in rules["nftables"]
                for expr in item.get("rule", {}).get("expr", []) if "counter" in expr]
    require(len(counters) == 1, f"Expected one ingress rule counter: {rules}")
    return counters[0]["packets"]


def count_jumbo_packets(router):
    add_ingress_chain(router)
    namespace_run(router, "nft", "add", "rule", "netdev", "mrc_fabric", "ingress",
                  "ip6", "saddr", SID["tx"], "ip6", "nexthdr", "43",
                  "ip6", "length", ">", str(JUMBO_PMTU), "counter")


def hold_negotiation(receiver):
    namespace_run(receiver, "nft", "add", "table", "ip", "mrc_oob")
    namespace_run(receiver, "nft", "add", "chain", "ip", "mrc_oob", "input",
                  "{ type filter hook input priority 0; policy accept; }")
    namespace_run(receiver, "nft", "add", "rule", "ip", "mrc_oob", "input",
                  "tcp", "dport", OOB_PORT, "reject", "with", "tcp", "reset")


def configure_taps(endpoint, role):
    for rail in range(2):
        # TAP probe sets the Linux MTU to 1500 and programs the DPDK MAC.
        # Change the TAP's kernel MAC so the bridge forwards frames for
        # the DPDK MAC through the TAP fd.
        address = f"06:00:00:00:00:{16 * ROLES.index(role) + rail:02x}"
        namespace_run(endpoint, "ip", "link", "set", "dev", f"{role}{rail}",
                      "mtu", JUMBO_LINK_MTU, "address", address)


def prepare_link_fault(routers):
    for router, _ in routers:
        namespace_run(router, "tc", "qdisc", "add", "dev", "to-rx", "root", "netem", "rate", "20mbit")
    router = routers[0][0]
    add_ingress_chain(router)
    # Outer IPv6, one 24-byte SRH, inner IPv6, UDP, then the BTH and ERTH.
    # Check the endpoint request opcode, ERTH Port Status operation and mask.
    for name, mask in (("both_rails", "3"), ("surviving_rail", "1")):
        namespace_run(router, "nft", "add", "counter", "netdev", "mrc_fabric", name)
        namespace_run(router, "nft", "add", "rule", "netdev", "mrc_fabric", "ingress",
                      "ip6", "saddr", SID["tx"], "ip6", "nexthdr", "43",
                      "@nh,896,8", "0xd8", "@nh,1000,8", "0", "@nh,1024,32", mask,
                      "counter", "name", name)


def watch_link_fault(sender, endpoints, routers):
    router = routers[0][0]

    def reports(name):
        data = json.loads(namespace_run(router, "nft", "-j", "list", "counter", "netdev", "mrc_fabric", name))
        return next(item["counter"]["packets"] for item in data["nftables"] if "counter" in item)

    def forwarded():
        return [route_stats(node, stage)["packets"] for node, stage in routers]

    wait_for(lambda: reports("both_rails") > 0 and min(forwarded()) >= 64, sender,
             lambda: f"No baseline traffic on both rails: {forwarded()}")
    old_down_reports = reports("surviving_rail")
    namespace_run(endpoints["tx"], "ip", "link", "set", "dev", "tx1", "down")
    wait_for(lambda: reports("surviving_rail") > old_down_reports, sender,
             lambda: "No Port Status update with only rail 0 after kernel link down")
    before = forwarded()
    wait_for(lambda: forwarded()[0] >= before[0] + 128, sender,
             lambda: f"Traffic did not continue on the surviving rail: {forwarded()}")
    during = forwarded()
    require(during[1] == before[1], f"The down rail forwarded traffic: {before}, {during}")

    old_up_reports = reports("both_rails")
    namespace_run(endpoints["tx"], "ip", "link", "set", "dev", "tx1", "up")
    wait_for(lambda: reports("both_rails") > old_up_reports, sender,
             lambda: "No Port Status update with both rails after kernel link up")
    wait_for(lambda: forwarded()[1] >= during[1] + 64, sender,
             lambda: f"Traffic did not resume on rail 1: {forwarded()}")
    return {"port_status_masks": [3, 1, 3], "forwarded_before": before,
            "forwarded_while_down": during, "forwarded_after_restore": forwarded()}


def command(args, role, cpu, directory, profile):
    cmd = [str(args.binary), f"--lcores=0@{cpu}", "--no-huge", "--no-pci",
           "--no-telemetry", f"--file-prefix={directory.name}-{role}", "-m", "64"]
    driver = args.tap_driver if args.jumbo else args.af_packet_driver
    if driver:
        cmd += ["-d", str(driver)]
    for rail in range(2):
        vdev = f"net_tap{rail},iface={role}{rail},mac={mac(role, rail)}" if args.jumbo else (
            f"net_af_packet{rail},iface={role}{rail}"
        )
        cmd += [f"--vdev={vdev}"]
    peer = "tx" if role == "rx" else "rx"
    cmd += ["--", role, str(args.bytes), INNER[role], INNER[peer],
            "0", mac(role, 0, router=True), "1", mac(role, 1, router=True),
            "--srv6-local", SID[role], "--srv6-peer", SID[peer], "--srv6-srh", "1",
            "--ack-timeout", "20" if args.fault == "link" else "18",
            "--immediate", "99", "--dynamic-mpr", "1",
            "--oob-host", MANAGEMENT["rx"], "--oob-port", OOB_PORT]
    if args.jumbo:
        cmd += ["--pmtu", str(JUMBO_PMTU)]
    if role == "tx":
        cmd += ["--profile-dir", str(directory), "--profile-id", profile]
        if args.fault == "trim":
            cmd += ["--trim-path-after", "64"]
    else:
        cmd += ["--mpr-change-at", "65", "--mpr-change-to", "2"]
    return cmd


def transfer(args, directory, resources, endpoints, routers):
    cpus = sorted(os.sched_getaffinity(0))
    require(len(cpus) >= 2, "The fabric test needs two polling cores")
    profile = run(str(args.controller), "--seed-generated-srv6-srh", str(directory)).strip()
    require(profile.isdecimal() and int(profile) > 0, f"Invalid controller profile ID: {profile!r}")
    env = {**os.environ, "RUNTIME_DIRECTORY": str(directory), "XDG_RUNTIME_DIR": str(directory)}

    def start(role, cpu):
        path = directory / f"{role}.log"
        with path.open("w") as error:
            launch = namespace_command(endpoints[role], *command(args, role, cpu, directory, profile))
            process = subprocess.Popen(launch,
                                       stdout=subprocess.PIPE, stderr=error, env=env, text=True)
        resources.callback(stop, process)
        return process, path

    last_port = "net_tap1" if args.jumbo else "net_af_packet1"

    def wait_for_ports(process, log):
        wait_for(lambda: f"DPDK port 1: {last_port}" in log.read_text(), process,
                 lambda: f"Endpoint did not open both rails:\n{log.read_text()}")

    if args.jumbo:
        hold_negotiation(endpoints["rx"])
    receiver, rx_log = start("rx", cpus[0])
    wait_for_ports(receiver, rx_log)
    if args.jumbo:
        configure_taps(endpoints["rx"], "rx")
    sender, tx_log = start("tx", cpus[1])
    if args.jumbo:
        wait_for_ports(sender, tx_log)
        configure_taps(endpoints["tx"], "tx")
        namespace_run(endpoints["rx"], "nft", "delete", "table", "ip", "mrc_oob")
    link = watch_link_fault(sender, endpoints, routers) if args.fault == "link" else None
    results = []
    for process, log in ((sender, tx_log), (receiver, rx_log)):
        stdout, _ = process.communicate(timeout=12)
        require(process.returncode == 0, f"Endpoint exited {process.returncode}:\n{stdout}{log.read_text()}")
        results.append(json.loads(stdout.strip().splitlines()[-1]))
    tx, rx = results
    return tx, rx, link


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=root / "mrc_write")
    parser.add_argument("--controller", type=Path, default=root / "tests/test_mrc_ctl_abi")
    parser.add_argument("--af-packet-driver", type=Path)
    parser.add_argument("--tap-driver", type=Path)
    scenario = parser.add_mutually_exclusive_group()
    scenario.add_argument("--fault", choices=("ecn", "trim", "link"))
    scenario.add_argument("--jumbo", action="store_true", help="verify negotiation and forwarding at PMTU 4096")
    args = parser.parse_args()
    for name in ("binary", "controller", "af_packet_driver", "tap_driver"):
        path = getattr(args, name)
        if path is not None:
            require(path.is_file(), f"Missing {name}: {path}")
            setattr(args, name, path.resolve())
    args.bytes = LINK_TEST_BYTES if args.fault == "link" else BYTES
    if args.jumbo:
        args.bytes = JUMBO_BYTES
    require([link["ifname"] for link in json.loads(run("ip", "-j", "link", "show"))] == ["lo"],
            "Run inside an empty network namespace with unshare -Urn")
    ecn_policy_test = check_ecn_decap_policy() if args.fault == "ecn" else None
    with tempfile.TemporaryDirectory(prefix="mrc-fabric-") as temp, ExitStack() as resources:
        endpoints = {role: open_namespace(resources) for role in ROLES}
        build_management(endpoints)
        decap_rail = 0 if args.fault == "trim" else 1
        routers = [build_rail(resources, endpoints, rail, decap_rail, args.jumbo) for rail in range(2)]
        if args.fault == "ecn":
            for rail, (router, _) in enumerate(routers):
                mark_ecn(router, decapsulates=rail == decap_rail)
        elif args.fault == "link":
            prepare_link_fault(routers)
        elif args.jumbo:
            for router, _ in routers:
                count_jumbo_packets(router)
        tx, rx, link = transfer(args, Path(temp), resources, endpoints, routers)
        transit = [route_stats(router, stage) for router, stage in routers]
        decap = route_stats(routers[decap_rail][0], SID["rx"])
        require(tx["bytes"] == rx["bytes"] == args.bytes and sum(rx["received"]) == tx["packets"]
                and min(rx["received"]) > 0 and tx["write_completions"] == rx["immediate_completions"] == 1
                and tx["registered_memory"] and rx["registered_memory"]
                and tx["mpr_updates"] == rx["mpr_updates"] == 1,
                f"Incorrect WRITEIMM delivery or negotiated MPR: {tx}, {rx}")
        require(all(stats["packets"] >= count and stats["errors"] == 0
                    for stats, count in zip(transit, rx["received"]))
                and decap["packets"] >= rx["received"][decap_rail] and decap["errors"] == 0,
                f"Missing SRv6 forwarding or decapsulation: {transit}, {decap}")
        results = {"case": "linux_srv6_fabric", "fault": args.fault, "tx": tx, "rx": rx,
                   "end": transit, "decap_rail": decap_rail, "end_dx6": decap}
        if args.fault == "ecn":
            results["ecn_policy_selftest"] = ecn_policy_test
            results["ecn_switches"] = [ecn_observation(router, rail == decap_rail)
                                       for rail, (router, _) in enumerate(routers)]
            results["ecn_marked"] = sum(item["marked"] for item in results["ecn_switches"])
            reflected = [sum(item["skip_sacks_by_ev"][rail] for item in results["ecn_switches"])
                         for rail in range(2)]
            require(min(reflected) > 0 and tx["ev_skipped"] >= 2,
                    f"Both preserved and decapsulated CE marks must reach MRC feedback: {results}")
        elif args.fault == "trim":
            require(tx["trimmed"] > 0 and rx["trim_nacks"] > 0 and tx["trim_nacks"] > 0
                    and rx["nack_sent_mask"] & 2 and tx["nack_received_mask"] & 2
                    and tx["retries"][0] > 0, f"Trim did not trigger alternate-rail delivery: {results}")
        elif args.fault == "link":
            results["link"] = link
        elif args.jumbo:
            jumbo_frames = [ingress_packets(router) for router, _ in routers]
            full_packets = JUMBO_BYTES // JUMBO_PMTU
            require(tx["pmtu"] == rx["pmtu"] == JUMBO_PMTU and tx["packets"] == full_packets + 1
                    and min(jumbo_frames) > 0
                    and all(large >= received - 1 for large, received in zip(jumbo_frames, rx["received"]))
                    and sum(jumbo_frames) >= full_packets,
                    f"Missing full-sized WRITEIMM packets on the jumbo rails: {tx}, {rx}, {jumbo_frames}")
            results["jumbo_frames"] = jumbo_frames
        print(json.dumps(results))


if __name__ == "__main__":
    main()
