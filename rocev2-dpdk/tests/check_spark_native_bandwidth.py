#!/usr/bin/env python3
"""Measure matched native RoCEv2 bandwidth on the Spark-04 to Spark-02 rails."""

import argparse
from contextlib import contextmanager, ExitStack
from dataclasses import dataclass
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "mrc-dpdk" / "tests"))
from check_spark_cycle import Commands, CycleError, require


LOCAL = "spark-04"
PEER = "spark-02"
VERSION = "Version: 6.30"
WRITE_BYTES = 1048576
PMTU = 1024
DURATION = 8
MARGIN = 2
SAMPLE_US = (DURATION - 2 * MARGIN) * 1_000_000
LINK_SPEED = 100000
LINK_MTU = 1500
PROFILES = {"single": (1, 1), "multi": (16, 16)}


@dataclass(frozen=True)
class Port:
    pci: str
    interface: str
    address: str


@dataclass(frozen=True)
class Rail:
    tcp_port: int
    cpu: int
    tx: Port
    rx: Port


RAILS = {
    "first": Rail(28870, 5,
                  Port("0000:01:00.0", "enp1s0f0np0", "10.61.0.2"),
                  Port("0000:01:00.1", "enp1s0f1np1", "10.61.0.1")),
    "second": Rail(28871, 6,
                   Port("0002:01:00.0", "enP2p1s0f0np0", "10.61.0.14"),
                   Port("0002:01:00.1", "enP2p1s0f1np1", "10.61.0.13")),
}
MODES = {"first": ("first",), "second": ("second",), "aggregate": tuple(RAILS)}


def peer_command(commands, *args):
    return [
        "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
        "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2",
        "-o", "ControlMaster=auto", "-o", f"ControlPath={commands.socket_path}",
        "-o", "ControlPersist=10s", PEER, shlex.join(map(str, args)),
    ]


def select_gid(output, local_ip):
    candidates = []
    for line in output.splitlines():
        device, index, gid = line.split()
        if ipaddress.IPv6Address(gid).ipv4_mapped == ipaddress.IPv4Address(local_ip):
            candidates.append((device, int(index)))
    require(len(candidates) == 1, f"{local_ip}: expected exactly one IPv4-mapped RoCEv2 GID")
    return candidates[0]


def read_port(commands, host, port):
    param = commands.run(host, "devlink", "dev", "param", "show", f"pci/{port.pci}", "name", "enable_roce")
    require("cmode driverinit value true" in param, f"{host} {port.pci}: enable native RoCE first")
    link = json.loads(commands.run(host, "ip", "-j", "-4", "address", "show", "dev", port.interface))
    require(len(link) == 1 and link[0].get("operstate") == "UP" and link[0].get("mtu") == LINK_MTU
            and any(item.get("local") == port.address and item.get("prefixlen") == 30
                    for item in link[0].get("addr_info", [])),
            f"{host} {port.interface}: expected a live MTU {LINK_MTU} link with {port.address}/30")
    speed = int(commands.run(host, "cat", f"/sys/class/net/{port.interface}/speed").strip())
    require(speed == LINK_SPEED, f"{host} {port.interface}: expected {LINK_SPEED} Mb/s, got {speed}")

    command = r'''set -eu
for device in /sys/class/infiniband/*; do
    pci=$(realpath "$device/device")
    [ "${pci##*/}" = "$1" ] || continue
    node="$device/ports/1"
    read -r state < "$node/state"
    [ "$state" = "4: ACTIVE" ] || continue
    for type in "$node/gid_attrs/types/"*; do
        index=${type##*/}
        read -r value < "$type" 2>/dev/null || continue
        [ "$value" = "RoCE v2" ] || continue
        read -r interface < "$node/gid_attrs/ndevs/$index" 2>/dev/null || continue
        [ "$interface" = "$2" ] || continue
        read -r gid < "$node/gids/$index"
        printf '%s %s %s\n' "${device##*/}" "$index" "$gid"
    done
done'''
    device, index = select_gid(commands.run(host, "bash", "-c", command, "find-gid", port.pci,
                                            port.interface), port.address)
    return {"pci": port.pci, "interface": port.interface, "data_ip": port.address,
            "device": device, "port": 1, "gid_index": index, "speed_mbps": speed, "mtu": LINK_MTU}


def verify_version(commands, host, binary):
    # Verify the banner because some perftest builds return nonzero for --version.
    output = commands.run(host, "bash", "-c", '"$1" --version 2>&1 || :',
                          "perftest-version", binary)
    require(output.strip() == VERSION, f"{host}: {binary} must report {VERSION}")


@contextmanager
def staged_perftest(commands, local, peer):
    verify_version(commands, LOCAL, local)
    digest = commands.run(LOCAL, "sha256sum", "--", local).split()[0]
    directory = commands.run(PEER, "mktemp", "-d", "/tmp/roce-native-bw.XXXXXXXX").strip()
    require(re.fullmatch(r"/tmp/roce-native-bw\.[a-zA-Z0-9]{8}", directory),
            "unexpected peer benchmark workspace")
    workspace = Path(directory)
    try:
        if not peer:
            peer = workspace / "ib_write_bw"
            copy = peer_command(commands, "bash", "-c", 'umask 077; cat > "$1" && chmod 0700 "$1"',
                                "copy-perftest", peer)
            with local.open("rb") as binary:
                subprocess.run(copy, stdin=binary, check=True, timeout=20)
        verify_version(commands, PEER, peer)
        require(commands.run(PEER, "sha256sum", "--", peer).split()[0] == digest,
                "both hosts must use identical perftest binaries")
        yield peer, workspace, digest
    finally:
        commands.run(PEER, "rm", "-rf", "--", workspace)


@contextmanager
def negotiation_tunnel(commands, directory, names):
    control = str(Path(directory) / "native-tunnel")
    ssh = ["ssh", "-S", control, "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
           "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2"]
    options = []
    for name in names:
        port = RAILS[name].tcp_port
        options.extend(("-L", f"127.0.0.1:{port}:127.0.0.1:{port}"))
    try:
        subprocess.run([*ssh, "-fNT", "-o", "ControlMaster=yes", "-o", "ControlPersist=no",
                        "-o", "ExitOnForwardFailure=yes", *options, PEER],
                       check=True, timeout=15, stdin=subprocess.DEVNULL)
        for name in names:
            port = RAILS[name].tcp_port
            require(listener(commands, LOCAL, port), f"native TCP forward {port} did not open")
        yield
    finally:
        if Path(control).exists():
            subprocess.run([*ssh, "-O", "exit", PEER], check=True, timeout=8, stdin=subprocess.DEVNULL)


def listener(commands, host, port):
    sockets = commands.run(host, "ss", "-H", "-ltn", "sport", "=", f":{port}")
    return [line.split()[3] for line in sockets.splitlines()] == [f"127.0.0.1:{port}"]


def perftest_args(rail, port, profile):
    qps, post_list = PROFILES[profile]
    return [
        "-d", port["device"], "-i", "1", "-x", str(port["gid_index"]),
        "-m", str(PMTU), "-s", str(WRITE_BYTES), "-q", str(qps), "-l", str(post_list),
        "-Q", "16", "-t", "128", "-D", str(DURATION), "-f", str(MARGIN),
        "--perform_warm_up", "--report_gbits", "-N", "-F", "--bind_source_ip", "127.0.0.1",
        "-p", str(rail.tcp_port),
    ]


def parse_rate(output, port, profile):
    qps, post_list = PROFILES[profile]
    expected = {"Device": port["device"], "Number of qps": str(qps), "Connection type": "RC",
                "Mtu": f"{PMTU}[B]", "Link type": "Ethernet", "GID index": str(port["gid_index"])}
    if post_list != 1:
        expected["Post List"] = str(post_list)
    for field, value in expected.items():
        require(re.search(rf"{re.escape(field)}\s*:\s*{re.escape(value)}(?:\s|$)", output),
                f"{port['device']}: perftest result did not match {field} {value}")
    rows = re.findall(rf"^\s*{WRITE_BYTES}\s+([0-9]+)\s+[0-9.]+\s+([0-9.]+)\s+[0-9.]+\s*$",
                      output, re.MULTILINE)
    require(len(rows) == 1, f"{port['device']}: expected one perftest bandwidth result")
    iterations, rate = rows[0]
    require(int(iterations) > 0 and float(rate) > 0, f"{port['device']}: no native bandwidth measured")
    return {"iterations": int(iterations), "payload_gbps": float(rate)}


def parse_windows(trace):
    def microseconds(match):
        return int(match[1]) * 1_000_000 + int(match[2])

    timers = [microseconds(match) for match in re.finditer(r"^(\d+)\.(\d{6}) setitimer\(ITIMER_REAL,",
                                                         trace, re.MULTILINE)]
    alarms = [microseconds(match) for match in re.finditer(r"^(\d+)\.(\d{6}) --- SIGALRM\b",
                                                         trace, re.MULTILINE)]
    require(len(timers) == len(alarms) == 3, "expected three native perftest timer boundaries")
    require(timers[0] < alarms[0] < alarms[1] < alarms[2],
            "native perftest timer boundaries are out of order")
    require(abs(alarms[1] - alarms[0] - SAMPLE_US) <= 250_000, "unexpected native sample duration")
    return {"load_start_us": timers[0], "sample_start_us": alarms[0],
            "sample_end_us": alarms[1], "load_end_us": alarms[2]}


def summarize(rails):
    overlap = min(data["sample_end_us"] for data in rails.values()) - max(
        data["sample_start_us"] for data in rails.values())
    shared_load_start = max(data["load_start_us"] for data in rails.values())
    shared_load_end = min(data["load_end_us"] for data in rails.values())
    require(overlap > 0 and all(shared_load_start <= data["sample_start_us"]
                                <= data["sample_end_us"] <= shared_load_end for data in rails.values()),
            "native perftest sample windows did not overlap while all selected rails were under load")
    return {"sample_overlap_us": overlap,
            "payload_gbps": sum(data["payload_gbps"] for data in rails.values()), "rails": rails}


def verify_rate(rate, windows, rail):
    elapsed_us = windows["sample_end_us"] - windows["sample_start_us"]
    payload_gbps = rate["iterations"] * WRITE_BYTES * 8 / (elapsed_us * 1000)
    require(math.isclose(rate["payload_gbps"], payload_gbps, rel_tol=0.01, abs_tol=0.01),
            f"{rail}: perftest rate disagrees with the completed WRITEs and traced sample length")


def start_server(commands, binary, workspace, run_id, rail, port, profile, log):
    pidfile = workspace / run_id
    wrapper = '''set -eu
export OCS_ROCE_NATIVE_RUN="$1"
printf '%s\\n' "$$" > "$1"
shift
exec timeout --kill-after=2s 35s "$@"'''
    argv = peer_command(commands, "bash", "-c", wrapper, "native-perftest", pidfile,
                        "taskset", "-c", rail.cpu, binary, *perftest_args(rail, port, profile))
    process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
    return process, pidfile


def stop_server(commands, pidfile):
    # The workspace is private to this run. The marker also guards against PID reuse.
    command = r'''if [ -r "$1" ]; then
    read -r pid < "$1"
    if [[ $pid =~ ^[0-9]+$ ]] && [ -r "/proc/$pid/environ" ] &&
       grep -Fxzq -- "OCS_ROCE_NATIVE_RUN=$1" "/proc/$pid/environ"; then
        kill -TERM "$pid" 2>/dev/null || :
    fi
fi'''
    commands.run(PEER, "bash", "-c", command, "stop-native-perftest", pidfile)


def wait_ready(commands, servers):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        require(all(proc.poll() is None for proc, _ in servers.values()), "native receiver exited before readiness")
        if all(listener(commands, PEER, RAILS[name].tcp_port) for name in servers):
            return
        time.sleep(0.15)
    raise CycleError("native perftest receivers did not become ready")


def wait_process(process, timeout, label):
    require(process.wait(timeout=max(0.1, timeout - time.monotonic())) == 0,
            f"{label}: native perftest failed. See raw benchmark logs.")


def stop_client(process):
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=2)
        except ProcessLookupError:
            pass
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=2)


def run_sample(commands, local, peer, workspace, output, mode, profile, sample, ports):
    prefix = f"{mode}-{profile}-{sample}"
    names = MODES[mode]
    servers = {}
    clients = {}
    completed = False
    with ExitStack() as logs:
        try:
            for name in names:
                rail = RAILS[name]
                require(not listener(commands, PEER, rail.tcp_port),
                        f"{PEER}: native TCP port {rail.tcp_port} is already in use")
                log = logs.enter_context((output / f"{prefix}-{name}-rx.txt").open("w"))
                servers[name] = start_server(commands, peer, workspace, f"{prefix}-{name}.pid", rail,
                                             ports[name]["rx"], profile, log)
            wait_ready(commands, servers)
            for name in names:
                rail = RAILS[name]
                log = logs.enter_context((output / f"{prefix}-{name}-tx.txt").open("w"))
                trace = output / f"{prefix}-{name}-tx.strace"
                argv = ["strace", "-ttt", "-e", "trace=setitimer", "-e", "signal=SIGALRM", "-o", trace,
                        "taskset", "-c", str(rail.cpu), local,
                        *perftest_args(rail, ports[name]["tx"], profile), "127.0.0.1"]
                clients[name] = subprocess.Popen(argv, start_new_session=True, stdin=subprocess.DEVNULL,
                                                 stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 25
            for name in names:
                wait_process(clients[name], deadline, f"sender {name}")
                wait_process(servers[name][0], deadline, f"receiver {name}")
            completed = True
        finally:
            for client in clients.values():
                stop_client(client)
            for process, pidfile in servers.values():
                if not completed:
                    try:
                        stop_server(commands, pidfile)
                        process.wait(timeout=4)
                    except (CycleError, subprocess.TimeoutExpired) as error:
                        print(f"{PEER}: native receiver cleanup: {error}", file=sys.stderr)
                        if process.poll() is None:
                            process.terminate()
                            process.wait(timeout=3)

    results = {}
    for name in names:
        tx_log = (output / f"{prefix}-{name}-tx.txt").read_text()
        rx_log = (output / f"{prefix}-{name}-rx.txt").read_text()
        trace = (output / f"{prefix}-{name}-tx.strace").read_text()
        rx = parse_rate(rx_log, ports[name]["rx"], profile)
        tx = parse_rate(tx_log, ports[name]["tx"], profile)
        require(tx == rx, f"{name}: sender and receiver disagree on native bandwidth")
        windows = parse_windows(trace)
        verify_rate(tx, windows, name)
        results[name] = {**tx, **windows}
    return summarize(results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--perftest", type=Path, required=True, help="local pinned perftest 6.30 binary")
    parser.add_argument("--peer-perftest", type=Path, help="exact matching binary already on Spark-02")
    parser.add_argument("--rail", choices=MODES, action="append", help="repeat to select modes")
    parser.add_argument("--profile", choices=PROFILES, action="append", help="repeat to select QP profiles")
    parser.add_argument("--samples", type=int, default=3, help="trials per mode and profile (default 3)")
    parser.add_argument("--output", type=Path, help="new directory for raw output and summary")
    args = parser.parse_args()
    if args.samples < 1:
        parser.error("--samples must be positive")
    if args.peer_perftest and not args.peer_perftest.is_absolute():
        parser.error("--peer-perftest must be an absolute path")
    modes = tuple(dict.fromkeys(args.rail or MODES))
    profiles = tuple(dict.fromkeys(args.profile or PROFILES))
    names = tuple(dict.fromkeys(name for mode in modes for name in MODES[mode]))
    require(socket.gethostname().split(".")[0] == "spark-7e64", "run the benchmark from Spark-04")

    with tempfile.TemporaryDirectory(prefix="roce-native-control-") as sockets:
        commands = Commands(sockets)
        for host in (LOCAL, PEER):
            active = commands.run(host, "systemctl", "list-units", "--state=running,activating,deactivating",
                                  "--no-pager", "--no-legend", "--plain", "ocs-mrc*.service", "ocs-roce*.service")
            require(not active.strip(), f"{host}: a lab service is running")
        ports = {name: {"tx": read_port(commands, LOCAL, RAILS[name].tx),
                        "rx": read_port(commands, PEER, RAILS[name].rx)} for name in names}
        local = args.perftest.resolve(strict=True)
        with (staged_perftest(commands, local, args.peer_perftest) as staged,
              negotiation_tunnel(commands, sockets, names)):
            peer, workspace, digest = staged
            output = args.output or Path(tempfile.mkdtemp(prefix="roce-native-results-"))
            if args.output:
                output.mkdir()
            print(f"Native benchmark output {output}", flush=True)
            report = {"perftest_version": VERSION, "sha256": digest, "ports": ports, "results": {}}
            for mode in modes:
                for profile in profiles:
                    key = f"{mode}-{profile}"
                    trials = [run_sample(commands, local, peer, workspace, output, mode, profile, sample, ports)
                              for sample in range(1, args.samples + 1)]
                    median = statistics.median(trial["payload_gbps"] for trial in trials)
                    report["results"][key] = {"trials": trials, "median_payload_gbps": median}
                    print(f"{key} native median payload {median:.3f} Gbit/s", flush=True)
                    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    try:
        main()
    except (CycleError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
