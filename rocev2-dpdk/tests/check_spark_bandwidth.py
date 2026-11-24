#!/usr/bin/env python3
"""Measure verified RoCEv2 DPDK WRITEs between Spark-04 and Spark-02."""

import argparse
from contextlib import ExitStack, contextmanager
from dataclasses import dataclass, replace
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
TX_BURST = 16
SEND_WINDOW = 256
CONTROL_IP = "10.61.0.13"
DESTINATION_ROOT = Path("/opt/ocs-roce-bw")
APP_DESTINATION_ROOT = Path("/opt/ocs-roce-app-bw")
APP_FILES = ("roce_write", "roce_app_write", "run_app", "provider/libroce_dpdk-rdmav34.so")
APP_MODES = ("memfd-gather", "mr-copy", "mr-gather")
APP_POST_BATCH = 32
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
    pmtu: int = PMTU
    ethernet_mtu: int | None = None
    payload: int = PAYLOAD
    app_mode: str | None = None
    prefault_app: bool = False

    @property
    def writes(self):
        return self.payload // (self.qps * WRITE_BYTES)

    @property
    def packets(self):
        return self.qps * self.writes * ((WRITE_BYTES + self.pmtu - 1) // self.pmtu)

    @property
    def uses_tunnel(self):
        return self.aggregate or self.ethernet_mtu is not None

    @property
    def cpus(self):
        first = 5 + 10 * (self.rail - 1)
        return tuple(range(first, first + self.workers))

    @property
    def runtime_dir(self):
        if self.app_mode:
            return Path("/run/ocs-roce-app-bw")
        return Path("/run/ocs-roce-bw" if self.rail == 1 else "/run/ocs-roce-bw-2")

    @property
    def native_mr(self):
        return self.app_mode in ("mr-copy", "mr-gather")

    @property
    def scatter_gather(self):
        return self.app_mode in ("memfd-gather", "mr-gather")


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
BURST_STYLE_PROFILES = {
    f"mtu-{mtu}-{name}": Profile(base + i, profile.qps, profile.workers, pmtu=pmtu, ethernet_mtu=mtu)
    for mtu, pmtu, base in ((1500, 1024, 28876), (4200, 4096, 28880))
    for i, (name, profile) in enumerate(PROFILES.items())
}
APP_SHAPES = PROFILES | {"smoke": Profile(0, 1, 1, payload=8 * WRITE_BYTES)}
APP_PROFILES = {
    f"app-{mode}-mtu-{mtu}-{name}": Profile(
        28900 + mode_index * 10 + mtu_index * 5 + index, profile.qps, profile.workers,
        pmtu=pmtu, ethernet_mtu=mtu, payload=profile.payload, app_mode=mode,
    )
    for mode_index, mode in enumerate(APP_MODES)
    for mtu_index, (mtu, pmtu) in enumerate(((1500, 1024), (4200, 4096)))
    for index, (name, profile) in enumerate(APP_SHAPES.items())
}
PREFAULT_PROFILES = {
    f"{name}-prefault": replace(profile, port=profile.port + len(APP_PROFILES), prefault_app=True)
    for name, profile in APP_PROFILES.items()
}
ALL_PROFILES = PROFILES | AGGREGATE_PROFILES | BURST_STYLE_PROFILES | APP_PROFILES | PREFAULT_PROFILES


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
    control_ip = LOOPBACK if profile.uses_tunnel else CONTROL_IP
    device = f"{pci},max_dump_files_num=1" if profile.app_mode else pci
    argv = [
        "/usr/bin/flock", "--exclusive", "--nonblock", str(lock),
        str(destination / ("run_app" if profile.app_mode else "roce_write")),
        "-l", cpus, "--iova-mode=va", "--no-huge",
        "--no-telemetry", f"--file-prefix=ocs-roce-bw-{endpoint.profile}",
        "-d", str(runtime / "librte_mempool_ring.so.24"),
        "-d", str(runtime / "librte_net_mlx5.so.24"), "-a", device,
        "--", endpoint.role, str(WRITE_BYTES), control_ip, str(profile.port), data_ip, str(profile.pmtu),
        pci, "--report-bw", "--tx-burst", str(TX_BURST), "--send-window", str(SEND_WINDOW),
        "--writes", str(profile.writes), "--qps", str(profile.qps), "--workers", str(profile.workers),
    ]
    if profile.aggregate and endpoint.role == "tx":
        argv += ["--start-gate", str(GATE_PORT)]
    if profile.native_mr:
        argv += ["--mr-socket", str(profile.runtime_dir / "registration.sock")]
    if profile.prefault_app:
        argv.append("--prefault-app")
    if profile.app_mode and endpoint.role == "tx":
        argv += ["--app-post-batch", str(APP_POST_BATCH)]
        if profile.native_mr:
            argv.append("--verbs-queues")
        if profile.scatter_gather:
            argv.append("--scatter-gather")
    return argv


def unit_environment(endpoint, destination, runtime):
    profile = endpoint.settings
    environment = {"LD_LIBRARY_PATH": str(runtime), "XDG_RUNTIME_DIR": str(profile.runtime_dir)}
    if profile.native_mr:
        environment.update(RDMAV_DRIVERS=str(destination / "provider/libroce_dpdk"),
                           ROCE_DPDK_MR_SOCKET=str(profile.runtime_dir / "registration.sock"),
                           ROCE_DPDK_MR_REQUIRE_NETLINK="1")
    return environment


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
    if endpoint.settings.uses_tunnel:
        require(f"XDG_RUNTIME_DIR={endpoint.settings.runtime_dir}" in environment,
                f"{endpoint.label}: wrong DPDK runtime directory")
    runtimes = [Path(word.split("=", 1)[1]) for word in environment
                if word.startswith("LD_LIBRARY_PATH=")]
    require(len(runtimes) == 1 and runtimes[0].is_absolute() and ":" not in str(runtimes[0]),
            f"{endpoint.label}: expected one absolute DPDK runtime library path")
    if endpoint.settings.app_mode:
        runtime = destination / "lib"
        expected_environment = unit_environment(endpoint, destination, runtime)
        require(sorted(environment) == sorted(f"{key}={value}" for key, value in expected_environment.items()),
                f"{endpoint.label}: installed Environment differs from the expected application profile")
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


def require_idle(commands, host):
    active = commands.run(host, "systemctl", "list-units", "--state=running,activating,deactivating",
                          "--no-pager", "--no-legend", "--plain", "ocs-mrc*.service", "ocs-roce*.service")
    require(not active.strip(), f"{host}: another lab service is running")


def app_digests(bundle):
    digests = {}
    for line in (bundle / "APP_SHA256SUMS").read_text().splitlines():
        match = re.fullmatch(r"([a-f0-9]{64})  (\S+)", line)
        require(match and match[2] in APP_FILES and match[2] not in digests,
                "unexpected application bundle digest entry")
        digests[match[2]] = match[1]
    require(set(digests) == set(APP_FILES), "incomplete application bundle manifest")
    return digests


def verify_deployed_file(commands, host, path):
    require(commands.run(host, "realpath", "-e", "--", path).strip() == str(path),
            f"{host}: unexpected installed path for {path}")
    owner, mode = commands.run(host, "stat", "-c", "%u %a", "--", path).split()
    require(owner == "0" and not (int(mode, 8) & 0o022),
            f"{host}: {path} must be writable only by root")


def preflight(commands, profiles, destination, digests):
    settings = [ALL_PROFILES[name] for name in profiles]
    aggregate = settings[0].aggregate
    app_mode = settings[0].app_mode
    native_mr = any(profile.native_mr for profile in settings)
    links = {name: {} for name in profiles} if aggregate else {}
    for role, (host, _, _, _) in PORTS.items():
        require_idle(commands, host)
        if aggregate:
            for name in profiles:
                endpoint = Endpoint(role, name)
                links[name][role] = {
                    **verify_link(commands, endpoint.link), "cpu_ids": list(endpoint.settings.cpus),
                }
        else:
            links[role] = verify_link(commands, PORTS[role])

        if native_mr:
            commands.run(host, "test", "-c", "/dev/roce_mr")
            description = commands.run(host, "cat", "/sys/class/infiniband/roce_dpdk0/node_desc")
            require(description.strip() == "RoCEv2 DPDK", f"{host}: provider device is missing")
        verify_deployed_file(commands, host, destination)
        if app_mode:
            for directory in ("lib", "provider"):
                verify_deployed_file(commands, host, destination / directory)
        for file, digest in digests.items():
            path = destination / file
            verify_deployed_file(commands, host, path)
            actual = commands.run(host, "sha256sum", "--", path).split()[0]
            require(actual == digest, f"{host}: installed file {file} does not match the expected SHA-256")
        for profile in profiles:
            verify_unit(commands, Endpoint(role, profile), destination)
    return links


def verify_loopback_listener(commands, host, port):
    sockets = commands.run(host, "ss", "-H", "-ltn", "sport", "=", f":{port}")
    addresses = [line.split()[3] for line in sockets.splitlines()]
    require(addresses == [f"{LOOPBACK}:{port}"],
            f"{host}: expected one loopback listener on port {port}, found {addresses}")


@contextmanager
def control_tunnel(commands, socket_dir, profiles):
    host = PORTS["rx"][0]
    control = Path(socket_dir) / "bandwidth-forward"
    ssh = ["ssh", "-S", str(control), "-o", "BatchMode=yes"]
    argv = [
        *ssh, "-fNT", "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=5",
        "-o", "ServerAliveCountMax=2", "-o", "ControlMaster=yes",
        "-o", "ControlPersist=no", "-o", "ExitOnForwardFailure=yes",
    ]
    for profile in profiles:
        argv += ["-L", f"{LOOPBACK}:{profile.port}:{LOOPBACK}:{profile.port}"]
    try:
        result = subprocess.run([*argv, host], capture_output=True, text=True, timeout=15,
                                stdin=subprocess.DEVNULL)
        require(result.returncode == 0, f"{host}: could not open benchmark tunnel: {result.stderr.strip()}")
        for profile in profiles:
            verify_loopback_listener(commands, PORTS["tx"][0], profile.port)
        yield
    except subprocess.TimeoutExpired as error:
        raise CycleError(f"{host}: timed out opening benchmark tunnel") from error
    finally:
        if control.exists():
            failed = sys.exc_info()[0] is not None
            try:
                result = subprocess.run([*ssh, "-O", "exit", host], capture_output=True,
                                        text=True, timeout=5, stdin=subprocess.DEVNULL)
                require(result.returncode == 0 or not control.exists(),
                        f"{host}: benchmark tunnel cleanup failed: {result.stderr.strip()}")
            except (CycleError, OSError, subprocess.TimeoutExpired) as error:
                if not failed:
                    raise CycleError(f"{host}: could not close benchmark tunnel: {error}") from error
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


def app_completion_marker(endpoint):
    outcome = "completed" if endpoint.role == "tx" else "verified"
    return f"Application {outcome} {endpoint.settings.payload} bytes"


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
            app_finished = not endpoint.settings.app_mode or app_completion_marker(endpoint) in messages
            if app_finished and any(line.startswith("BANDWIDTH ") for line in messages):
                return messages
        time.sleep(0.2)
    phase = "receiver readiness" if ready else "completion and verification"
    raise CycleError(f"{endpoint.label}: timed out waiting for {phase} ({invocation})")


def app_setup_timings(endpoint, messages):
    timings = {}
    for stage, tag in (("attach", "APP_ATTACH"), ("dma", "APP_DMA_SETUP"), ("prefault", "APP_PREFAULT")):
        lines = [message for message in messages if message.startswith(f"{tag} ")]
        require(len(lines) <= 1, f"{endpoint.label}: repeated {tag} result")
        if lines:
            fields = dict(field.split("=", 1) for field in lines[0].split()[1:])
            require(fields.get("bytes") == str(endpoint.settings.payload) and fields.get("elapsed_ns", "").isdigit(),
                    f"{endpoint.label}: invalid {tag} result")
            timings[stage] = {key: int(value) for key, value in fields.items()}
    require(("prefault" in timings) == endpoint.settings.prefault_app,
            f"{endpoint.label}: application prefault result differs from the profile")
    if endpoint.settings.prefault_app:
        require(timings["prefault"].get("pages", 0) > 0 and "attach" in timings,
                f"{endpoint.label}: incomplete application prefault timing")
    return timings


def app_transfer_timing(endpoint, messages):
    lines = [message for message in messages if message.startswith("APP_TRANSFER ")]
    require(len(lines) <= 1, f"{endpoint.label}: repeated application transfer timing")
    if not lines:
        return None  # Bundles staged before application timing are still usable.
    match = re.fullmatch(rf"APP_TRANSFER role={endpoint.role} payload_bytes={endpoint.settings.payload} "
                         r"start_ns=([1-9][0-9]*) elapsed_ns=([1-9][0-9]*) process_cpu_ns=([0-9]+)", lines[0])
    require(match, f"{endpoint.label}: invalid application transfer timing")
    return dict(zip(("start_ns", "elapsed_ns", "process_cpu_ns"), map(int, match.groups())))


def parse_result(endpoint, messages):
    lines = [line for line in messages if line.startswith("BANDWIDTH ")]
    require(len(lines) == 1, f"{endpoint.label}: expected one bandwidth result")
    fields = dict(field.split("=", 1) for field in lines[0].split()[1:])
    profile = endpoint.settings
    payload = profile.payload
    expected = {
        "role": endpoint.role, "payload_bytes": str(payload), "writes": str(profile.writes),
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
    require(math.isclose(rate, payload * 8 / elapsed, rel_tol=0, abs_tol=0.000501)
            and math.isclose(cores, cpu / elapsed, rel_tol=0, abs_tol=0.000501),
            f"{endpoint.label}: inconsistent bandwidth or CPU counters")
    if profile.aggregate:
        require(int(fields.get("start_ns", "0")) > 0, f"{endpoint.label}: missing monotonic start")
    fault_counters = ("poll_minor_faults", "poll_major_faults")
    if profile.prefault_app or any(key in fields for key in fault_counters):
        require(all(fields.get(key, "").isdigit() for key in fault_counters),
                f"{endpoint.label}: incomplete polling fault counts")
        fields.update((key, int(fields[key])) for key in fault_counters)

    if endpoint.role == "tx":
        pattern = rf"WRITE complete: {payload} bytes, {profile.packets} packets, ([0-9]+) retries, peer verified"
        matches = [re.fullmatch(pattern, message) for message in messages]
        retries = [int(match[1]) for match in matches if match]
        require(len(retries) == 1, f"{endpoint.label}: missing verified sender completion or retry count")
        fields["retries"] = str(retries[0])
    else:
        marker = f"WRITE verified: {payload} bytes, {profile.packets} packets"
        require(messages.count(marker) == 1, f"{endpoint.label}: missing byte verification")
    if profile.app_mode:
        require(messages.count(app_completion_marker(endpoint)) == 1,
                f"{endpoint.label}: missing application verification")
        processes = [match for message in messages if
                     (match := re.fullmatch(r"APP_PROCESSES worker_pid=([1-9][0-9]*) app_pid=([1-9][0-9]*)", message))]
        require(len(processes) == 1 and processes[0][1] != processes[0][2],
                f"{endpoint.label}: missing separate worker and application process IDs")
        fields.update(worker_pid=int(processes[0][1]), app_pid=int(processes[0][2]))
        timings = app_setup_timings(endpoint, messages)
        if timings:
            fields["memory_setup"] = timings
        transfer = app_transfer_timing(endpoint, messages)
        if transfer is not None:
            fields["application_transfer"] = transfer
        if endpoint.role == "tx":
            suffix = " via libibverbs" if profile.native_mr else ""
            completion = f"Application observed {profile.writes} WRITE completions per QP{suffix}"
            require(messages.count(completion) == 1, f"{endpoint.label}: missing application WRITE completions")
            if profile.scatter_gather:
                kind = "DMA-BUF" if profile.native_mr else "DMA"
                require(messages.count(f"Application {kind} region mapped for scatter/gather TX ({payload} bytes)") == 1,
                        f"{endpoint.label}: missing {kind} registration for scatter/gather")
    counters = [line for line in messages if line.startswith("PORT_STATS ")]
    if counters:
        require(len(counters) == 1, f"{endpoint.label}: expected one port counter result")
        fields["port_stats"] = {name: int(value) for name, value in
                                (field.split("=", 1) for field in counters[0].split()[1:])}
    return fields


def run_endpoints(commands, profiles, output, sample):
    receivers = [Endpoint("rx", name) for name in profiles]
    senders = [Endpoint("tx", name) for name in profiles]
    settings = senders[0].settings
    aggregate = settings.aggregate
    invocations = {}
    try:
        for endpoint in receivers:
            invocations[endpoint] = start(commands, endpoint)
        ready_deadline = time.monotonic() + (60 if settings.app_mode else 20)
        for endpoint in receivers:
            wait_endpoint(commands, endpoint, invocations[endpoint], ready_deadline, ready=True)
            if endpoint.settings.uses_tunnel:
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
        deadline = time.monotonic() + (90 if settings.app_mode else 60)
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


def set_link_mtu(commands, link, mtu):
    commands.run(link["host"], "sudo", "-n", "/usr/sbin/ip", "link", "set", "dev",
                 link["interface"], "mtu", mtu)


@contextmanager
def temporary_mtu(commands, links, mtu):
    changed = []
    try:
        for link in links.values():
            host, interface = link["host"], link["interface"]
            path = f"/sys/class/net/{interface}/mtu"
            previous = int(commands.run(host, "cat", path))
            if previous != mtu:
                commands.run(host, "sudo", "-n", "-l", "/usr/sbin/ip", "link", "set",
                             "dev", interface, "mtu", previous)
                changed.append((link, path, previous))
                set_link_mtu(commands, link, mtu)
            require(int(commands.run(host, "cat", path)) == mtu,
                    f"{host} {interface}: expected Ethernet MTU {mtu}")
        yield
    finally:
        failed = sys.exc_info()[0] is not None
        errors = []
        for link, path, previous in reversed(changed):
            try:
                if int(commands.run(link["host"], "cat", path)) == mtu:
                    set_link_mtu(commands, link, previous)
                    require(int(commands.run(link["host"], "cat", path)) == previous,
                            f"{link['host']} {link['interface']}: MTU restore failed")
            except (CycleError, OSError, ValueError) as error:
                errors.append(str(error))
        if errors:
            if not failed:
                raise CycleError("\n".join(errors))
            print("\n".join(errors), file=sys.stderr)


def run_profiles(commands, profiles, output, samples, report, *, keep_going=False):
    failures = []
    for profile in profiles:
        trials = []
        config = ALL_PROFILES[profile]
        profile_result = {"trials": trials}
        if config.ethernet_mtu is not None:
            profile_result.update(ethernet_mtu=config.ethernet_mtu, pmtu=config.pmtu)
        if config.app_mode:
            profile_result.update(app_mode=config.app_mode, payload_bytes=config.payload,
                                  app_post_batch=min(APP_POST_BATCH, config.writes), prefault_app=config.prefault_app)
        report["profiles"][profile] = profile_result
        for sample in range(1, samples + 1):
            try:
                results, _ = run_endpoints(commands, (profile,), output, sample)
            except CycleError as error:
                profile_result["failure"] = {"sample": sample, "error": str(error)}
                save_report(output, report)
                if not keep_going:
                    raise
                for host, _, _, _ in PORTS.values():
                    require_idle(commands, host)
                failures.append(profile)
                print(f"{profile} sample {sample} failed\n{error}", file=sys.stderr, flush=True)
                break
            trials.append(results[profile])
            median = {role: statistics.median(float(trial[role]["payload_gbps"]) for trial in trials)
                      for role in PORTS}
            profile_result["median_payload_gbps"] = median
            save_report(output, report)
        else:
            print(f"{profile} median verified payload TX {median['tx']:.3f} RX {median['rx']:.3f} Gbit/s",
                  flush=True)
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", required=True, type=Path, help="pinned installed snapshot directory")
    parser.add_argument("--sha256", help="expected SHA-256 of the installed standalone roce_write binary")
    parser.add_argument("--app-bundle", type=Path, help="staged application bundle containing APP_SHA256SUMS")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--aggregate", action="store_true",
                           help="run four-worker 16-QP transfers concurrently on both 100 Gbit/s rails")
    selection.add_argument("--burst-style-bench", action="store_true",
                           help="compare the same WRITE profiles at Ethernet MTUs 1500 and 4200")
    selection.add_argument("--app-bench", action="append", choices=APP_MODES,
                           help="repeat to select separate-process application profiles at both MTUs")
    fault_modes = parser.add_mutually_exclusive_group()
    fault_modes.add_argument("--compare-prefault", action="store_true",
                             help="include matching application profiles that prefault the worker's memory alias")
    fault_modes.add_argument("--prefault-only", action="store_true",
                             help="run the prefaulting application profiles alone")
    parser.add_argument("--profile", action="append", choices=APP_SHAPES,
                        help="repeat to select profiles; smoke is an 8 MiB application check")
    parser.add_argument("--mtu", type=int, choices=(1500, 4200),
                        help="limit a paired-MTU benchmark to this Ethernet MTU")
    parser.add_argument("--samples", type=int, default=3, help="trials per profile (default 3)")
    parser.add_argument("--output", type=Path, help="new directory for logs and results")
    args = parser.parse_args()
    root = APP_DESTINATION_ROOT if args.app_bench else DESTINATION_ROOT
    if args.destination.parent != root or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", args.destination.name):
        parser.error(f"--destination must name a snapshot directly under {root}")
    if args.app_bench:
        if args.sha256 or not args.app_bundle:
            parser.error("--app-bench requires --app-bundle in place of --sha256")
        digests = app_digests(args.app_bundle)
    else:
        if not args.sha256 or not re.fullmatch(r"[a-f0-9]{64}", args.sha256.lower()) or args.app_bundle:
            parser.error("standalone runs require --sha256 with 64 hexadecimal digits")
        digests = {"roce_write": args.sha256.lower()}
    if args.samples <= 0:
        parser.error("--samples must be positive")
    if args.aggregate and args.profile:
        parser.error("--aggregate cannot be combined with --profile")
    paired_mtu = args.burst_style_bench or args.app_bench
    if args.mtu is not None and not paired_mtu:
        parser.error("--mtu requires --burst-style-bench or --app-bench")
    selected = tuple(dict.fromkeys(args.profile or PROFILES))
    if "smoke" in selected and not args.app_bench:
        parser.error("--profile smoke requires --app-bench")
    if args.compare_prefault and not args.app_bench:
        parser.error("--compare-prefault requires --app-bench")
    if args.prefault_only and not args.app_bench:
        parser.error("--prefault-only requires --app-bench")
    mtus = (args.mtu,) if args.mtu is not None else (1500, 4200)
    if args.aggregate:
        profiles = tuple(AGGREGATE_PROFILES)
    elif args.app_bench:
        variants = ("-prefault",) if args.prefault_only else ("",)
        if args.compare_prefault:
            variants += ("-prefault",)
        profiles = tuple(f"app-{mode}-mtu-{mtu}-{name}{suffix}" for mtu in mtus
                         for mode in dict.fromkeys(args.app_bench) for name in selected for suffix in variants)
    elif args.burst_style_bench:
        profiles = tuple(f"mtu-{mtu}-{name}" for mtu in mtus for name in selected)
    else:
        profiles = selected

    require(socket.gethostname().split(".")[0] == "spark-7e64", "run the benchmark from Spark-04")
    with tempfile.TemporaryDirectory(prefix="roce-bw-control-") as sockets:
        commands = Commands(sockets)
        links = preflight(commands, profiles, args.destination, digests)
        output = args.output or Path(tempfile.mkdtemp(prefix="roce-bw-results-"))
        if args.output:
            output.mkdir()
        print(f"Benchmark output {output}", flush=True)
        report = {"sha256": digests["roce_write"], "destination": str(args.destination), "links": links}
        if args.app_bench:
            report["files_sha256"] = digests
        if args.aggregate:
            trials = []
            report["aggregate"] = {"trials": trials}
            with control_tunnel(commands, sockets, AGGREGATE_PROFILES.values()):
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
        if paired_mtu:
            failures = []
            with control_tunnel(commands, sockets, [ALL_PROFILES[name] for name in profiles]):
                for mtu in mtus:
                    batch = [name for name in profiles if ALL_PROFILES[name].ethernet_mtu == mtu]
                    with temporary_mtu(commands, links, mtu):
                        failures.extend(run_profiles(commands, batch, output, args.samples, report,
                                                     keep_going=True))
            require(not failures, f"Failed profiles: {', '.join(failures)}")
        else:
            run_profiles(commands, profiles, output, args.samples, report)


if __name__ == "__main__":
    try:
        main()
    except (CycleError, OSError, ValueError, KeyError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
