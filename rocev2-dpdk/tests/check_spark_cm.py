#!/usr/bin/env python3
"""Verify stock RDMA CM and RC WRITE between Spark-04 DPDK and Spark-02 verbs."""

import argparse
from contextlib import ExitStack, contextmanager
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import time

from check_spark_bandwidth import (
    Commands, CycleError, LOOPBACK, PORTS, invocation_messages, require, require_idle,
    save_report, set_link_mtu, start, stop_owned, temporary_mtu, verify_link, verify_loopback_listener,
    wait_endpoint,
)
from check_spark_native_bandwidth import peer_command, select_gid
from stage_spark_cm import INSTALL_ROOT, PROFILES, RUNTIME, render_unit


LOCAL, PEER = PORTS["tx"][0], PORTS["rx"][0]
RUNTIME_DIR = "/run/ocs-roce-cm"
PEER_MAC = "4c:bb:47:82:4e:db"
WRITE_BYTES = 8195
PMTUS = {mtu: pmtu for _, mtu, pmtu, _ in PROFILES}
PORT_NUMBERS = {(role, mtu): port for role, mtu, _, port in PROFILES}
INSTALLED_FILES = ("roce_write", "roce_app_write", "run_app")
NATIVE_BINARY = "roce_verbs_write"


@dataclass(frozen=True)
class Endpoint:
    role: str
    mtu: int
    host: str = LOCAL

    @property
    def port(self):
        return PORT_NUMBERS[self.role, self.mtu]

    @property
    def pmtu(self):
        return PMTUS[self.mtu]

    @property
    def unit(self):
        return f"ocs-roce-cm-{self.role}-{self.mtu}.service"

    @property
    def label(self):
        return f"{self.host} {self.unit}"

    @property
    def native_role(self):
        return "rx" if self.role == "tx" else "tx"


def read_bundle(directory, endpoints):
    digests = {}
    for line in (directory / "SHA256SUMS").read_text().splitlines():
        match = re.fullmatch(r"([0-9a-f]{64}) [ *]([A-Za-z0-9][A-Za-z0-9._-]*)", line)
        require(match and match[2] not in digests, f"{directory}: invalid or repeated SHA256SUMS entry")
        digests[match[2]] = match[1]
    required = (*INSTALLED_FILES, NATIVE_BINARY, *(endpoint.unit for endpoint in endpoints))
    for name in required:
        require(name in digests, f"{directory}: SHA256SUMS is missing {name}")
    artifacts = {}
    for name, digest in digests.items():
        path = directory / name
        require(path.is_file() and not path.is_symlink(), f"{path}: expected a regular bundle file")
        data = path.read_bytes()
        require(hashlib.sha256(data).hexdigest() == digest, f"{path}: SHA-256 mismatch")
        if name in required:
            artifacts[name] = data
    return digests, artifacts


def verify_root_path(commands, path, *, directory=False, executable=False, digest=None):
    require(commands.run(LOCAL, "realpath", "-e", "--", path).strip() == str(path),
            f"{LOCAL}: unexpected installed path for {path}")
    uid, mode, kind = commands.run(LOCAL, "stat", "-c", "%u %a %F", "--", path).split(maxsplit=2)
    mode = int(mode, 8)
    require(uid == "0" and not mode & 0o7022 and
            kind.strip() == ("directory" if directory else "regular file") and
            (not executable or mode & 0o111 == 0o111),
            f"{LOCAL}: {path} must be root-owned with the expected file type and permissions")
    if digest:
        actual = commands.run(LOCAL, "sha256sum", "--", path).split()[0]
        require(actual == digest, f"{LOCAL}: installed SHA-256 for {path} differs from the bundle")


def exec_start(unit):
    lines = [line.removeprefix("ExecStart=") for line in unit.splitlines() if line.startswith("ExecStart=")]
    require(len(lines) == 1, "expected exactly one ExecStart in the CM unit")
    return shlex.split(lines[0])


def verify_unit(commands, endpoint, destination, data, digest):
    staged = exec_start(data.decode())
    canonical = exec_start(render_unit(destination, endpoint.role, endpoint.mtu, endpoint.pmtu, endpoint.port))
    require(staged == canonical, f"{endpoint.label}: staged ExecStart differs from the fixed CM profile")
    props = commands.properties(endpoint, "LoadState", "ActiveState", "WorkingDirectory", "ExecStart",
                                "FragmentPath", "DropInPaths", "Environment", "UnsetEnvironment",
                                "User", "DynamicUser", "RuntimeDirectory")
    require(props.get("LoadState") == "loaded" and props.get("ActiveState") in ("inactive", "failed"),
            f"{endpoint.label}: expected a loaded, inactive unit")
    path = Path("/etc/systemd/system") / endpoint.unit
    require(props.get("FragmentPath") == str(path) and not props.get("DropInPaths"),
            f"{endpoint.label}: unexpected systemd fragment or drop-in")
    verify_root_path(commands, path, digest=digest)
    require(props.get("WorkingDirectory") == str(destination) and props.get("User") == "ocs-roce-cm" and
            props.get("DynamicUser") == "yes" and props.get("RuntimeDirectory") == "ocs-roce-cm",
            f"{endpoint.label}: wrong working directory, service user or runtime directory")
    environment = shlex.split(props.get("Environment", ""))
    require(f"LD_LIBRARY_PATH={RUNTIME}" in environment and f"XDG_RUNTIME_DIR={RUNTIME_DIR}" in environment and
            "RUNTIME_DIRECTORY" in shlex.split(props.get("UnsetEnvironment", "")),
            f"{endpoint.label}: unexpected DPDK runtime environment")
    argv = re.findall(r"argv\[\]=(.*?) ;", props.get("ExecStart", ""))
    require(len(argv) == 1 and shlex.split(argv[0]) == staged,
            f"{endpoint.label}: systemd ExecStart differs from the staged CM unit")
    for action in ("start", "stop"):
        commands.run(LOCAL, "sudo", "-n", "-l", "/usr/bin/systemctl", action, endpoint.unit)
    commands.sudo_journals.add(endpoint)
    commands.journal(endpoint)


def read_native_link(commands, *, allow_raw=False):
    host, pci, interface, data_ip = PORTS["rx"]
    param = commands.run(host, "devlink", "dev", "param", "show", f"pci/{pci}", "name", "enable_roce")
    modes = re.findall(r"\bcmode driverinit value (true|false)\b", param)
    require(len(modes) == 1, f"{host} {pci}: could not read the configured RoCE mode")
    enabled = modes[0] == "true"
    require(enabled or allow_raw, f"{host} {pci}: native RoCE is disabled")
    netdev = Path("/sys/class/net") / interface
    device = Path(commands.run(host, "readlink", "-f", "--", netdev / "device").strip()).name
    require(device == pci, f"{host} {interface}: expected PCI function {pci}, found {device}")
    address = commands.run(host, "ip", "-br", "-4", "address", "show", "dev", interface).split()
    require("UP" in address and f"{data_ip}/30" in address and
            commands.run(host, "cat", netdev / "carrier").strip() == "1",
            f"{host} {interface}: link or address not ready")
    mac = commands.run(host, "cat", netdev / "address").strip()
    require(mac == PEER_MAC, f"{host} {interface}: expected MAC {PEER_MAC}, found {mac}")
    link = {"host": host, "device": pci, "interface": interface, "data_ip": data_ip, "mac": mac,
            "ethernet_mtu": int(commands.run(host, "cat", netdev / "mtu")),
            "link_mbps": int(commands.run(host, "cat", netdev / "speed")), "roce_enabled": enabled}
    if not enabled:
        return link
    find_gid = r'''set -eu
for device in /sys/class/infiniband/*; do
    pci=$(realpath -e -- "$device/device" 2>/dev/null) || continue
    [ "${pci##*/}" = "$1" ] || continue
    node="$device/ports/1"
    read -r state < "$node/state"
    [ "$state" = "4: ACTIVE" ] || continue
    for type in "$node/gid_attrs/types/"*; do
        index=${type##*/}
        read -r value < "$type" 2>/dev/null || continue
        [ "$value" = "RoCE v2" ] || continue
        read -r ndev < "$node/gid_attrs/ndevs/$index" 2>/dev/null || continue
        [ "$ndev" = "$2" ] || continue
        read -r gid < "$node/gids/$index"
        printf '%s %s %s\n' "${device##*/}" "$index" "$gid"
    done
done'''
    device, gid = select_gid(commands.run(host, "bash", "-c", find_gid, "find-gid", pci, interface), data_ip)
    return {**link, "rdma_device": device, "rdma_port": 1, "gid_index": gid}


def peer_mode_commands(enabled):
    device = f"pci/{PORTS['rx'][1]}"
    return (("/usr/sbin/devlink", "dev", "param", "set", device, "name", "enable_roce", "value",
             "true" if enabled else "false", "cmode", "driverinit"),
            ("/usr/sbin/devlink", "dev", "reload", device, "action", "driver_reinit"))


def nopasswd_commands(output):
    commands = set()
    for entry in output.split("Sudoers entry:")[1:]:
        attributes, _, rows = entry.partition("Commands:")
        options = re.search(r"^[ \t]*Options:[ \t]*(.+)$", attributes, re.MULTILINE)
        if not options or "!authenticate" not in re.split(r"[\s,]+", options[1].strip()):
            continue
        for line in rows.splitlines():
            try:
                command = tuple(shlex.split(line.strip()))
            except ValueError:
                continue
            if command and command[0].startswith("/"):
                commands.add(command)
    return commands


def preflight(commands, endpoints, destination, digests, artifacts, *, manage_peer_mode=False):
    for host in (LOCAL, PEER):
        require_idle(commands, host)
    links = {"dpdk": verify_link(commands, PORTS["tx"]),
             "native": read_native_link(commands, allow_raw=manage_peer_mode)}
    for path in (INSTALL_ROOT, destination, RUNTIME):
        verify_root_path(commands, path, directory=True)
    for name in INSTALLED_FILES:
        verify_root_path(commands, destination / name, executable=True, digest=digests[name])
    for endpoint in endpoints:
        verify_unit(commands, endpoint, destination, artifacts[endpoint.unit], digests[endpoint.unit])
    required = {LOCAL: [], PEER: []}
    for link in links.values():
        values = {link["ethernet_mtu"], *(endpoint.mtu for endpoint in endpoints)}
        if len(values) > 1:
            for mtu in sorted(values):
                required[link["host"]].append(("/usr/sbin/ip", "link", "set", "dev",
                                               link["interface"], "mtu", str(mtu)))
    if manage_peer_mode and not links["native"]["roce_enabled"]:
        required[PEER].extend(dict.fromkeys(peer_mode_commands(True) + peer_mode_commands(False)))
    for host, arguments in required.items():
        if arguments:
            allowed = nopasswd_commands(commands.run(host, "env", "LC_ALL=C", "sudo", "-n", "-ll"))
            for argv in arguments:
                require(argv in allowed, f"{host}: missing exact NOPASSWD rule for {shlex.join(argv)}")
    return links


def cleanup_call(action, *args):
    failed = sys.exc_info()[0] is not None
    try:
        action(*args)
    except (CycleError, OSError, subprocess.SubprocessError) as error:
        if not failed:
            raise
        print(error, file=sys.stderr)


def configure_peer_mode(commands, enabled):
    for argv in peer_mode_commands(enabled):
        commands.run(PEER, "sudo", "-n", *argv, timeout=25)


def wait_peer_mode(commands, enabled):
    deadline = time.monotonic() + 20
    while True:
        try:
            link = read_native_link(commands, allow_raw=not enabled)
            require(link["roce_enabled"] == enabled, f"{PEER}: unexpected RoCE mode after reload")
            return link
        except (CycleError, ValueError) as error:
            if time.monotonic() >= deadline:
                raise CycleError(f"{PEER}: address, carrier or RoCE mode did not return: {error}") from error
            time.sleep(0.25)


def restore_peer_mode(commands, original):
    configure_peer_mode(commands, False)
    restored = wait_peer_mode(commands, False)
    if restored["ethernet_mtu"] != original["ethernet_mtu"]:
        set_link_mtu(commands, restored, original["ethernet_mtu"])
        require(wait_peer_mode(commands, False)["ethernet_mtu"] == original["ethernet_mtu"],
                f"{PEER}: MTU did not return to {original['ethernet_mtu']} after restoring raw mode")
    print(f"{PEER} {restored['interface']} restored raw mode with {restored['data_ip']} and carrier", flush=True)


@contextmanager
def native_peer_mode(commands, original):
    if original["roce_enabled"]:
        yield original
        return
    for host in (LOCAL, PEER):
        require_idle(commands, host)
    enable, reload = peer_mode_commands(True)
    commands.run(PEER, "sudo", "-n", *enable, timeout=25)
    try:
        commands.run(PEER, "sudo", "-n", *reload, timeout=25)
        yield wait_peer_mode(commands, True)
    finally:
        cleanup_call(restore_peer_mode, commands, original)


@contextmanager
def stage_native(commands, data, digest):
    output = commands.run(PEER, "mktemp", "-d", "/tmp/ocs-roce-cm-peer.XXXXXXXX").strip()
    require(re.fullmatch(r"/tmp/ocs-roce-cm-peer\.[A-Za-z0-9]{8}", output),
            f"{PEER}: invalid staging directory")
    workspace = Path(output)
    binary = workspace / NATIVE_BINARY
    try:
        owner, mode = commands.run(PEER, "stat", "-c", "%u %a", "--", workspace).split()
        require(owner == commands.run(PEER, "id", "-u").strip() and mode == "700",
                f"{PEER}: expected a private staging directory owned by the SSH account")
        command = peer_command(commands, "bash", "-c", 'set -eu; umask 077; cat > "$1"; chmod 0700 "$1"',
                               "stage-roce-peer", binary)
        copied = subprocess.run(command, input=data, capture_output=True, timeout=20)
        require(copied.returncode == 0, f"{PEER}: could not stage {binary}: {copied.stderr.decode(errors='replace')}")
        require(commands.run(PEER, "sha256sum", "--", binary).split()[0] == digest,
                f"{PEER}: staged native peer SHA-256 mismatch")
        yield workspace, binary
    finally:
        cleanup_call(commands.run, PEER, "rm", "-f", "--", binary)
        cleanup_call(commands.run, PEER, "rmdir", "--", workspace)


@contextmanager
def verification_tunnel(commands, directory, endpoint):
    control = Path(directory) / f"{endpoint.role}-{endpoint.mtu}.ssh"
    ssh = ["ssh", "-S", str(control), "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
           "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2"]
    forward = "-L" if endpoint.role == "tx" else "-R"
    address = f"{LOOPBACK}:{endpoint.port}:{LOOPBACK}:{endpoint.port}"
    try:
        result = subprocess.run([*ssh, "-fNT", "-o", "ControlMaster=yes", "-o", "ControlPersist=no",
                                 "-o", "ExitOnForwardFailure=yes", forward, address, PEER],
                                capture_output=True, text=True, timeout=15, stdin=subprocess.DEVNULL)
        require(result.returncode == 0, f"{endpoint.label}: verification tunnel failed: {result.stderr.strip()}")
        verify_loopback_listener(commands, LOCAL if endpoint.role == "tx" else PEER, endpoint.port)
        yield
    finally:
        if control.exists():
            failed = sys.exc_info()[0] is not None
            try:
                result = subprocess.run([*ssh, "-O", "exit", PEER], capture_output=True, text=True,
                                        timeout=5, stdin=subprocess.DEVNULL)
                require(result.returncode == 0 or not control.exists(),
                        f"{endpoint.label}: verification tunnel cleanup failed: {result.stderr.strip()}")
            except (CycleError, OSError, subprocess.SubprocessError) as error:
                if not failed:
                    raise
                print(error, file=sys.stderr)


@contextmanager
def native_process(commands, workspace, binary, endpoint, link, path):
    pidfile = workspace / f"{endpoint.role}-{endpoint.mtu}.pid"
    launch = r'''set -eu
export OCS_ROCE_CM_RUN="$1"
printf '%s\n' "$$" > "$1"
shift
exec timeout --kill-after=2s 55s env -u LD_PRELOAD -u LD_LIBRARY_PATH -u RDMAV_DRIVERS "$@"'''
    arguments = (binary, endpoint.native_role, WRITE_BYTES, LOOPBACK, endpoint.port, link["data_ip"],
                 endpoint.pmtu, link["rdma_device"], link["rdma_port"], link["gid_index"], "--cm-peer", PORTS["tx"][3])
    argv = peer_command(commands, "bash", "-c", launch, "run-roce-peer", pidfile, *arguments)
    with path.open("w") as log:
        process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        try:
            yield process
        finally:
            stop = r'''if [ -r "$1" ]; then
    read -r pid < "$1"
    if [[ $pid =~ ^[0-9]+$ ]] && [ -r "/proc/$pid/environ" ] &&
       grep -Fxzq -- "OCS_ROCE_CM_RUN=$1" "/proc/$pid/environ"; then
        kill -TERM "$pid" 2>/dev/null || :
    fi
    rm -f -- "$1"
fi'''
            try:
                cleanup_call(commands.run, PEER, "bash", "-c", stop, "stop-roce-peer", pidfile)
            finally:
                try:
                    process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=4)


def ready_native(commands, endpoint, process, path, deadline):
    while time.monotonic() < deadline:
        status = process.poll()
        require(status is None, f"{endpoint.label}: native receiver exited with status {status} before readiness")
        if "control listener ready" in path.read_text(errors="replace").splitlines():
            verify_loopback_listener(commands, PEER, endpoint.port)
            return
        time.sleep(0.1)
    raise CycleError(f"{endpoint.label}: native receiver readiness timed out")


def validate_result(endpoint, messages, native):
    packets = (WRITE_BYTES + endpoint.pmtu - 1) // endpoint.pmtu
    connections = [line for line in messages if re.fullmatch(
        r"RoCEv2 CM established local_qpn=[0-9a-f]{6} peer_qpn=[0-9a-f]{6}", line)]
    require(len(connections) == 1 and messages.count("RoCEv2 CM disconnected") == 1,
            f"{endpoint.label}: missing DPDK CM establishment or disconnect")
    require(native.count("Native RoCEv2 CM established") == 1 and
            native.count("Native RoCEv2 CM disconnected") == 1,
            f"{endpoint.label}: missing native CM establishment or disconnect")
    result = {"payload_bytes": WRITE_BYTES, "packets": packets, "pmtu": endpoint.pmtu}
    bandwidth = [line for line in messages if line.startswith("BANDWIDTH ")]
    require(len(bandwidth) == 1, f"{endpoint.label}: missing bandwidth report")
    counters = dict(field.split("=", 1) for field in bandwidth[0].split()[1:])
    expected = {"role": endpoint.role, "payload_bytes": str(WRITE_BYTES), "writes": "1", "qps": "1", "workers": "1"}
    require(all(counters.get(key) == value for key, value in expected.items()),
            f"{endpoint.label}: unexpected bandwidth report {bandwidth[0]}")
    if endpoint.role == "tx":
        pattern = rf"WRITE complete: {WRITE_BYTES} bytes, {packets} packets, ([0-9]+) retries, peer verified"
        writes = [match for line in messages if (match := re.fullmatch(pattern, line))]
        maps = [match for line in messages if (match := re.fullmatch(
            r"Application DMA region mapped for scatter/gather TX \(([0-9]+) bytes\)", line))]
        require(len(writes) == 1 and len(maps) == 1 and int(maps[0][1]) >= WRITE_BYTES,
                f"{endpoint.label}: missing DPDK WRITE completion, retry count or application DMA mapping")
        require(native.count(f"WRITE verified: {WRITE_BYTES} bytes in native registered memory") == 1,
                f"{endpoint.label}: missing native byte verification")
        result.update(retries=int(writes[0][1]), dma_bytes=int(maps[0][1]))
    else:
        require(messages.count(f"WRITE verified: {WRITE_BYTES} bytes, {packets} packets") == 1,
                f"{endpoint.label}: missing DPDK byte verification")
        require(native.count(f"WRITE complete: {WRITE_BYTES} bytes, native verbs completion, peer verified") == 1,
                f"{endpoint.label}: missing native WRITE completion")
    return result


def resolve_native_neighbor(commands, link):
    _, _, dpdk_interface, dpdk_address = PORTS["tx"]
    dpdk_mac = commands.run(LOCAL, "cat", f"/sys/class/net/{dpdk_interface}/address").strip()
    interface = link["interface"]
    try:
        # Resolve the neighbor while Linux still receives ARP on the DPDK port.
        commands.run(PEER, "ping", "-n", "-I", interface, "-c", "1", "-W", "2", dpdk_address, timeout=5)
    except CycleError:
        # An ICMP reply is optional once ARP has obtained the expected MAC.
        pass
    neighbors = json.loads(commands.run(PEER, "ip", "-j", "neigh", "show", "to", dpdk_address, "dev", interface))
    valid_states = {"REACHABLE", "STALE", "DELAY", "PROBE", "PERMANENT"}
    require(any(entry.get("lladdr") == dpdk_mac and valid_states.intersection(entry.get("state", ()))
                for entry in neighbors), f"{PEER}: no usable ARP neighbor for {dpdk_address} on {interface}")


def run_trial(commands, sockets, endpoint, workspace, binary, link, output):
    stem = f"{endpoint.role}-{endpoint.mtu}"
    native_log = output / f"{stem}-native.txt"
    dpdk_log = output / f"{stem}-dpdk.txt"
    for host in (LOCAL, PEER):
        require_idle(commands, host)
    resolve_native_neighbor(commands, link)
    invocation = None
    with verification_tunnel(commands, sockets, endpoint), ExitStack() as resources:
        try:
            if endpoint.role == "rx":
                invocation = start(commands, endpoint)
                wait_endpoint(commands, endpoint, invocation, time.monotonic() + 20, ready=True)
                verify_loopback_listener(commands, LOCAL, endpoint.port)
            native = resources.enter_context(native_process(commands, workspace, binary, endpoint, link, native_log))
            if endpoint.role == "tx":
                ready_native(commands, endpoint, native, native_log, time.monotonic() + 20)
                invocation = start(commands, endpoint)
            deadline = time.monotonic() + 45
            messages = wait_endpoint(commands, endpoint, invocation, deadline)
            dpdk_log.write_text("\n".join(messages) + "\n")
            status = native.wait(timeout=max(0, deadline - time.monotonic()))
            require(status == 0, f"{endpoint.label}: native peer exited with status {status}")
            return {"invocation": invocation, **validate_result(
                endpoint, messages, native_log.read_text(errors="replace").splitlines())}
        finally:
            if invocation:
                try:
                    if not dpdk_log.exists():
                        messages = invocation_messages(commands, endpoint, invocation)
                        dpdk_log.write_text("\n".join(messages) + "\n")
                except (OSError, CycleError, ValueError) as error:
                    print(error, file=sys.stderr)
                cleanup_call(stop_owned, commands, endpoint, invocation)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True, help="local staging bundle with SHA256SUMS")
    parser.add_argument("--destination", type=Path, required=True, help="installed snapshot directory")
    parser.add_argument("--mtu", type=int, choices=PMTUS, action="append", help="repeat to select Ethernet MTUs")
    parser.add_argument("--role", choices=("tx", "rx"), action="append", help="repeat to select DPDK roles")
    parser.add_argument("--output", type=Path, help="new directory for logs and results")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--preflight", action="store_true", help="check units, permissions and links without running")
    mode.add_argument("--manage-peer-mode", action="store_true",
                      help="enable native Spark-02 RoCE for the run and restore its original mode afterward")
    args = parser.parse_args()
    if args.destination.parent != INSTALL_ROOT or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*",
                                                                       args.destination.name):
        parser.error(f"--destination must name a snapshot directly under {INSTALL_ROOT}")
    require(socket.gethostname().split(".")[0] == "spark-7e64", "run the CM test from Spark-04")
    mtus = tuple(dict.fromkeys(args.mtu or PMTUS))
    roles = tuple(dict.fromkeys(args.role or ("tx", "rx")))
    endpoints = tuple(Endpoint(role, mtu) for mtu in mtus for role in roles)
    output = args.output or Path(tempfile.mkdtemp(prefix="roce-cm-results-"))
    if args.output:
        output.mkdir()
    print(f"CM test output {output}", flush=True)
    report = {"destination": str(args.destination), "bundle": str(args.bundle.resolve()), "trials": {}}
    try:
        digests, artifacts = read_bundle(args.bundle, endpoints)
        report["sha256"] = digests
        with tempfile.TemporaryDirectory(prefix="roce-cm-control-") as sockets:
            commands = Commands(sockets)
            links = preflight(commands, endpoints, args.destination, digests, artifacts,
                              manage_peer_mode=args.manage_peer_mode)
            report["links"] = links
            save_report(output, report)
            if args.preflight:
                return
            with (
                stage_native(commands, artifacts[NATIVE_BINARY], digests[NATIVE_BINARY]) as (workspace, binary),
                native_peer_mode(commands, links["native"]) as native_link,
            ):
                live_links = {**links, "native": native_link}
                for mtu in mtus:
                    with temporary_mtu(commands, live_links, mtu):
                        native_link = read_native_link(commands)
                        for endpoint in (item for item in endpoints if item.mtu == mtu):
                            result = run_trial(commands, sockets, endpoint, workspace, binary, native_link, output)
                            report["trials"][endpoint.unit] = result
                            save_report(output, report)
                            print(f"{endpoint.label} verified {WRITE_BYTES} bytes, {result['packets']} packets", flush=True)
    except (CycleError, OSError, ValueError, KeyError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        report["failure"] = str(error) or "interrupted"
        save_report(output, report)
        raise


def terminate(signal_number, _frame):
    raise KeyboardInterrupt(signal.Signals(signal_number).name)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, terminate)
    try:
        main()
    except (CycleError, OSError, ValueError, KeyError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        print(str(error) or "interrupted", file=sys.stderr)
        sys.exit(1)
