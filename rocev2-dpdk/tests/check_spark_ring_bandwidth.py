#!/usr/bin/env python3
"""Measure concurrent, verified RoCEv2 WRITEs on the three-Spark ring."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, contextmanager
from dataclasses import dataclass
import os
from pathlib import Path
import re
import select
import shlex
import socket
import statistics
import subprocess
import sys
import tempfile
import time

from check_spark_bandwidth import (
    Commands, CycleError, DESTINATION_ROOT, GATE_LEAD_NS, GATE_PORT, GATE_TIMEOUT,
    LOOPBACK, PAYLOAD, PMTU, Profile, SEND_WINDOW, TX_BURST, WRITE_BYTES,
    invocation_messages, parse_result, require, save_report, start, stop_owned,
    verify_loopback_listener, wait_endpoint,
)


HOSTS = ("spark-04", "spark-02", "spark-01")


@dataclass(frozen=True)
class Endpoint:
    host: str
    role: str
    port: int
    data_ip: str

    @property
    def name(self):
        return self.role

    @property
    def settings(self):
        return Profile(self.port, 16, 4, aggregate=True)

    @property
    def pci(self):
        return "0000:01:00.0" if self.role == "tx" else "0000:01:00.1"

    @property
    def interface(self):
        return "enp1s0f0np0" if self.role == "tx" else "enp1s0f1np1"

    @property
    def cpus(self):
        return (5, 6, 7, 8) if self.role == "tx" else (15, 16, 17, 18)

    @property
    def runtime_dir(self):
        if (self.host, self.role) in (("spark-04", "tx"), ("spark-02", "rx")):
            return Path("/run/ocs-roce-bw")
        return Path(f"/run/ocs-roce-ring-{self.role}")

    @property
    def unit(self):
        return f"ocs-roce-ring-{self.role}.service"

    @property
    def label(self):
        return f"{self.host} {self.unit}"


EDGES = (
    (Endpoint("spark-04", "tx", 28873, "10.61.0.2"), Endpoint("spark-02", "rx", 28873, "10.61.0.1")),
    (Endpoint("spark-02", "tx", 28874, "10.61.0.10"), Endpoint("spark-01", "rx", 28874, "10.61.0.9")),
    (Endpoint("spark-01", "tx", 28875, "10.61.0.6"), Endpoint("spark-04", "rx", 28875, "10.61.0.5")),
)
SENDERS, RECEIVERS = tuple(zip(*EDGES))
ENDPOINTS = tuple(endpoint for edge in EDGES for endpoint in edge)


def unit_argv(endpoint, destination, runtime):
    profile = endpoint.settings
    argv = [
        "/usr/bin/flock", "--exclusive", "--nonblock", str(endpoint.runtime_dir / "function.lock"),
        str(destination / "roce_write"), "-l", ",".join(map(str, endpoint.cpus)), "--iova-mode=va",
        "--no-huge", "--no-telemetry", f"--file-prefix=ocs-roce-ring-{endpoint.role}",
        "-d", str(runtime / "librte_mempool_ring.so.24"),
        "-d", str(runtime / "librte_net_mlx5.so.24"), "-a", endpoint.pci,
        "--", endpoint.role, str(WRITE_BYTES), LOOPBACK, str(endpoint.port), endpoint.data_ip,
        str(PMTU), endpoint.pci, "--report-bw", "--tx-burst", str(TX_BURST),
        "--send-window", str(SEND_WINDOW), "--writes", str(profile.writes),
        "--qps", str(profile.qps), "--workers", str(profile.workers),
    ]
    if endpoint.role == "tx":
        argv += ["--start-gate", str(GATE_PORT)]
    return argv


def verify_unit(commands, endpoint, destination, *, build_argv=None):
    build_argv = build_argv or unit_argv
    props = commands.properties(
        endpoint, "LoadState", "ActiveState", "WorkingDirectory", "ExecStart",
        "Environment", "UnsetEnvironment",
    )
    require(props.get("LoadState") == "loaded", f"{endpoint.label}: unit is missing")
    require(props.get("ActiveState") in ("inactive", "failed"), f"{endpoint.label}: already running")
    require(props.get("WorkingDirectory") == str(destination), f"{endpoint.label}: wrong snapshot")
    environment = shlex.split(props.get("Environment", ""))
    require(f"XDG_RUNTIME_DIR={endpoint.runtime_dir}" in environment and
            "RUNTIME_DIRECTORY" in shlex.split(props.get("UnsetEnvironment", "")),
            f"{endpoint.label}: unexpected DPDK runtime directory")
    paths = [Path(word.split("=", 1)[1]) for word in environment if word.startswith("LD_LIBRARY_PATH=")]
    require(len(paths) == 1 and paths[0].is_absolute() and ":" not in str(paths[0]),
            f"{endpoint.label}: expected one absolute DPDK runtime library path")
    installed = re.findall(r"argv\[\]=(.*?) ;", props.get("ExecStart", ""))
    require(len(installed) == 1 and shlex.split(installed[0]) == build_argv(endpoint, destination, paths[0]),
            f"{endpoint.label}: installed ExecStart differs from the expected ring profile")
    for operation in ("start", "stop"):
        commands.run(endpoint.host, "sudo", "-n", "-l", "/usr/bin/systemctl", operation, endpoint.unit)
    commands.sudo_journals.add(endpoint)
    commands.journal(endpoint)


def verify_link(commands, endpoint):
    host, pci = endpoint.host, endpoint.pci
    sysfs = Path("/sys/class/net") / endpoint.interface
    actual = Path(commands.run(host, "readlink", "-f", "--", sysfs / "device").strip()).name
    require(actual == pci, f"{endpoint.label}: {endpoint.interface} is on {actual}, expected {pci}")
    mode = commands.run(host, "devlink", "dev", "param", "show", f"pci/{pci}", "name", "enable_roce")
    require("cmode driverinit value false" in mode, f"{endpoint.label}: native RoCE still enabled on {pci}")
    carrier, mtu, speed = commands.run(host, "cat", sysfs / "carrier", sysfs / "mtu", sysfs / "speed").split()
    require(carrier == "1" and int(speed) == 100_000, f"{endpoint.label}: expected an active 100G link")
    require(int(mtu) >= PMTU + 60, f"{endpoint.label}: Ethernet MTU {mtu} cannot carry PMTU {PMTU}")
    return {
        "host": host, "device": pci, "interface": endpoint.interface, "data_ip": endpoint.data_ip,
        "ethernet_mtu": int(mtu), "link_mbps": int(speed), "cpu_ids": list(endpoint.cpus),
    }


def preflight(commands, destination, digest):
    executable = destination / "roce_write"
    for host in HOSTS:
        active = commands.run(host, "systemctl", "list-units", "--state=running,activating,deactivating",
                              "--no-pager", "--no-legend", "--plain", "ocs-mrc*.service", "ocs-roce*.service")
        require(not active.strip(), f"{host}: another lab service is running")
        for path in (destination, executable):
            require(commands.run(host, "realpath", "-e", "--", path).strip() == str(path),
                    f"{host}: unexpected installed path for {path}")
            owner, mode = commands.run(host, "stat", "-c", "%u %a", "--", path).split()
            require(owner == "0" and not (int(mode, 8) & 0o022),
                    f"{host}: {path} must be writable only by root")
        actual = commands.run(host, "sha256sum", "--", executable).split()[0]
        require(actual == digest, f"{host}: installed binary does not match the expected SHA-256")
    links = {}
    for tx, rx in EDGES:
        name = edge_name(tx, rx)
        links[name] = {}
        for endpoint in (tx, rx):
            verify_unit(commands, endpoint, destination)
            links[name][endpoint.role] = verify_link(commands, endpoint)
    return links


def edge_name(tx, rx):
    return f"{tx.host}-to-{rx.host}"


def forward(port):
    return f"{LOOPBACK}:{port}:{LOOPBACK}:{port}"


@contextmanager
def control_tunnels(commands, socket_dir, edges=None):
    # The 02→01 loopback connection traverses both management sessions via 04.
    edges = EDGES if edges is None else edges
    controller = HOSTS[0]
    sessions = {host: [] for host in HOSTS[1:]}
    for tx, rx in edges:
        if tx.host != controller:
            sessions[tx.host].extend(("-R", forward(tx.port)))
        if rx.host != controller:
            sessions[rx.host].extend(("-L", forward(rx.port)))
    opened = []
    try:
        for host, forwards in sessions.items():
            control = Path(socket_dir) / f"{host}.ring"
            ssh = ["ssh", "-S", str(control), "-o", "BatchMode=yes"]
            opened.append((host, control, ssh))
            result = subprocess.run(
                [*ssh, "-fNT", "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=5",
                 "-o", "ServerAliveCountMax=2", "-o", "ControlMaster=yes", "-o", "ControlPersist=no",
                 "-o", "ExitOnForwardFailure=yes", *forwards, host],
                stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=15,
            )
            require(result.returncode == 0, f"{host}: could not open ring tunnel: {result.stderr.strip()}")
        for tx, rx in edges:
            if controller not in (tx.host, rx.host):
                verify_loopback_listener(commands, controller, tx.port)
            verify_loopback_listener(commands, tx.host, tx.port)
        yield
    except subprocess.TimeoutExpired as error:
        raise CycleError(f"ring control tunnel timed out: {error}") from error
    finally:
        failed = sys.exc_info()[0] is not None
        errors = []
        for host, control, ssh in reversed(opened):
            if not control.exists():
                continue
            try:
                result = subprocess.run([*ssh, "-O", "exit", host], stdin=subprocess.DEVNULL,
                                        capture_output=True, text=True, timeout=5)
                require(result.returncode == 0 or not control.exists(),
                        f"{host}: ring tunnel cleanup failed: {result.stderr.strip()}")
            except (CycleError, OSError, subprocess.TimeoutExpired) as error:
                errors.append(str(error))
        if errors:
            if not failed:
                raise CycleError("\n".join(errors))
            print("\n".join(errors), file=sys.stderr)


# This program runs unprivileged in the controller's local or SSH session.
# EOF from the controller closes the listener and any accepted sender socket.
GATE_PROGRAM = r"""
import os
import select
import socket
import struct
import sys
import time
from contextlib import ExitStack

loopback, gate, ports, timeout, lead = sys.argv[1:]
pending = {int(port) for port in ports.split(",")}
deadline = time.monotonic() + int(timeout)
with ExitStack() as stack:
    listener = stack.enter_context(socket.create_server((loopback, int(gate))))
    connections = []
    print("LISTENING", flush=True)
    while pending:
        active, _, _ = select.select([listener, 0], [], [], max(0, deadline - time.monotonic()))
        if listener not in active or 0 in active:
            raise RuntimeError("Ring sender gate expired or was canceled")
        conn, _ = listener.accept()
        stack.enter_context(conn)
        conn.settimeout(max(0.001, deadline - time.monotonic()))
        header = conn.recv(2, socket.MSG_WAITALL)
        if len(header) != 2:
            raise RuntimeError("Incomplete sender identity")
        port = struct.unpack("!H", header)[0]
        if port not in pending:
            raise RuntimeError(f"Unexpected sender identity {port}")
        pending.remove(port)
        connections.append(conn)
    print("READY", flush=True)
    readable, _, _ = select.select([0, *connections], [], [], max(0, deadline - time.monotonic()))
    if readable != [0] or os.read(0, 1) != b"G":
        raise RuntimeError("Ring sender left the gate or the run was canceled")
    release = time.monotonic_ns() + int(lead)
    for conn in connections:
        conn.settimeout(0.25)
        conn.sendall(struct.pack("!Q", release))
    print(f"RELEASED {release}", flush=True)
"""


@contextmanager
def coordinator(commands, endpoint, log, *, ports=None, timeout=None):
    argv = [
        sys.executable if endpoint.host == HOSTS[0] else "python3", "-u", "-c", GATE_PROGRAM,
        LOOPBACK, str(GATE_PORT), ports or str(endpoint.port), str(timeout or GATE_TIMEOUT), str(GATE_LEAD_NS),
    ]
    if endpoint.host != HOSTS[0]:
        argv = [
            "ssh", "-T", "-S", commands.socket_path, "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
            "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2", endpoint.host, shlex.join(argv),
        ]
    with log.open("wb") as stderr:
        child = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr, bufsize=0)
        try:
            yield child
        finally:
            failed = sys.exc_info()[0] is not None
            child.stdin.close()
            try:
                status = child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                child.terminate()
                try:
                    status = child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    child.kill()
                    status = child.wait()
            child.stdout.close()
            if not failed:
                require(status == 0, f"{endpoint.host}: ring gate failed ({status}), see {log}")


def read_gate_event(child, host, marker, deadline):
    line = b""
    while b"\n" not in line:
        remaining = deadline - time.monotonic()
        require(remaining > 0 and select.select([child.stdout], [], [], remaining)[0],
                f"{host}: waiting for ring gate {marker} timed out")
        part = os.read(child.stdout.fileno(), 128)
        require(part, f"{host}: ring gate exited before {marker}")
        require(len(line) + len(part) <= 128, f"{host}: oversized ring gate reply")
        line += part
    pattern = rb"RELEASED ([1-9][0-9]*)\n" if marker == "RELEASED" else marker.encode("ascii") + b"\n"
    match = re.fullmatch(pattern, line)
    require(match, f"{host}: unexpected ring gate reply {line!r}")
    return int(match[1]) if marker == "RELEASED" else None


def start_group(commands, endpoints, invocations, *, timeout=15):
    # Capture every successfully started invocation, including those finishing
    # after another host fails, so the caller can stop all workers it owns.
    errors = []
    with ThreadPoolExecutor(max_workers=len(endpoints)) as pool:
        jobs = [(endpoint, pool.submit(start, commands, endpoint, timeout=timeout))
                for endpoint in endpoints]
        for endpoint, job in jobs:
            try:
                invocations[endpoint] = job.result()
            except Exception as error:
                errors.append(f"{endpoint.label}: {error}")
    if errors:
        raise CycleError("\n".join(errors))


def release_ring(commands, output, sample, invocations, *, senders=None, timeout=None):
    senders = SENDERS if senders is None else senders
    timeout = GATE_TIMEOUT if timeout is None else timeout
    with ExitStack() as gates:
        deadline = time.monotonic() + timeout
        children = {}
        for host in dict.fromkeys(tx.host for tx in senders):
            local = [tx for tx in senders if tx.host == host]
            ports = ",".join(str(tx.port) for tx in local)
            children[host] = gates.enter_context(coordinator(
                commands, local[0], output / f"{sample}-{host}-gate.log", ports=ports, timeout=timeout,
            ))
        for host, child in children.items():
            read_gate_event(child, host, "LISTENING", deadline)
        start_group(commands, senders, invocations)
        for host, child in children.items():
            read_gate_event(child, host, "READY", deadline)

        timing = {}
        for host, child in children.items():
            sent = time.monotonic_ns()
            child.stdin.write(b"G")
            timing[host] = {"controller_sent_ns": sent, "controller_write_done_ns": time.monotonic_ns()}
        for host, child in children.items():
            timing[host]["sender_gate_ns"] = read_gate_event(child, host, "RELEASED", deadline)
            timing[host]["controller_received_ns"] = time.monotonic_ns()
        return timing


def log_path(output, sample, endpoint):
    return output / f"{sample}-{endpoint.host}-{endpoint.name}.txt"


def cleanup_invocations(commands, output, sample, invocations):
    failed = sys.exc_info()[0] is not None
    errors = []
    for endpoint, invocation in reversed(tuple(invocations.items())):
        path = log_path(output, sample, endpoint)
        if failed and not path.exists():
            try:
                messages = invocation_messages(commands, endpoint, invocation)
                path.write_text("\n".join(messages) + "\n")
            except (OSError, CycleError, ValueError) as error:
                errors.append(str(error))
        try:
            stop_owned(commands, endpoint, invocation)
        except CycleError as error:
            errors.append(str(error))
    if errors:
        if not failed:
            raise CycleError("\n".join(errors))
        print("\n".join(errors), file=sys.stderr)


def summarize(timing, results, edges=None):
    edges = EDGES if edges is None else edges
    links, earliest_finishes, latest_starts = {}, [], []
    for tx, rx in edges:
        tx_result = results[tx]
        gate = timing[tx.host]
        delay = int(tx_result["start_ns"]) - gate["sender_gate_ns"]
        require(delay >= 0, f"{tx.host}: sender started before its local release")
        # Translate durations using bounds on the coordinator's own gate event.
        # Raw monotonic stamps from separate hosts are never compared.
        earliest_finishes.append(gate["controller_sent_ns"] + GATE_LEAD_NS + delay +
                                 int(tx_result["elapsed_ns"]))
        latest_starts.append(gate["controller_received_ns"] + GATE_LEAD_NS + delay)
        links[edge_name(tx, rx)] = {
            "tx": tx_result, "rx": results[rx], "local_start_after_gate_ns": delay,
        }
    return {
        "controller_release_span_ns": max(event["controller_write_done_ns"] for event in timing.values()) -
                                      min(event["controller_sent_ns"] for event in timing.values()),
        "overlap_lower_bound_ns": max(0, min(earliest_finishes) - max(latest_starts)),
        "verified_payload_bytes": len(edges) * PAYLOAD,
        "gate": timing,
        "links": links,
    }


def run_sample(commands, output, sample):
    invocations = {}
    try:
        for endpoint in RECEIVERS:
            invocations[endpoint] = start(commands, endpoint)
        deadline = time.monotonic() + 20
        for endpoint in RECEIVERS:
            wait_endpoint(commands, endpoint, invocations[endpoint], deadline, ready=True)
            verify_loopback_listener(commands, endpoint.host, endpoint.port)
        timing = release_ring(commands, output, sample, invocations)

        results = {}
        deadline = time.monotonic() + 60
        for endpoint in (*SENDERS, *RECEIVERS):
            invocation = invocations[endpoint]
            messages = wait_endpoint(commands, endpoint, invocation, deadline)
            log_path(output, sample, endpoint).write_text("\n".join(messages) + "\n")
            results[endpoint] = {"invocation": invocation, **parse_result(endpoint, messages)}
        return summarize(timing, results)
    finally:
        cleanup_invocations(commands, output, sample, invocations)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", required=True, type=Path, help="pinned installed snapshot directory")
    parser.add_argument("--sha256", required=True, help="expected SHA-256 of the installed roce_write")
    parser.add_argument("--samples", type=int, default=3, help="trials to run (default 3)")
    parser.add_argument("--output", type=Path, help="new directory for logs and results")
    args = parser.parse_args()
    digest = args.sha256.lower()
    if not re.fullmatch(r"[a-f0-9]{64}", digest):
        parser.error("--sha256 must contain 64 hexadecimal digits")
    if args.destination.parent != DESTINATION_ROOT or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*",
                                                                       args.destination.name):
        parser.error(f"--destination must name a snapshot directly under {DESTINATION_ROOT}")
    if args.samples <= 0:
        parser.error("--samples must be positive")
    require(socket.gethostname().split(".")[0] == "spark-7e64", "run the benchmark from Spark-04")

    with tempfile.TemporaryDirectory(prefix="roce-ring-control-") as sockets:
        commands = Commands(sockets)
        links = preflight(commands, args.destination, digest)
        output = args.output or Path(tempfile.mkdtemp(prefix="roce-ring-results-"))
        if args.output:
            output.mkdir()
        print(f"Ring benchmark output {output}", flush=True)
        trials = []
        report = {"sha256": digest, "destination": str(args.destination), "links": links, "trials": trials}
        with control_tunnels(commands, sockets):
            for sample in range(1, args.samples + 1):
                trial = run_sample(commands, output, sample)
                trials.append(trial)
                report["median_payload_gbps"] = {
                    link: {role: statistics.median(float(item["links"][link][role]["payload_gbps"])
                                                   for item in trials) for role in ("tx", "rx")}
                    for link in links
                }
                save_report(output, report)
                print(f"Ring sample {sample}, release span {trial['controller_release_span_ns']} ns, "
                      f"overlap lower bound {trial['overlap_lower_bound_ns']} ns", flush=True)
                for link, fields in trial["links"].items():
                    rate = fields["tx"]["payload_gbps"]
                    retries = fields["tx"]["retries"]
                    print(f"{link} verified TX payload {rate} Gbit/s, {retries} retries", flush=True)
                require(trial["overlap_lower_bound_ns"] > 0, "could not verify overlap across the ring")


if __name__ == "__main__":
    try:
        main()
    except (CycleError, OSError, ValueError, KeyError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
