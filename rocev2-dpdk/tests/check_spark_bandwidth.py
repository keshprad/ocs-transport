#!/usr/bin/env python3
"""Measure verified single-rail RoCEv2 DPDK WRITEs between Spark-04 and Spark-02."""

import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import re
import shlex
import socket
import statistics
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
UNIT_LOCK = "/run/ocs-roce-bw/function.lock"
DESTINATION_ROOT = Path("/opt/ocs-roce-bw")
PORTS = {
    "tx": ("spark-04", "0000:01:00.0", "enp1s0f0np0", "10.61.0.2"),
    "rx": ("spark-02", "0000:01:00.1", "enp1s0f1np1", "10.61.0.1"),
}


@dataclass(frozen=True)
class Profile:
    port: int
    qps: int
    workers: int

    @property
    def writes(self):
        return PAYLOAD // (self.qps * WRITE_BYTES)


PROFILES = {
    "single": Profile(28866, 1, 1),
    "multi-1": Profile(28867, 16, 1),
    "multi-2": Profile(28868, 16, 2),
    "multi-4": Profile(28869, 16, 4),
}


@dataclass(frozen=True)
class Endpoint:
    role: str
    profile: str

    @property
    def host(self):
        return PORTS[self.role][0]

    @property
    def unit(self):
        return f"ocs-roce-bw-{self.profile}.service"

    @property
    def label(self):
        return f"{self.host} {self.unit}"


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
    runtimes = [Path(word.split("=", 1)[1]) for word in shlex.split(props.get("Environment", ""))
                if word.startswith("LD_LIBRARY_PATH=")]
    require(len(runtimes) == 1 and runtimes[0].is_absolute() and ":" not in str(runtimes[0]),
            f"{endpoint.label}: expected one absolute DPDK runtime library path")
    runtime = runtimes[0]
    profile = PROFILES[endpoint.profile]
    _, pci, _, data_ip = PORTS[endpoint.role]
    cpus = ",".join(str(cpu) for cpu in range(5, 5 + profile.workers))
    expected = [
        "/usr/bin/flock", "--exclusive", "--nonblock", UNIT_LOCK,
        str(destination / "roce_write"), "-l", cpus, "--iova-mode=va", "--no-huge",
        "--no-telemetry", f"--file-prefix=ocs-roce-bw-{endpoint.profile}",
        "-d", str(runtime / "librte_mempool_ring.so.24"),
        "-d", str(runtime / "librte_net_mlx5.so.24"), "-a", pci,
        "--", endpoint.role, str(WRITE_BYTES), CONTROL_IP, str(profile.port), data_ip, str(PMTU),
        pci, "--report-bw", "--tx-burst", str(TX_BURST), "--send-window", str(SEND_WINDOW),
        "--writes", str(profile.writes), "--qps", str(profile.qps), "--workers", str(profile.workers),
    ]
    installed = re.findall(r"argv\[\]=(.*?) ;", props.get("ExecStart", ""))
    require(len(installed) == 1 and shlex.split(installed[0]) == expected,
            f"{endpoint.label}: installed ExecStart differs from the expected benchmark profile")
    for operation in ("start", "stop"):
        commands.run(endpoint.host, "sudo", "-n", "-l", "/usr/bin/systemctl", operation, endpoint.unit)
    # A personal journal can be readable even when the unit's records are hidden.
    commands.sudo_journals.add(endpoint)
    commands.journal(endpoint)


def preflight(commands, profiles, destination, digest):
    links = {}
    for role, (host, pci, interface, data_ip) in PORTS.items():
        param = commands.run(host, "devlink", "dev", "param", "show", f"pci/{pci}", "name", "enable_roce")
        require("cmode driverinit value false" in param, f"{host} {pci}: native RoCE still enabled")
        addr = commands.run(host, "ip", "-br", "-4", "address", "show", "dev", interface).split()
        require("UP" in addr and f"{data_ip}/30" in addr, f"{host} {interface}: link or address not ready")
        sysfs = Path("/sys/class/net") / interface
        device = Path(commands.run(host, "readlink", "-f", "--", sysfs / "device").strip()).name
        require(device == pci, f"{host} {interface}: expected PCI function {pci}, found {device}")
        links[role] = {
            "host": host, "device": pci, "interface": interface, "data_ip": data_ip,
            "ethernet_mtu": int(commands.run(host, "cat", sysfs / "mtu")),
            "link_mbps": int(commands.run(host, "cat", sysfs / "speed")),
        }
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
    profile = PROFILES[endpoint.profile]
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

    if endpoint.role == "tx":
        pattern = rf"WRITE complete: {PAYLOAD} bytes, {PACKETS} packets, ([0-9]+) retries, peer verified"
        matches = [re.fullmatch(pattern, message) for message in messages]
        retries = [int(match[1]) for match in matches if match]
        require(len(retries) == 1, f"{endpoint.label}: missing verified sender completion or retry count")
        fields["retries"] = str(retries[0])
    else:
        marker = f"WRITE verified: {PAYLOAD} bytes, {PACKETS} packets"
        require(messages.count(marker) == 1, f"{endpoint.label}: missing byte verification")
    return fields


def run_sample(commands, profile, output, sample):
    rx = Endpoint("rx", profile)
    tx = Endpoint("tx", profile)
    invocations = {}
    try:
        invocations[rx] = start(commands, rx)
        wait_endpoint(commands, rx, invocations[rx], time.monotonic() + 20, ready=True)
        invocations[tx] = start(commands, tx)
        deadline = time.monotonic() + 60
        results = {}
        for endpoint in (tx, rx):
            invocation = invocations[endpoint]
            messages = wait_endpoint(commands, endpoint, invocation, deadline)
            (output / f"{profile}-{sample}-{endpoint.role}.txt").write_text("\n".join(messages) + "\n")
            fields = parse_result(endpoint, messages)
            results[endpoint.role] = {"invocation": invocation, **fields}
            print(f"{endpoint.label} sample {sample} {invocation} {fields}", flush=True)
        return results
    finally:
        failed = sys.exc_info()[0] is not None
        cleanup_failures = []
        for endpoint, invocation in reversed(tuple(invocations.items())):
            path = output / f"{profile}-{sample}-{endpoint.role}.txt"
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", required=True, type=Path, help="pinned installed snapshot directory")
    parser.add_argument("--sha256", required=True, help="expected SHA-256 of the installed roce_write binary")
    parser.add_argument("--profile", action="append", choices=PROFILES, help="repeat to select profiles")
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
    profiles = tuple(dict.fromkeys(args.profile or PROFILES))

    require(socket.gethostname().split(".")[0] == "spark-7e64", "run the benchmark from Spark-04")
    with tempfile.TemporaryDirectory(prefix="roce-bw-control-") as sockets:
        commands = Commands(sockets)
        links = preflight(commands, profiles, args.destination, digest)
        output = args.output or Path(tempfile.mkdtemp(prefix="roce-bw-results-"))
        if args.output:
            output.mkdir()
        print(f"Benchmark output {output}", flush=True)
        report = {"sha256": digest, "destination": str(args.destination), "links": links, "profiles": {}}
        for profile in profiles:
            trials = []
            profile_result = {"trials": trials}
            report["profiles"][profile] = profile_result
            for sample in range(1, args.samples + 1):
                trials.append(run_sample(commands, profile, output, sample))
                median = {role: statistics.median(float(trial[role]["payload_gbps"]) for trial in trials)
                          for role in PORTS}
                profile_result["median_payload_gbps"] = median
                pending = output / "results.json.tmp"
                pending.write_text(json.dumps(report, indent=2) + "\n")
                pending.replace(output / "results.json")
            print(f"{profile} median verified payload TX {median['tx']:.3f} RX {median['rx']:.3f} Gbit/s",
                  flush=True)


if __name__ == "__main__":
    try:
        main()
    except (CycleError, OSError, ValueError, KeyError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
