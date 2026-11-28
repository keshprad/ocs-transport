#!/usr/bin/env python3
"""Switch the three-Spark ring's QSFP-facing interfaces between RoCE modes."""

from argparse import ArgumentParser
import fcntl
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time


PORTS = (
    ("enp1s0f0np0", "0000:01:00.0"),
    ("enP2p1s0f0np0", "0002:01:00.0"),
    ("enp1s0f1np1", "0000:01:00.1"),
    ("enP2p1s0f1np1", "0002:01:00.1"),
)
ADDRESS_SUFFIXES = {
    "spark-28a3": (6, 18, 9, 21),
    "spark-4ed9": (10, 22, 1, 13),
    "spark-7e64": (2, 14, 5, 17),
}
IP = "/usr/sbin/ip"
DEVLINK = "/usr/sbin/devlink"
STATE = Path("/var/lib/ocs-roce-ethernet/state.json")


def run(*args):
    return subprocess.check_output(args, text=True)


def roce_mode(pci):
    output = run(DEVLINK, "dev", "param", "show", f"pci/{pci}", "name", "enable_roce")
    values = re.findall(r"cmode driverinit value (true|false)", output)
    if len(values) != 1:
        raise ValueError(f"Cannot determine RoCE mode on {pci}")
    return values[0]


def interface_state(interface, pci, address):
    device = Path(f"/sys/class/net/{interface}/device").resolve(strict=True)
    if device.name != pci:
        raise ValueError(f"{interface} maps to {device.name}, expected {pci}")
    info, = json.loads(run(IP, "-j", "link", "show", "dev", interface))
    ipv4 = json.loads(run(IP, "-j", "-4", "address", "show", "dev", interface))
    addresses = []
    for item in ipv4[0].get("addr_info", []) if ipv4 else ():
        saved = {"cidr": f"{item['local']}/{item['prefixlen']}"}
        for key in ("broadcast", "label"):
            if key in item:
                saved[key] = item[key]
        if item.get("noprefixroute"):
            saved["noprefixroute"] = True
        addresses.append(saved)
    return {
        "interface": interface, "pci": pci, "mtu": info["mtu"], "up": "UP" in info["flags"],
        "roce": roce_mode(pci), "addresses": addresses, "dpdk_address": address, "reload_pending": False,
    }


def save(state):
    pending = STATE.with_suffix(".tmp")
    pending.write_text(json.dumps(state, indent=2) + "\n")
    pending.replace(STATE)


def set_mode(state, entry, target):
    pci = entry["pci"]
    current = roce_mode(pci)
    if current == target and not entry["reload_pending"]:
        return
    entry["reload_pending"] = True
    save(state)
    if current != target:
        run(DEVLINK, "dev", "param", "set", f"pci/{pci}", "name", "enable_roce",
            "value", target, "cmode", "driverinit")
    run(DEVLINK, "dev", "reload", f"pci/{pci}", "action", "driver_reinit")
    if roce_mode(pci) != target:
        raise ValueError(f"{pci} did not accept enable_roce={target}")
    entry["reload_pending"] = False
    save(state)


def update_link(entry, raw, target):
    interface, pci, dpdk_address = (entry[key] for key in ("interface", "pci", "dpdk_address"))
    deadline = time.monotonic() + 10
    while not Path(f"/sys/class/net/{interface}/device").exists() and time.monotonic() < deadline:
        time.sleep(0.2)
    current = interface_state(interface, pci, dpdk_address)
    mtu = 1500 if raw else entry["mtu"]
    up = raw or entry["up"]
    if current["mtu"] != mtu:
        run(IP, "link", "set", "dev", interface, "mtu", str(mtu))
    if current["up"] != up:
        run(IP, "link", "set", "dev", interface, "up" if up else "down")

    original = {item["cidr"] for item in entry["addresses"]}
    present = {item["cidr"] for item in current["addresses"]}
    desired = entry["addresses"][:]
    if raw and dpdk_address not in original:
        desired.append({"cidr": dpdk_address})
    if not raw and dpdk_address in present - original:
        run(IP, "-4", "address", "del", dpdk_address, "dev", interface)
    for item in desired:
        if item["cidr"] in present:
            continue
        args = [IP, "-4", "address", "add", item["cidr"], "dev", interface]
        for key in ("broadcast", "label"):
            if key in item:
                args.extend((key, item[key]))
        if item.get("noprefixroute"):
            args.append("noprefixroute")
        run(*args)

    updated = interface_state(interface, pci, dpdk_address)
    expected = {item["cidr"] for item in desired}
    active = {item["cidr"] for item in updated["addresses"]}
    if updated["roce"] != target or not expected <= active or updated["mtu"] != mtu or updated["up"] != up:
        raise ValueError(f"{interface} did not reach the requested Ethernet configuration")
    if not raw and dpdk_address not in original and dpdk_address in active:
        raise ValueError(f"{interface} still has temporary address {dpdk_address}")
    print(f"{interface} {pci} enable_roce={updated['roce']} mtu={mtu} IPv4={','.join(sorted(active))}", flush=True)


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument(
        "action", choices=("status", "raw", "rocev2", "restore"),
        help="raw configures raw Ethernet for DPDK, rocev2 enables hardware RoCE, "
             "restore reapplies the exact saved settings",
    )
    args = parser.parse_args()
    host = socket.gethostname().split(".")[0]
    if host not in ADDRESS_SUFFIXES:
        parser.error(f"{host} is outside the three-Spark ring")
    ports = [(interface, pci, f"10.61.0.{suffix}/30")
             for (interface, pci), suffix in zip(PORTS, ADDRESS_SUFFIXES[host], strict=True)]
    if args.action == "status":
        for port in ports:
            entry = interface_state(*port)
            print(entry["interface"], entry["pci"], "enable_roce=" + entry["roce"],
                  "mtu=" + str(entry["mtu"]), *(item["cidr"] for item in entry["addresses"]))
        return
    if os.geteuid() != 0:
        parser.error(f"{args.action} requires sudo")

    os.umask(0o077)
    STATE.parent.mkdir(mode=0o700, exist_ok=True)
    with (STATE.parent / "lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if STATE.exists():
            state = json.loads(STATE.read_text())
            saved_ports = [(entry["interface"], entry["pci"], entry["dpdk_address"])
                           for entry in state["interfaces"]]
            if state["host"] != host or saved_ports != ports:
                raise ValueError(f"Saved settings in {STATE} do not match this host")
        elif args.action in ("raw", "rocev2"):
            state = {"host": host, "interfaces": [interface_state(*port) for port in ports]}
            save(state)
        else:
            raise ValueError(f"No saved state in {STATE}")

        raw = args.action == "raw"
        requested_mode = {"raw": "false", "rocev2": "true"}.get(args.action)
        for entry in state["interfaces"]:
            set_mode(state, entry, requested_mode or entry["roce"])
        # A function reload can also clear its sibling interface's addresses.
        for entry in state["interfaces"]:
            update_link(entry, raw, requested_mode or entry["roce"])
        if not raw:
            STATE.unlink()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"{exc}. Saved settings, if present, are in {STATE}.") from exc
