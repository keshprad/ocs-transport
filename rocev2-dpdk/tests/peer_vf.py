#!/usr/bin/python3 -I
"""Configure the two Spark ring receivers on virtual functions of their peer's TX port."""

import argparse
import fcntl
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path("/etc/systemd/system")
HELPER = Path("/usr/local/libexec/ocs-roce-peer-vf")
PCI = Path("/sys/bus/pci/devices")
HOSTS = {"spark-28a3": 1, "spark-4ed9": 2, "spark-7e64": 4}
# Incoming forward data comes from the peer reached by the reverse sender.
PARENTS = {"forward-rx": "0002:01:00.1", "reverse-rx": "0000:01:00.0"}
OLD = {"forward-rx": "0000:01:00.1", "reverse-rx": "0002:01:00.0"}
UNIT = "ocs-roce-ring-gather-"
PLACEHOLDER = "${ROCE_PEER_VF}"
BDF = re.compile(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]")


def run(*argv):
    return subprocess.check_output(argv, text=True).strip()


def state_file(role):
    return Path(f"/run/ocs-roce-peer-{role}.env")


def netdev(device):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        names = list((device / "net").glob("*"))
        if len(names) == 1:
            return names[0].name
        time.sleep(0.1)
    raise RuntimeError(f"Expected one network interface on {device.name}")


def invocation():
    value = os.environ.get("INVOCATION_ID", "")
    if not re.fullmatch(r"[0-9a-f]{32}", value):
        raise RuntimeError("Expected a systemd service invocation")
    return value


def release(role):
    state = state_file(role)
    if not state.exists():
        return
    owner, *fields = state.read_text().splitlines()
    if owner != "# " + invocation():
        raise RuntimeError(f"{state} belongs to a different invocation")
    parent = PCI / PARENTS[role]
    count = int((parent / "sriov_numvfs").read_text())
    if count == 1:
        vf = (parent / "virtfn0").resolve(strict=True)
        if fields and fields != ["ROCE_PEER_VF=" + vf.name]:
            raise RuntimeError(f"{parent.name}: VF identity changed")
        (parent / "sriov_numvfs").write_text("0\n")
    elif count:
        raise RuntimeError(f"{parent.name}: VF count changed to {count}")
    state.unlink()


def prepare(role):
    parent = PCI / PARENTS[role]
    count = parent / "sriov_numvfs"
    pf_interface = netdev(parent)
    if count.read_text().strip() != "0":
        raise RuntimeError(f"{parent.name} already has VFs in use")
    if (parent / "net" / pf_interface / "mtu").read_text().strip() != "4200":
        raise RuntimeError(f"{parent.name}: the jumbo ring pool must be active")
    state = state_file(role)
    with state.open("x") as marker:
        marker.write("# " + invocation() + "\n")
    try:
        count.write_text("1\n")
        vf = (parent / "virtfn0").resolve(strict=True)
        if (vf / "physfn").resolve(strict=True) != parent.resolve() or not BDF.fullmatch(vf.name):
            raise RuntimeError(f"{vf}: incorrect parent")
        with state.open("a") as marker:
            marker.write("ROCE_PEER_VF=" + vf.name + "\n")
        host = HOSTS[socket.gethostname().split(".")[0]]
        mac = f"02:4f:43:53:{host:02x}:{1 if role == 'forward-rx' else 2:02x}"
        run("/usr/sbin/ip", "link", "set", "dev", pf_interface, "vf", "0", "mac", mac)
        netdev(vf)  # Wait for mlx5_core to bind before using devlink.
        devlink = "/usr/sbin/devlink"
        target = "pci/" + vf.name
        param = ("dev", "param", "show", target, "name", "enable_roce")
        if "cmode driverinit value false" not in run(devlink, *param):
            run(devlink, "dev", "param", "set", target, "name", "enable_roce",
                "value", "false", "cmode", "driverinit")
            run(devlink, "dev", "reload", target)
            if "cmode driverinit value false" not in run(devlink, *param):
                raise RuntimeError(f"{target}: native RoCE still enabled")
        iface = netdev(vf)
        run("/usr/sbin/ip", "link", "set", "dev", iface, "address", mac, "mtu", "4200", "up")
        if (vf / "net" / iface / "address").read_text().strip() != mac:
            raise RuntimeError(f"{iface}: unexpected VF MAC")
        print(f"PEER_PCIE parent={parent.name} vf={vf.name} interface={iface}", flush=True)
    except BaseException:
        release(role)
        raise


def dropin(role, unit_text):
    starts = [line for line in unit_text.splitlines() if line.startswith("ExecStart=")]
    old = OLD[role]
    if len(starts) != 1 or starts[0].count(old) != 2 or f"-a {old}," not in starts[0] or f"4096 {old} " not in starts[0]:
        raise RuntimeError(f"{role}: unexpected base unit PCI arguments")
    return (f"[Service]\nEnvironmentFile=-{state_file(role)}\n"
            f"ExecStartPre=+{HELPER} prepare {role}\nExecStart=\n"
            + starts[0].replace(old, PLACEHOLDER)
            + f"\nExecStopPost=+{HELPER} release {role}\n")


def replace_file(path, data, mode):
    path.parent.mkdir(exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as out:
        temporary = Path(out.name)
        try:
            out.write(data)
            out.flush()
            os.fchmod(out.fileno(), mode)
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)


def configure(action):
    files = {HELPER: (Path(__file__).read_bytes(), 0o755)}
    busy = run("systemctl", "list-units", "--state=active,activating,deactivating",
               "--no-legend", "--plain", "ocs-roce*.service", "ocs-mrc*.service")
    if busy:
        raise RuntimeError("A benchmark service is active: " + busy)
    for role, parent in PARENTS.items():
        unit = UNIT + role + ".service"
        target = ROOT / (unit + ".d") / "60-peer-pcie.conf"
        definition = (ROOT / unit).read_text()
        if run("systemctl", "show", "-p", "FragmentPath", "--value", unit) != str(ROOT / unit):
            raise RuntimeError(f"{unit}: unexpected unit location")
        drops = run("systemctl", "show", "-p", "DropInPaths", "--value", unit).split()
        if set(drops) - {str(target)}:
            raise RuntimeError(f"{unit}: another override is installed")
        device = PCI / parent
        if int((device / "sriov_totalvfs").read_text()) < 1 or int((device / "sriov_numvfs").read_text()):
            raise RuntimeError(f"{parent}: expected available SR-IOV capacity and no existing VFs")
        files[target] = (dropin(role, definition).encode(), 0o644)
        print(f"{role} -> VF 0 of {parent} ({netdev(device)})")
    if action == "check":
        return
    changes = []
    try:
        for path, (data, mode) in files.items():
            old = path.read_bytes() if path.exists() else None
            if old is not None and old != data:
                raise RuntimeError(f"Existing file differs: {path}")
            if action == "install" and old is None:
                replace_file(path, data, mode)
                changes.append((path, old, mode))
            elif action == "uninstall" and old is not None:
                path.unlink()
                changes.append((path, old, mode))
        run("systemctl", "daemon-reload")
        for role in PARENTS:
            unit = UNIT + role + ".service"
            effective = run("systemctl", "show", "-p", "ExecStart", "--value", unit)
            if (PLACEHOLDER in effective) != (action == "install"):
                raise RuntimeError(f"{unit}: unexpected effective PCI argument")
    except BaseException:
        for path, old, mode in reversed(changes):
            if old is None:
                path.unlink(missing_ok=True)
            else:
                replace_file(path, old, mode)
        run("systemctl", "daemon-reload")
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("check", "install", "uninstall", "prepare", "release"))
    parser.add_argument("role", choices=PARENTS, nargs="?")
    args = parser.parse_args()
    if socket.gethostname().split(".")[0] not in HOSTS:
        parser.error("Run on Spark-01, Spark-02 or Spark-04")
    if bool(args.role) != (args.action in ("prepare", "release")):
        parser.error("Only prepare and release take a receiver role")
    if args.action == "check":
        configure(args.action)
        return
    if os.geteuid() != 0:
        parser.error("This operation requires sudo")
    with open("/run/ocs-roce-peer-vf.lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if args.action == "prepare":
            prepare(args.role)
        elif args.action == "release":
            release(args.role)
        else:
            configure(args.action)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(str(exc))
