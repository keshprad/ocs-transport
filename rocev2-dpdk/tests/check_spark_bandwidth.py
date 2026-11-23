#!/usr/bin/env python3
"""Measure verified RoCEv2 DPDK WRITEs between Spark-04 and Spark-02."""

import argparse
from contextlib import ExitStack, contextmanager
from dataclasses import dataclass
import json
import math
from pathlib import Path
import re
import shlex
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "mrc-dpdk" / "tests"))
from check_spark_cycle import ACTIVE_STATES, Commands, CycleError, require, start, stop_owned


WRITE_BYTES = 1048576
PAYLOAD = 16 * (1 << 30)
PMTU = 1024
PACKETS = PAYLOAD // PMTU
TX_BURST = 16
SEND_WINDOW = 256
CONTROL_IP = "10.61.0.13"
DESTINATION_ROOT = Path("/opt/ocs-roce-bw")
LOOPBACK = "127.0.0.1"
GATE_PORT = 28872
GATE_TIMEOUT = 20
GATE_LEAD_NS = 50_000_000
PORTS = {
    "tx": ("spark-04", "0000:01:00.0", "enp1s0f0np0", "10.61.0.2"),
    "rx": ("spark-02", "0000:01:00.1", "enp1s0f1np1", "10.61.0.1"),
}
SECOND_RAIL = {
    "tx": ("spark-04", "0002:01:00.0", "enP2p1s0f0np0", "10.61.0.14"),
    "rx": ("spark-02", "0002:01:00.1", "enP2p1s0f1np1", "10.61.0.13"),
}


@dataclass(frozen=True)
class Profile:
    port: int
    qps: int
    workers: int
    rail: int = 1
    aggregate: bool = False

    @property
    def writes(self):
        return PAYLOAD // (self.qps * WRITE_BYTES)

    @property
    def cpus(self):
        first = 5 + 10 * (self.rail - 1)
        return tuple(range(first, first + self.workers))

    @property
    def runtime_dir(self):
        return Path("/run/ocs-roce-bw" if self.rail == 1 else "/run/ocs-roce-bw-2")


PROFILES = {
    "single": Profile(28866, 1, 1),
    "multi-1": Profile(28867, 16, 1),
    "multi-2": Profile(28868, 16, 2),
    "multi-4": Profile(28869, 16, 4),
}
AGGREGATE_PROFILES = {
    "aggregate-1": Profile(28870, 16, 4, rail=1, aggregate=True),
    "aggregate-2": Profile(28871, 16, 4, rail=2, aggregate=True),
}
ALL_PROFILES = PROFILES | AGGREGATE_PROFILES


@dataclass(frozen=True)
class Endpoint:
    role: str
    profile: str

    @property
    def settings(self):
        return ALL_PROFILES[self.profile]

    @property
    def link(self):
        return (PORTS if self.settings.rail == 1 else SECOND_RAIL)[self.role]

    @property
    def host(self):
        return self.link[0]

    @property
    def unit(self):
        return f"ocs-roce-bw-{self.profile}.service"

    @property
    def label(self):
        return f"{self.host} {self.unit}"


def unit_argv(endpoint, destination, runtime):
    profile = endpoint.settings
    _, pci, _, data_ip = endpoint.link
    cpus = ",".join(map(str, profile.cpus))
    lock = profile.runtime_dir / "function.lock"
    control_ip = LOOPBACK if profile.aggregate else CONTROL_IP
    argv = [
        "/usr/bin/flock", "--exclusive", "--nonblock", str(lock),
        str(destination / "roce_write"), "-l", cpus, "--iova-mode=va", "--no-huge",
        "--no-telemetry", f"--file-prefix=ocs-roce-bw-{endpoint.profile}",
        "-d", str(runtime / "librte_mempool_ring.so.24"),
        "-d", str(runtime / "librte_net_mlx5.so.24"), "-a", pci,
        "--", endpoint.role, str(WRITE_BYTES), control_ip, str(profile.port), data_ip, str(PMTU),
        pci, "--report-bw", "--tx-burst", str(TX_BURST), "--send-window", str(SEND_WINDOW),
        "--writes", str(profile.writes), "--qps", str(profile.qps), "--workers", str(profile.workers),
    ]
    if profile.aggregate and endpoint.role == "tx":
        argv += ["--start-gate", str(GATE_PORT)]
    return argv


def verify_unit(commands, endpoint, destination):
    props = commands.properties(
        endpoint, "LoadState", "ActiveState", "WorkingDirectory", "ExecStart",
        "Environment", "UnsetEnvironment",
    )
    require(props.get("LoadState") == "loaded", f"{endpoint.label}: benchmark unit is missing")
    require(props.get("ActiveState") in ("inactive", "failed"), f"{endpoint.label}: already running")
    require(props.get("WorkingDirectory") == str(destination), f"{endpoint.label}: wrong snapshot")
    require("RUNTIME_DIRECTORY" in shlex.split(props.get("UnsetEnvironment", "")),
            f"{endpoint.label}: missing UnsetEnvironment=RUNTIME_DIRECTORY")
    environment = shlex.split(props.get("Environment", ""))
    if endpoint.settings.aggregate:
        require(f"XDG_RUNTIME_DIR={endpoint.settings.runtime_dir}" in environment,
                f"{endpoint.label}: wrong DPDK runtime directory")
    runtimes = [Path(word.split("=", 1)[1]) for word in environment
                if word.startswith("LD_LIBRARY_PATH=")]
    require(len(runtimes) == 1 and runtimes[0].is_absolute() and ":" not in str(runtimes[0]),
            f"{endpoint.label}: expected one absolute DPDK runtime library path")
    expected = unit_argv(endpoint, destination, runtimes[0])
    installed = re.findall(r"argv\[\]=(.*?) ;", props.get("ExecStart", ""))
    require(len(installed) == 1 and shlex.split(installed[0]) == expected,
            f"{endpoint.label}: installed ExecStart differs from the expected benchmark profile")
    for operation in ("start", "stop"):
        commands.run(endpoint.host, "sudo", "-n", "-l", "/usr/bin/systemctl", operation, endpoint.unit)
    # A personal journal can be readable even when the unit's records are hidden.
    commands.sudo_journals.add(endpoint)
    commands.journal(endpoint)


def verify_link(commands, link):
    host, pci, interface, data_ip = link
    param = commands.run(host, "devlink", "dev", "param", "show", f"pci/{pci}", "name", "enable_roce")
    require("cmode driverinit value false" in param, f"{host} {pci}: native RoCE still enabled")
    addr = commands.run(host, "ip", "-br", "-4", "address", "show", "dev", interface).split()
    require("UP" in addr and f"{data_ip}/30" in addr, f"{host} {interface}: link or address not ready")
    sysfs = Path("/sys/class/net") / interface
    device = Path(commands.run(host, "readlink", "-f", "--", sysfs / "device").strip()).name
    require(device == pci, f"{host} {interface}: expected PCI function {pci}, found {device}")
    return {
        "host": host, "device": pci, "interface": interface, "data_ip": data_ip,
        "ethernet_mtu": int(commands.run(host, "cat", sysfs / "mtu")),
        "link_mbps": int(commands.run(host, "cat", sysfs / "speed")),
    }


def preflight(commands, profiles, destination, digest):
    aggregate = ALL_PROFILES[profiles[0]].aggregate
    links = {name: {} for name in profiles} if aggregate else {}
    for role, (host, _, _, _) in PORTS.items():
        if aggregate:
            for name in profiles:
                endpoint = Endpoint(role, name)
                links[name][role] = {
                    **verify_link(commands, endpoint.link), "cpu_ids": list(endpoint.settings.cpus),
                }
        else:
            links[role] = verify_link(commands, PORTS[role])
        active = commands.run(host, "systemctl", "list-units", "--state=running,activating,deactivating",
                              "--no-pager", "--no-legend", "--plain", "ocs-mrc*.service", "ocs-roce*.service")
        require(not active.strip(), f"{host}: another lab service is running")

        executable = destination / "roce_write"
        for path in (destination, executable):
            require(commands.run(host, "realpath", "-e", "--", path).strip() == str(path),
                    f"{host}: unexpected installed path for {path}")
            owner, mode = commands.run(host, "stat", "-c", "%u %a", "--", path).split()
            require(owner == "0" and not (int(mode, 8) & 0o022),
                    f"{host}: {path} must be writable only by root")
        actual = commands.run(host, "sha256sum", "--", executable).split()[0]
        require(actual == digest, f"{host}: installed binary does not match the expected SHA-256")
        for profile in profiles:
            verify_unit(commands, Endpoint(role, profile), destination)
    return links


def verify_loopback_listener(commands, host, port):
    sockets = commands.run(host, "ss", "-H", "-ltn", "sport", "=", f":{port}")
    addresses = [line.split()[3] for line in sockets.splitlines()]
    require(addresses == [f"{LOOPBACK}:{port}"],
            f"{host}: expected one loopback listener on port {port}, found {addresses}")


@contextmanager
def aggregate_tunnel(commands, socket_dir):
    host = PORTS["rx"][0]
    control = Path(socket_dir) / "aggregate-forward"
    ssh = ["ssh", "-S", str(control), "-o", "BatchMode=yes"]
    argv = [
        *ssh, "-fNT", "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=5",
        "-o", "ServerAliveCountMax=2", "-o", "ControlMaster=yes",
        "-o", "ControlPersist=no", "-o", "ExitOnForwardFailure=yes",
    ]
    for profile in AGGREGATE_PROFILES.values():
        argv += ["-L", f"{LOOPBACK}:{profile.port}:{LOOPBACK}:{profile.port}"]
    try:
        result = subprocess.run([*argv, host], capture_output=True, text=True, timeout=15,
                                stdin=subprocess.DEVNULL)
        require(result.returncode == 0, f"{host}: could not open aggregate tunnel: {result.stderr.strip()}")
        for profile in AGGREGATE_PROFILES.values():
            verify_loopback_listener(commands, PORTS["tx"][0], profile.port)
        yield
    except subprocess.TimeoutExpired as error:
        raise CycleError(f"{host}: timed out opening aggregate tunnel") from error
    finally:
        if control.exists():
            failed = sys.exc_info()[0] is not None
            try:
                result = subprocess.run([*ssh, "-O", "exit", host], capture_output=True,
                                        text=True, timeout=5, stdin=subprocess.DEVNULL)
                require(result.returncode == 0 or not control.exists(),
                        f"{host}: aggregate tunnel cleanup failed: {result.stderr.strip()}")
            except (CycleError, OSError, subprocess.TimeoutExpired) as error:
                if not failed:
                    raise CycleError(f"{host}: could not close aggregate tunnel: {error}") from error
                print(error, file=sys.stderr)


def gate_read_identity(connection, deadline):
    identity = b""
    while len(identity) < 2:
        remaining = deadline - time.monotonic()
        require(remaining > 0, "timed out waiting for aggregate sender identity")
        connection.settimeout(remaining)
        part = connection.recv(2 - len(identity))
        require(bool(part), "aggregate sender closed the gate before identifying itself")
        identity += part
    return struct.unpack("!H", identity)[0]


def release_senders(listener, expected, deadline):
    pending = set(expected)
    with ExitStack() as connections:
        senders = []
        try:
            while pending:
                remaining = deadline - time.monotonic()
                require(remaining > 0, f"timed out waiting for aggregate senders on ports {sorted(pending)}")
                listener.settimeout(remaining)
                connection, _ = listener.accept()
                connections.enter_context(connection)
                port = gate_read_identity(connection, deadline)
                require(port in pending, f"unexpected or duplicate aggregate sender port {port}")
                pending.remove(port)
                senders.append(connection)
            release_ns = time.monotonic_ns() + GATE_LEAD_NS
            release = struct.pack("!Q", release_ns)
            for sender in senders:
                sender.settimeout(GATE_LEAD_NS / 1_000_000_000)
                sender.sendall(release)
        except TimeoutError as error:
            raise CycleError(f"aggregate start gate timed out, pending control ports {sorted(pending)}") from error
    return release_ns


def invocation_messages(commands, endpoint, invocation):
    output = commands.journal(endpoint, f"_SYSTEMD_INVOCATION_ID={invocation}")
    messages = []
    for row in output.splitlines():
        event = json.loads(row)
        if event.get("_SYSTEMD_INVOCATION_ID") == invocation and isinstance(event.get("MESSAGE"), str):
            messages.append(event["MESSAGE"])
    return messages


def wait_endpoint(commands, endpoint, invocation, deadline, *, ready=False):
    while time.monotonic() < deadline:
        status = commands.properties(endpoint, "ActiveState", "InvocationID", "Result", "ExecMainStatus")
        active = status.get("ActiveState") in ACTIVE_STATES
        if active:
            require(status.get("InvocationID") == invocation, f"{endpoint.label}: invocation changed")
        messages = invocation_messages(commands, endpoint, invocation)
        if ready and active and "control listener ready" in messages:
            return messages
        if not active:
            require(
                not ready and status.get("ActiveState") == "inactive"
                and status.get("Result") == "success" and status.get("ExecMainStatus") == "0",
                f"{endpoint.label}: unexpected service exit\n" + "\n".join(messages[-12:]),
            )
            # Read after observing the terminal state so journal records can settle.
            if any(line.startswith("BANDWIDTH ") for line in messages):
                return messages
        time.sleep(0.2)
    phase = "receiver readiness" if ready else "completion and verification"
    raise CycleError(f"{endpoint.label}: timed out waiting for {phase} ({invocation})")


def parse_result(endpoint, messages):
    lines = [line for line in messages if line.startswith("BANDWIDTH ")]
    require(len(lines) == 1, f"{endpoint.label}: expected one bandwidth result")
    fields = dict(field.split("=", 1) for field in lines[0].split()[1:])
    profile = endpoint.settings
    expected = {
        "role": endpoint.role, "payload_bytes": str(PAYLOAD), "writes": str(profile.writes),
        "qps": str(profile.qps), "workers": str(profile.workers),
    }
    if endpoint.role == "tx":
        expected.update(send_window=str(SEND_WINDOW), tx_burst=str(TX_BURST))
    require(all(fields.get(key) == value for key, value in expected.items()),
            f"{endpoint.label}: unexpected bandwidth profile: {lines[0]}")

    elapsed = int(fields["elapsed_ns"])
    cpu = int(fields["thread_cpu_ns"])
    rate = float(fields["payload_gbps"])
    cores = float(fields["poll_cores"])
    require(elapsed > 0 and cpu > 0 and cores > 0 and rate > 0,
            f"{endpoint.label}: expected positive bandwidth and CPU counters")
    require(math.isclose(rate, PAYLOAD * 8 / elapsed, rel_tol=0, abs_tol=0.000501)
            and math.isclose(cores, cpu / elapsed, rel_tol=0, abs_tol=0.000501),
            f"{endpoint.label}: inconsistent bandwidth or CPU counters")
    if profile.aggregate:
        require(int(fields.get("start_ns", "0")) > 0, f"{endpoint.label}: missing monotonic start")

    if endpoint.role == "tx":
        pattern = rf"WRITE complete: {PAYLOAD} bytes, {PACKETS} packets, ([0-9]+) retries, peer verified"
        matches = [re.fullmatch(pattern, message) for message in messages]
        retries = [int(match[1]) for match in matches if match]
        require(len(retries) == 1, f"{endpoint.label}: missing verified sender completion or retry count")
        fields["retries"] = str(retries[0])
    else:
        marker = f"WRITE verified: {PAYLOAD} bytes, {PACKETS} packets"
        require(messages.count(marker) == 1, f"{endpoint.label}: missing byte verification")
    counters = [line for line in messages if line.startswith("PORT_STATS ")]
    if counters:
        require(len(counters) == 1, f"{endpoint.label}: expected one port counter result")
        fields["port_stats"] = {name: int(value) for name, value in
                                (field.split("=", 1) for field in counters[0].split()[1:])}
    return fields


def run_endpoints(commands, profiles, output, sample):
    receivers = [Endpoint("rx", name) for name in profiles]
    senders = [Endpoint("tx", name) for name in profiles]
    aggregate = senders[0].settings.aggregate
    invocations = {}
    try:
        for endpoint in receivers:
            invocations[endpoint] = start(commands, endpoint)
        ready_deadline = time.monotonic() + 20
        for endpoint in receivers:
            wait_endpoint(commands, endpoint, invocations[endpoint], ready_deadline, ready=True)
            if aggregate:
                verify_loopback_listener(commands, endpoint.host, endpoint.settings.port)
        release_ns = None
        with ExitStack() as gates:
            if aggregate:
                listener = gates.enter_context(socket.create_server((LOOPBACK, GATE_PORT)))
                gate_deadline = time.monotonic() + GATE_TIMEOUT
            for endpoint in senders:
                invocations[endpoint] = start(commands, endpoint)
            if aggregate:
                release_ns = release_senders(listener, [endpoint.settings.port for endpoint in senders],
                                             gate_deadline)
        deadline = time.monotonic() + 60
        results = {name: {} for name in profiles}
        for endpoint in (*senders, *receivers):
            invocation = invocations[endpoint]
            messages = wait_endpoint(commands, endpoint, invocation, deadline)
            (output / f"{endpoint.profile}-{sample}-{endpoint.role}.txt").write_text("\n".join(messages) + "\n")
            fields = parse_result(endpoint, messages)
            results[endpoint.profile][endpoint.role] = {"invocation": invocation, **fields}
            print(f"{endpoint.label} sample {sample} {invocation} {fields}", flush=True)
        return results, release_ns
    finally:
        failed = sys.exc_info()[0] is not None
        cleanup_failures = []
        for endpoint, invocation in reversed(tuple(invocations.items())):
            path = output / f"{endpoint.profile}-{sample}-{endpoint.role}.txt"
            if failed and not path.exists():
                try:
                    messages = invocation_messages(commands, endpoint, invocation)
                    path.write_text("\n".join(messages) + "\n")
                except (OSError, CycleError, ValueError) as error:
                    print(error, file=sys.stderr)
            try:
                stop_owned(commands, endpoint, invocation)
            except CycleError as error:
                cleanup_failures.append(str(error))
        if cleanup_failures:
            message = "\n".join(cleanup_failures)
            if not failed:
                raise CycleError(message)
            print(message, file=sys.stderr)


def aggregate_result(rails, release_ns):
    starts = [int(rail["tx"]["start_ns"]) for rail in rails.values()]
    elapsed = [int(rail["tx"]["elapsed_ns"]) for rail in rails.values()]
    finishes = [start + duration for start, duration in zip(starts, elapsed)]
    first_start = min(starts)
    require(first_start >= release_ns, "an aggregate sender started before the release timestamp")
    envelope = max(finishes) - first_start
    overlap = max(0, min(finishes) - max(starts))
    payload = sum(int(rail["tx"]["payload_bytes"]) for rail in rails.values())
    return {
        "release_ns": release_ns, "start_ns": first_start, "end_ns": max(finishes),
        "elapsed_ns": envelope, "start_skew_ns": max(starts) - first_start,
        "overlap_ns": overlap, "overlap_fraction": overlap / envelope,
        "payload_bytes": payload, "payload_gbps": payload * 8 / envelope,
        "tx_retries": sum(int(rail["tx"]["retries"]) for rail in rails.values()),
        "thread_cpu_ns": {
            role: sum(int(rail[role]["thread_cpu_ns"]) for rail in rails.values())
            for role in PORTS
        },
        "rails": rails,
    }


def save_report(output, report):
    pending = output / "results.json.tmp"
    pending.write_text(json.dumps(report, indent=2) + "\n")
    pending.replace(output / "results.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", required=True, type=Path, help="pinned installed snapshot directory")
    parser.add_argument("--sha256", required=True, help="expected SHA-256 of the installed roce_write binary")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--profile", action="append", choices=PROFILES, help="repeat to select profiles")
    selection.add_argument("--aggregate", action="store_true",
                           help="run four-worker 16-QP transfers concurrently on both 100 Gbit/s rails")
    parser.add_argument("--samples", type=int, default=3, help="trials per profile (default 3)")
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
    profiles = tuple(AGGREGATE_PROFILES) if args.aggregate else tuple(dict.fromkeys(args.profile or PROFILES))

    require(socket.gethostname().split(".")[0] == "spark-7e64", "run the benchmark from Spark-04")
    with tempfile.TemporaryDirectory(prefix="roce-bw-control-") as sockets:
        commands = Commands(sockets)
        links = preflight(commands, profiles, args.destination, digest)
        output = args.output or Path(tempfile.mkdtemp(prefix="roce-bw-results-"))
        if args.output:
            output.mkdir()
        print(f"Benchmark output {output}", flush=True)
        report = {"sha256": digest, "destination": str(args.destination), "links": links}
        if args.aggregate:
            trials = []
            report["aggregate"] = {"trials": trials}
            with aggregate_tunnel(commands, sockets):
                for sample in range(1, args.samples + 1):
                    rails, release_ns = run_endpoints(commands, profiles, output, sample)
                    trial = aggregate_result(rails, release_ns)
                    trials.append(trial)
                    median = statistics.median(item["payload_gbps"] for item in trials)
                    report["aggregate"]["median_payload_gbps"] = median
                    save_report(output, report)
                    require(trial["overlap_ns"] > 0, "aggregate sender intervals did not overlap")
                    print(f"aggregate sample {sample} verified {trial['payload_gbps']:.3f} Gbit/s, "
                          f"{trial['overlap_fraction']:.1%} overlap, "
                          f"{trial['start_skew_ns'] / 1_000_000:.3f} ms start skew", flush=True)
            print(f"aggregate median verified payload TX {median:.3f} Gbit/s", flush=True)
            return
        report["profiles"] = {}
        for profile in profiles:
            trials = []
            profile_result = {"trials": trials}
            report["profiles"][profile] = profile_result
            for sample in range(1, args.samples + 1):
                results, _ = run_endpoints(commands, (profile,), output, sample)
                trials.append(results[profile])
                median = {role: statistics.median(float(trial[role]["payload_gbps"]) for trial in trials)
                          for role in PORTS}
                profile_result["median_payload_gbps"] = median
                save_report(output, report)
            print(f"{profile} median verified payload TX {median['tx']:.3f} RX {median['rx']:.3f} Gbit/s",
                  flush=True)


if __name__ == "__main__":
    try:
        main()
    except (CycleError, OSError, ValueError, KeyError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
