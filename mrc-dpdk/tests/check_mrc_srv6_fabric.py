#!/usr/bin/env python3
"""Run the OCP MRC endpoint through two Linux SRv6 forwarding planes."""

import argparse
import json
import os
import subprocess
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


def build_rail(resources, endpoints, rail, decap_rail):
    router = open_namespace(resources)
    namespace_run(router, "sysctl", "-qw", "net.ipv6.conf.all.forwarding=1")
    for role in ROLES:
        port, remote = f"{role}{rail}", f"to-{role}"
        run("ip", "link", "add", port, "address", mac(role, rail), "type", "veth",
            "peer", "name", remote, "address", mac(role, rail, router=True),
            "netns", str(router.pid))
        endpoint = endpoints[role]
        run("ip", "link", "set", "dev", port, "netns", str(endpoint.pid))
        namespace_run(endpoint, "ip", "link", "set", "dev", port, "up")
        namespace_run(router, "ip", "link", "set", "dev", remote, "up")
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


def mark_ecn(router):
    namespace_run(router, "nft", "add", "table", "netdev", "mrc_fabric")
    namespace_run(router, "nft", "add", "chain", "netdev", "mrc_fabric", "ingress",
                  "{ type filter hook ingress device to-tx priority 0; policy accept; }")
    namespace_run(router, "nft", "add", "rule", "netdev", "mrc_fabric", "ingress",
                  "ip6", "ecn", "ect0", "numgen", "inc", "mod", "16", "15", "counter", "ip6", "ecn", "set", "ce")


def ecn_stats(router):
    rules = json.loads(namespace_run(router, "nft", "-j", "list", "chain", "netdev", "mrc_fabric", "ingress"))
    counters = [expr["counter"] for item in rules["nftables"]
                for expr in item.get("rule", {}).get("expr", []) if "counter" in expr]
    require(len(counters) == 1, f"Expected an ECN rule counter: {rules}")
    return counters[0]["packets"]


def command(args, role, cpu, directory, profile):
    cmd = [str(args.binary), f"--lcores=0@{cpu}", "--no-huge", "--no-pci",
           "--no-telemetry", f"--file-prefix={directory.name}-{role}", "-m", "64"]
    if args.af_packet_driver:
        cmd += ["-d", str(args.af_packet_driver)]
    for rail in range(2):
        cmd += [f"--vdev=net_af_packet{rail},iface={role}{rail}"]
    peer = "tx" if role == "rx" else "rx"
    cmd += ["--", role, str(BYTES), INNER[role], INNER[peer],
            "0", mac(role, 0, router=True), "1", mac(role, 1, router=True),
            "--srv6-local", SID[role], "--srv6-peer", SID[peer], "--srv6-srh", "1",
            "--ack-timeout", "18", "--immediate", "99", "--dynamic-mpr", "1",
            "--oob-host", MANAGEMENT["rx"], "--oob-port", OOB_PORT]
    if role == "tx":
        cmd += ["--profile-dir", str(directory), "--profile-id", profile]
        if args.fault == "trim":
            cmd += ["--trim-path-after", "64"]
    else:
        cmd += ["--mpr-change-at", "65", "--mpr-change-to", "2"]
    return cmd


def transfer(args, directory, resources, endpoints):
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

    receiver, rx_log = start("rx", cpus[0])
    wait_for(lambda: "DPDK port 1: net_af_packet1" in rx_log.read_text(), receiver,
             lambda: f"Receiver did not open both rails:\n{rx_log.read_text()}")
    sender, tx_log = start("tx", cpus[1])
    results = []
    for process, log in ((sender, tx_log), (receiver, rx_log)):
        stdout, _ = process.communicate(timeout=12)
        require(process.returncode == 0, f"Endpoint exited {process.returncode}:\n{stdout}{log.read_text()}")
        results.append(json.loads(stdout.strip().splitlines()[-1]))
    return results


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=root / "mrc_write")
    parser.add_argument("--controller", type=Path, default=root / "tests/test_mrc_ctl_abi")
    parser.add_argument("--af-packet-driver", type=Path)
    parser.add_argument("--fault", choices=("ecn", "trim"))
    args = parser.parse_args()
    for name in ("binary", "controller", "af_packet_driver"):
        path = getattr(args, name)
        if path is not None:
            require(path.is_file(), f"Missing {name}: {path}")
            setattr(args, name, path.resolve())
    require([link["ifname"] for link in json.loads(run("ip", "-j", "link", "show"))] == ["lo"],
            "Run inside an empty network namespace with unshare -Urn")
    with tempfile.TemporaryDirectory(prefix="mrc-fabric-") as temp, ExitStack() as resources:
        endpoints = {role: open_namespace(resources) for role in ROLES}
        build_management(endpoints)
        decap_rail = 0 if args.fault == "trim" else 1
        routers = [build_rail(resources, endpoints, rail, decap_rail) for rail in range(2)]
        if args.fault == "ecn":
            mark_ecn(routers[0][0])
        tx, rx = transfer(args, Path(temp), resources, endpoints)
        transit = [route_stats(router, stage) for router, stage in routers]
        decap = route_stats(routers[decap_rail][0], SID["rx"])
        require(tx["bytes"] == rx["bytes"] == BYTES and sum(rx["received"]) == tx["packets"]
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
            results["ecn_marked"] = ecn_stats(routers[0][0])
            require(results["ecn_marked"] > 0 and tx["ev_skipped"] > 0,
                    f"Kernel CE marks did not reach MRC feedback: {results}")
        elif args.fault == "trim":
            require(tx["trimmed"] > 0 and rx["trim_nacks"] > 0 and tx["trim_nacks"] > 0
                    and rx["nack_sent_mask"] & 2 and tx["nack_received_mask"] & 2
                    and tx["retries"][0] > 0, f"Trim did not trigger alternate-rail delivery: {results}")
        print(json.dumps(results))


if __name__ == "__main__":
    main()
