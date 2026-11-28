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

from spark_services import ACTIVE_STATES, Commands, CycleError, require, start, stop_owned


WRITE_BYTES = 1048576
PAYLOAD = 16 * (1 << 30)
PMTU = 1024
TX_BURST = 16
SEND_WINDOW = 256
CONTROL_IP = "10.61.0.13"
DESTINATION_ROOT = Path("/opt/ocs-roce-bw")
APP_DESTINATION_ROOT = Path("/opt/ocs-roce-app-bw")
APP_FILES = ("roce_write", "roce_app_write", "run_app", "provider/libroce_dpdk-rdmav34.so")
APP_MLX5_FILES = ("lib/librte_common_mlx5.so.24", "lib/librte_net_mlx5.so.24")
DMA_PROOF_MARKERS = {
    APP_MLX5_FILES[0]: (b"MLX5_DMABUF_MAP ",),
    APP_MLX5_FILES[1]: (b"MLX5_DMABUF_DSEG ", b"MLX5_DMABUF_CQE ", b"MLX5_DMABUF_CQ_ERROR "),
}
DMA_PROOF_SUFFIX = "-dmabuf-proof"
APP_AGGREGATE_SUFFIX = "-aggregate"
APP_TWO_QSFP_SUFFIX = "-two-qsfp"
APP_TWO_QSFP_POOL = "ocs-roce-bw-two-qsfp-pool.service"
APP_TWO_QSFP_HELPER = "manage_two_qsfp_pool"
APP_TWO_QSFP_HUGE_DIR = "/dev/hugepages/ocs-roce-two-qsfp"
HUGEPAGE_COUNT = "/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
APP_HUGEPAGE_HELPER = "manage_hugepages"
APP_HUGEPAGE_DIR = "/dev/hugepages/ocs-roce-app-bw"
APP_HUGEPAGES_RELEASED = "DPDK_HUGEPAGES restored=0 page_kib=2048"
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
SECOND_QSFP = {
    "tx": ("spark-04", "0002:01:00.1", "enP2p1s0f1np1", "10.61.0.17"),
    "rx": ("spark-01", "0002:01:00.0", "enP2p1s0f0np0", "10.61.0.18"),
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
    hugepages: bool = False
    write_bytes: int = WRITE_BYTES
    post_batch: int = APP_POST_BATCH
    dma_proof: bool = False

    @property
    def tx_burst(self):
        return 1 if self.dma_proof else TX_BURST

    @property
    def writes(self):
        return self.payload // (self.qps * self.write_bytes)

    @property
    def packets(self):
        return self.qps * self.writes * ((self.write_bytes + self.pmtu - 1) // self.pmtu)

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
            return Path("/run/ocs-roce-app-bw" if self.rail == 1 else "/run/ocs-roce-app-bw-2")
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
APP_AGGREGATE_PROFILES = {
    f"app-aggregate-{profile.rail}": replace(
        profile, port=29024 + profile.rail, ethernet_mtu=1500, app_mode="memfd-gather", prefault_app=True,
    )
    for profile in AGGREGATE_PROFILES.values()
}
APP_TWO_QSFP_PROFILES = {
    f"app-two-qsfp-mtu-{mtu}-{profile.rail}": replace(
        profile, port=29030 + 2 * index + profile.rail, pmtu=pmtu,
        ethernet_mtu=mtu, hugepages=mtu == 4200,
    )
    for index, (mtu, pmtu) in enumerate(((1500, 1024), (4200, 4096)))
    for profile in APP_AGGREGATE_PROFILES.values()
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
HUGEPAGE_PROFILES = {
    f"{name}-hugepages": replace(profile, port=profile.port + 2 * len(APP_PROFILES), hugepages=True)
    for name, profile in (APP_PROFILES | PREFAULT_PROFILES).items() if profile.ethernet_mtu == 4200
}
SHORT_WRITE_PROFILES = {
    f"app-mr-gather-mtu-{mtu}-short": Profile(
        port, 1, 1, pmtu=pmtu, ethernet_mtu=mtu, payload=payload, app_mode="mr-gather",
        write_bytes=pmtu, post_batch=1,
    )
    for mtu, pmtu, port, payload in ((1500, 1024, 29020, 4096), (4200, 4096, 29021, 8192))
}
DMA_PROOF_PROFILES = {
    f"{name.removesuffix('-short')}-dma-proof": replace(profile, port=profile.port + 2, dma_proof=True)
    for name, profile in SHORT_WRITE_PROFILES.items()
}
ALL_PROFILES = (PROFILES | AGGREGATE_PROFILES | BURST_STYLE_PROFILES | APP_PROFILES |
                PREFAULT_PROFILES | HUGEPAGE_PROFILES | SHORT_WRITE_PROFILES | DMA_PROOF_PROFILES |
                APP_AGGREGATE_PROFILES | APP_TWO_QSFP_PROFILES)


@dataclass(frozen=True)
class Endpoint:
    role: str
    profile: str

    @property
    def settings(self):
        return ALL_PROFILES[self.profile]

    @property
    def link(self):
        if self.settings.rail == 1:
            return PORTS[self.role]
        return (SECOND_QSFP if self.profile in APP_TWO_QSFP_PROFILES else SECOND_RAIL)[self.role]

    @property
    def shared_hugepages(self):
        return self.profile in APP_TWO_QSFP_PROFILES and self.settings.hugepages

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
    huge_directory = (f"{APP_TWO_QSFP_HUGE_DIR}/{profile.rail}"
                      if endpoint.shared_hugepages else APP_HUGEPAGE_DIR)
    backing = ([f"--huge-dir={huge_directory}", "--socket-mem=128", "--huge-unlink=always"]
               if profile.hugepages else ["--no-huge"])
    argv = [
        "/usr/bin/flock", "--exclusive", "--nonblock", str(lock),
        str(destination / ("run_app" if profile.app_mode else "roce_write")),
        "-l", cpus, "--iova-mode=va", *backing,
        "--no-telemetry", f"--file-prefix=ocs-roce-bw-{endpoint.profile}",
        "-d", str(runtime / "librte_mempool_ring.so.24"),
        "-d", str(runtime / "librte_net_mlx5.so.24"), "-a", device,
        "--", endpoint.role, str(profile.write_bytes), control_ip, str(profile.port), data_ip, str(profile.pmtu),
        pci, "--report-bw", "--tx-burst", str(profile.tx_burst), "--send-window", str(SEND_WINDOW),
        "--writes", str(profile.writes), "--qps", str(profile.qps), "--workers", str(profile.workers),
    ]
    if profile.aggregate and endpoint.role == "tx":
        argv += ["--start-gate", str(GATE_PORT)]
    if profile.native_mr:
        argv += ["--mr-socket", str(profile.runtime_dir / "registration.sock")]
    if profile.prefault_app:
        argv.append("--prefault-app")
    if profile.app_mode and endpoint.role == "tx":
        argv += ["--app-post-batch", str(profile.post_batch)]
        if profile.native_mr:
            argv.append("--verbs-queues")
        if profile.scatter_gather:
            argv.append("--scatter-gather")
    return argv


def unit_environment(endpoint, destination, runtime):
    profile = endpoint.settings
    environment = {"LD_LIBRARY_PATH": str(runtime), "XDG_RUNTIME_DIR": str(profile.runtime_dir)}
    if profile.app_mode == "mr-copy" and endpoint.role == "tx":
        environment["ROCE_DPDK_TX_MODE"] = "copy"
    if profile.native_mr:
        environment.update(RDMAV_DRIVERS=str(destination / "provider/libroce_dpdk"),
                           ROCE_DPDK_MR_SOCKET=str(profile.runtime_dir / "registration.sock"),
                           ROCE_DPDK_MR_REQUIRE_NETLINK="1")
    return environment


def verify_unit(commands, endpoint, destination):
    props = commands.properties(
        endpoint, "LoadState", "ActiveState", "WorkingDirectory", "ExecStart",
        "Environment", "UnsetEnvironment", "ExecStartPre", "ExecStopPost", "BindsTo", "After",
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
    if endpoint.shared_hugepages:
        require(all(APP_TWO_QSFP_POOL in props.get(prop, "").split() for prop in ("BindsTo", "After")),
                f"{endpoint.label}: missing the shared hugepage service dependency")
    elif endpoint.settings.hugepages:
        for prop, action in (("ExecStartPre", "prepare"), ("ExecStopPost", "cleanup")):
            hook = re.findall(r"argv\[\]=(.*?) ;", props.get(prop, ""))
            expected_hook = ["/bin/bash", str(destination / APP_HUGEPAGE_HELPER), action]
            require(len(hook) == 1 and shlex.split(hook[0]) == expected_hook,
                    f"{endpoint.label}: invalid hugepage {prop}")
    verify_service_access(commands, endpoint)


def verify_service_access(commands, endpoint):
    for operation in ("start", "stop"):
        commands.run(endpoint.host, "sudo", "-n", "-l", "/usr/bin/systemctl", operation, endpoint.unit)
    # A personal journal can be readable even when the unit's records are hidden.
    commands.sudo_journals.add(endpoint)
    commands.journal(endpoint)


def verify_link(commands, link, *, require_data_ip=True):
    host, pci, interface, data_ip = link
    param = commands.run(host, "devlink", "dev", "param", "show", f"pci/{pci}", "name", "enable_roce")
    require("cmode driverinit value false" in param, f"{host} {pci}: native RoCE still enabled")
    addr = commands.run(host, "ip", "-br", "-4", "address", "show", "dev", interface).split()
    configured = "UP" in addr and f"{data_ip}/30" in addr
    sysfs = Path("/sys/class/net") / interface
    if require_data_ip:
        require(configured, f"{host} {interface}: link or address not ready")
    else:
        # The benchmark exchanges wire addresses over the management SSH tunnel.
        require(commands.run(host, "cat", sysfs / "carrier").strip() == "1",
                f"{host} {interface}: link has no carrier")
    device = Path(commands.run(host, "readlink", "-f", "--", sysfs / "device").strip()).name
    require(device == pci, f"{host} {interface}: expected PCI function {pci}, found {device}")
    return {
        "host": host, "device": pci, "interface": interface, "data_ip": data_ip,
        "ethernet_mtu": int(commands.run(host, "cat", sysfs / "mtu")),
        "link_mbps": int(commands.run(host, "cat", sysfs / "speed")),
        "kernel_ipv4_configured": configured,
    }


def require_idle(commands, host):
    active = commands.run(host, "systemctl", "list-units", "--state=running,activating,deactivating",
                          "--no-pager", "--no-legend", "--plain", "ocs-mrc*.service", "ocs-roce*.service")
    require(not active.strip(), f"{host}: another lab service is running")


def app_digests(bundle):
    digests = {}
    supported = (*APP_FILES, *APP_MLX5_FILES, APP_HUGEPAGE_HELPER, APP_TWO_QSFP_HELPER)
    for line in (bundle / "APP_SHA256SUMS").read_text().splitlines():
        match = re.fullmatch(r"([a-f0-9]{64})  (\S+)", line)
        require(match and match[2] in supported and match[2] not in digests,
                "unexpected application bundle digest entry")
        digests[match[2]] = match[1]
    require(set(APP_FILES) <= digests.keys(), "incomplete application bundle manifest")
    return digests


def verify_deployed_file(commands, host, path):
    require(commands.run(host, "realpath", "-e", "--", path).strip() == str(path),
            f"{host}: unexpected installed path for {path}")
    owner, mode = commands.run(host, "stat", "-c", "%u %a", "--", path).split()
    require(owner == "0" and not (int(mode, 8) & 0o022),
            f"{host}: {path} must be writable only by root")


def verify_release(commands, host, destination, digests, app_mode):
    verify_deployed_file(commands, host, destination)
    if app_mode:
        for directory in ("lib", "provider"):
            verify_deployed_file(commands, host, destination / directory)
    for file, digest in digests.items():
        path = destination / file
        verify_deployed_file(commands, host, path)
        actual = commands.run(host, "sha256sum", "--", path).split()[0]
        require(actual == digest, f"{host}: installed file {file} does not match the expected SHA-256")


def preflight(commands, profiles, destination, digests):
    settings = [ALL_PROFILES[name] for name in profiles]
    aggregate = settings[0].aggregate
    app_mode = settings[0].app_mode
    native_mr = any(profile.native_mr for profile in settings)
    if any(name in SHORT_WRITE_PROFILES or name in DMA_PROOF_PROFILES or name in APP_AGGREGATE_PROFILES
           for name in profiles):
        require(set(APP_MLX5_FILES) <= digests.keys(), "benchmark profile requires pinned mlx5 libraries")
    if any(profile.hugepages for profile in settings):
        require(APP_HUGEPAGE_HELPER in digests, "application bundle lacks the hugepage helper")
    links = {name: {} for name in profiles} if aggregate else {}
    for role, (host, _, _, _) in PORTS.items():
        require_idle(commands, host)
        if aggregate:
            for name in profiles:
                endpoint = Endpoint(role, name)
                link = verify_link(commands, endpoint.link)
                expected_mtu = endpoint.settings.ethernet_mtu
                require(expected_mtu is None or link["ethernet_mtu"] == expected_mtu,
                        f"{endpoint.label}: expected Ethernet MTU {expected_mtu}")
                links[name][role] = {**link, "cpu_ids": list(endpoint.settings.cpus)}
        else:
            links[role] = verify_link(commands, PORTS[role])

        if native_mr:
            commands.run(host, "test", "-c", "/dev/roce_mr")
            description = commands.run(host, "cat", "/sys/class/infiniband/roce_dpdk0/node_desc")
            require(description.strip() == "RoCEv2 DPDK", f"{host}: provider device is missing")
        verify_release(commands, host, destination, digests, app_mode)
        for profile in profiles:
            verify_unit(commands, Endpoint(role, profile), destination)
    return links


def verify_loopback_listener(commands, host, port):
    sockets = commands.run(host, "ss", "-H", "-ltn", "sport", "=", f":{port}")
    addresses = [line.split()[3] for line in sockets.splitlines()]
    require(addresses == [f"{LOOPBACK}:{port}"],
            f"{host}: expected one loopback listener on port {port}, found {addresses}")


@contextmanager
def control_tunnels(commands, socket_dir, profiles):
    ports_by_host = {}
    for name in profiles:
        receiver = Endpoint("rx", name)
        ports_by_host.setdefault(receiver.host, []).append(receiver.settings.port)
    with ExitStack() as stack:
        for host, ports in ports_by_host.items():
            stack.enter_context(control_tunnel(commands, socket_dir, host, ports))
        yield


@contextmanager
def control_tunnel(commands, socket_dir, host, ports):
    control = Path(socket_dir) / f"{host}.forward"
    ssh = ["ssh", "-S", str(control), "-o", "BatchMode=yes"]
    argv = [
        *ssh, "-fNT", "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=5",
        "-o", "ServerAliveCountMax=2", "-o", "ControlMaster=yes",
        "-o", "ControlPersist=no", "-o", "ExitOnForwardFailure=yes",
    ]
    for port in ports:
        argv += ["-L", f"{LOOPBACK}:{port}:{LOOPBACK}:{port}"]
    try:
        result = subprocess.run([*argv, host], capture_output=True, text=True, timeout=15,
                                stdin=subprocess.DEVNULL)
        require(result.returncode == 0, f"{host}: could not open benchmark tunnel: {result.stderr.strip()}")
        for port in ports:
            verify_loopback_listener(commands, PORTS["tx"][0], port)
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
            restored = (not endpoint.settings.hugepages or endpoint.shared_hugepages or
                        APP_HUGEPAGES_RELEASED in messages)
            if app_finished and restored and any(line.startswith("BANDWIDTH ") for line in messages):
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


def parse_dma_proof(endpoint, messages):
    profile = endpoint.settings
    maps = [(position, re.fullmatch(r"MLX5_DMABUF_MAP device=(\S+) address=(0x[0-9a-f]+) "
                                   r"bytes=([1-9][0-9]*) lkey=(0x[0-9a-f]{8}) offset=0", line))
            for position, line in enumerate(messages) if line.startswith("MLX5_DMABUF_MAP ")]
    require(len(maps) == 1 and maps[0][1], f"{endpoint.label}: expected one DMA-BUF registration record")
    map_position, registration = maps[0]
    device, address, length, lkey = registration.groups()
    base = int(address, 16)
    require(device == endpoint.link[1] and base and int(length) == profile.payload,
            f"{endpoint.label}: DMA-BUF registration differs from benchmark payload")

    descriptors, completions, offsets = {}, {}, set()
    for position, line in enumerate(messages):
        if line.startswith("MLX5_DMABUF_DSEG "):
            match = re.fullmatch(r"MLX5_DMABUF_DSEG port=([0-9]+) queue=([0-9]+) wqe=([0-9]+) "
                                 r"address=(0x[0-9a-f]+) bytes=([1-9][0-9]*) lkey=(0x[0-9a-f]{8})", line)
            require(match, f"{endpoint.label}: malformed mlx5 DMA payload descriptor")
            port, queue, wqe, pointer, length, key = match.groups()
            index = (int(port), int(queue), int(wqe))
            offset = int(pointer, 16) - base
            require(position > map_position and index[1] == 0 and index[2] <= 0xffff and
                    index not in descriptors and key == lkey and
                    0 <= offset <= profile.payload - profile.write_bytes and
                    offset % profile.write_bytes == 0 and int(length) == profile.write_bytes,
                    f"{endpoint.label}: TX payload descriptor does not match the DMA-BUF registration: {line}")
            descriptors[index] = position
            offsets.add(offset)
        elif line.startswith("MLX5_DMABUF_CQE "):
            match = re.fullmatch(r"MLX5_DMABUF_CQE port=([0-9]+) queue=([0-9]+) wqe=([0-9]+)", line)
            require(match, f"{endpoint.label}: malformed mlx5 TX completion")
            index = tuple(map(int, match.groups()))
            require(index not in completions, f"{endpoint.label}: duplicate mlx5 TX completion {index}")
            completions[index] = position

    require(offsets == set(range(0, profile.payload, profile.write_bytes)),
            f"{endpoint.label}: DMA-BUF descriptors do not cover every posted WRITE")
    require(all(completions.get(index, -1) > position for index, position in descriptors.items()),
            f"{endpoint.label}: missing successful mlx5 TX completion for a DMA-BUF payload WQE")
    return {"lkey": lkey, "registered_bytes": profile.payload, "verified_offsets": sorted(offsets),
            "completed_payload_wqes": len(descriptors)}


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
        expected.update(send_window=str(SEND_WINDOW), tx_burst=str(profile.tx_burst))
        if "tx_mode" in fields:
            expected["tx_mode"] = "scatter-gather" if profile.scatter_gather else "copy"
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
            if endpoint.profile in SHORT_WRITE_PROFILES or profile.dma_proof:
                require(messages.count(f"APP_SERIAL_COMPLETIONS writes_per_qp={profile.writes}") == 1,
                        f"{endpoint.label}: missing serial WRITE completions")
                require(messages.count(f"DPDK_TX_BURST_COMPLETION queues={profile.workers}") == 1,
                        f"{endpoint.label}: missing mlx5 burst completion setup")
                fields["serial_completions"] = profile.writes
            if profile.dma_proof:
                fields["dma_buf_proof"] = parse_dma_proof(endpoint, messages)
    if profile.dma_proof:
        require(not any(line.startswith("MLX5_DMABUF_CQ_ERROR ") for line in messages),
                f"{endpoint.label}: mlx5 reported a TX completion error during the DMA proof")
    counters = [line for line in messages if line.startswith("PORT_STATS ")]
    if counters:
        require(len(counters) == 1, f"{endpoint.label}: expected one port counter result")
        fields["port_stats"] = {name: int(value) for name, value in
                                (field.split("=", 1) for field in counters[0].split()[1:])}
    pools = [line for line in messages if line.startswith("DPDK_PACKET_POOL ")]
    if profile.hugepages or pools:
        require(len(pools) == 1, f"{endpoint.label}: expected one packet pool result")
        match = re.fullmatch(r"DPDK_PACKET_POOL hugepages=([01]) chunks=([1-9][0-9]*)", pools[0])
        require(match and bool(int(match[1])) == profile.hugepages,
                f"{endpoint.label}: packet pool backing differs from the profile")
        fields["packet_pool"] = {"hugepages": profile.hugepages, "chunks": int(match[2])}
        modes = [re.fullmatch(r"DPDK_RX_MODE queue=([0-9]+) mode=(.+)", line) for line in messages
                 if line.startswith("DPDK_RX_MODE ")]
        if modes:
            require(all(modes) and [int(mode[1]) for mode in modes] == list(range(profile.workers)),
                    f"{endpoint.label}: incomplete RX mode diagnostics")
            fields["rx_modes"] = [mode[2] for mode in modes]
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
                gate_deadline = time.monotonic() + (60 if settings.app_mode else GATE_TIMEOUT)
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
            if endpoint.settings.dma_proof:
                evidence = fields.get("dma_buf_proof", {"verified_bytes": endpoint.settings.payload})
                print(f"{endpoint.label} sample {sample} {invocation} {evidence}", flush=True)
            else:
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
                                  write_bytes=config.write_bytes, app_post_batch=min(config.post_batch, config.writes),
                                  prefault_app=config.prefault_app, hugepages=config.hugepages)
        if config.dma_proof:
            profile_result["dma_proof"] = True
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
            if not config.dma_proof:
                median = {role: statistics.median(float(trial[role]["payload_gbps"]) for trial in trials)
                          for role in PORTS}
                profile_result["median_payload_gbps"] = median
            save_report(output, report)
        else:
            if config.dma_proof:
                print(f"{profile} verified DMA-BUF payload descriptors and NIC completions in {samples} sample(s)",
                      flush=True)
            else:
                print(f"{profile} median verified payload TX {median['tx']:.3f} RX {median['rx']:.3f} Gbit/s",
                      flush=True)
    return failures


@dataclass(frozen=True)
class PoolService:
    host: str
    unit: str = APP_TWO_QSFP_POOL

    @property
    def label(self):
        return f"{self.host} {self.unit}"


def two_qsfp_links(commands, profiles, mtu):
    links = {}
    for name in profiles:
        links[name] = {}
        for role in ("tx", "rx"):
            endpoint = Endpoint(role, name)
            link = verify_link(commands, endpoint.link, require_data_ip=False)
            require(link["ethernet_mtu"] == mtu, f"{endpoint.label}: expected Ethernet MTU {mtu}")
            links[name][role] = {**link, "cpu_ids": list(endpoint.settings.cpus)}
    return links


def preflight_two_qsfp(commands, profiles, destination, digests):
    require(set(APP_MLX5_FILES) <= digests.keys() and APP_TWO_QSFP_HELPER in digests,
            "two-port application runs require the staged mlx5 libraries and pool helper")
    endpoints = [Endpoint(role, name) for name in profiles for role in ("tx", "rx")]
    jumbo = endpoints[0].shared_hugepages
    for host in dict.fromkeys(endpoint.host for endpoint in endpoints):
        require_idle(commands, host)
        verify_release(commands, host, destination, digests, True)
        if jumbo:
            pool = PoolService(host)
            state = commands.properties(pool, "LoadState", "ActiveState", "ExecStartPre", "ExecStopPost")
            require(state.get("LoadState") == "loaded" and state.get("ActiveState") in ("inactive", "failed"),
                    f"{pool.label}: expected an idle hugepage service")
            for property_name, action in (("ExecStartPre", "prepare"), ("ExecStopPost", "cleanup")):
                hook = re.findall(r"argv\[\]=(.*?) ;", state.get(property_name, ""))
                expected = ["/bin/bash", str(destination / APP_TWO_QSFP_HELPER), action]
                require(len(hook) == 1 and shlex.split(hook[0]) == expected,
                        f"{pool.label}: unexpected {property_name}")
            verify_service_access(commands, pool)
    for endpoint in endpoints:
        verify_unit(commands, endpoint, destination)
    return two_qsfp_links(commands, profiles, 1500)


def stop_pool(commands, pool, invocation):
    stop_owned(commands, pool, invocation)
    state = commands.properties(pool, "ActiveState", "Result")
    require(state.get("ActiveState") == "inactive" and state.get("Result") == "success" and
            int(commands.run(pool.host, "cat", HUGEPAGE_COUNT)) == 0,
            f"{pool.label}: temporary hugepage cleanup failed")


@contextmanager
def aggregate_links(commands, profiles, links):
    if not all(name in APP_TWO_QSFP_PROFILES for name in profiles) or not ALL_PROFILES[profiles[0]].hugepages:
        yield links
        return

    hosts = dict.fromkeys(link["host"] for pair in links.values() for link in pair.values())
    mtu = ALL_PROFILES[profiles[0]].ethernet_mtu
    with ExitStack() as stack:
        for host in hosts:
            pool = PoolService(host)
            invocation = start(commands, pool)
            stack.callback(stop_pool, commands, pool, invocation)
        yield two_qsfp_links(commands, profiles, mtu)
    two_qsfp_links(commands, profiles, 1500)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", required=True, type=Path, help="pinned installed snapshot directory")
    parser.add_argument("--sha256", help="expected SHA-256 of the installed standalone roce_write binary")
    parser.add_argument("--app-bundle", type=Path, help="staged application bundle containing APP_SHA256SUMS")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--aggregate", action="store_true",
                           help="run four-worker 16-QP transfers concurrently on both logical paths")
    selection.add_argument("--app-aggregate", action="store_true",
                           help="run synchronized prefaulted memfd scatter/gather on both paths at MTU 1500")
    selection.add_argument("--two-qsfp", action="store_true",
                           help="measure concurrent application WRITEs on two physical QSFP ports")
    selection.add_argument("--burst-style-bench", action="store_true",
                           help="compare the same WRITE profiles at Ethernet MTUs 1500 and 4200")
    selection.add_argument("--app-bench", action="append", choices=APP_MODES,
                           help="repeat to select separate-process application profiles at both MTUs")
    fault_modes = parser.add_mutually_exclusive_group()
    fault_modes.add_argument("--compare-prefault", action="store_true",
                             help="include matching application profiles that prefault the worker's memory alias")
    fault_modes.add_argument("--prefault-only", action="store_true",
                             help="run the prefaulting application profiles alone")
    backing_modes = parser.add_mutually_exclusive_group()
    backing_modes.add_argument("--compare-hugepages", action="store_true",
                               help="compare default and 2 MiB packet-pool backing at MTU 4200")
    backing_modes.add_argument("--hugepages-only", action="store_true",
                               help="run only the 2 MiB packet-pool profiles at MTU 4200")
    parser.add_argument("--profile", action="append", choices=(*APP_SHAPES, "short", "dma-proof"),
                        help="repeat to select profiles; dma-proof verifies the mlx5 payload descriptors and CQEs")
    parser.add_argument("--mtu", type=int, choices=(1500, 4200),
                        help="limit a paired-MTU benchmark to this Ethernet MTU")
    parser.add_argument("--samples", type=int, default=3, help="trials per profile (default 3)")
    parser.add_argument("--output", type=Path, help="new directory for logs and results")
    args = parser.parse_args()
    aggregate = args.aggregate or args.app_aggregate or args.two_qsfp
    application_run = args.app_bench is not None or args.app_aggregate or args.two_qsfp
    root = APP_DESTINATION_ROOT if application_run else DESTINATION_ROOT
    if args.destination.parent != root or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", args.destination.name):
        parser.error(f"--destination must name a snapshot directly under {root}")
    if application_run:
        if args.sha256 or not args.app_bundle:
            parser.error("application runs require --app-bundle in place of --sha256")
        digests = app_digests(args.app_bundle)
    else:
        if not args.sha256 or not re.fullmatch(r"[a-f0-9]{64}", args.sha256.lower()) or args.app_bundle:
            parser.error("standalone runs require --sha256 with 64 hexadecimal digits")
        digests = {"roce_write": args.sha256.lower()}
    if args.samples <= 0:
        parser.error("--samples must be positive")
    if aggregate and args.profile:
        parser.error("aggregate runs cannot be combined with --profile")
    paired_mtu = args.burst_style_bench or args.app_bench
    if args.mtu is not None and not (paired_mtu or args.two_qsfp):
        parser.error("--mtu requires --burst-style-bench, --app-bench, or --two-qsfp")
    selected = tuple(dict.fromkeys(args.profile or PROFILES))
    if "smoke" in selected and not args.app_bench:
        parser.error("--profile smoke requires --app-bench")
    hugepages = args.compare_hugepages or args.hugepages_only
    if "short" in selected and (set(args.app_bench or ()) != {"mr-gather"} or
                                args.compare_prefault or args.prefault_only or hugepages):
        parser.error("--profile short requires --app-bench mr-gather without memory backing comparisons")
    if "dma-proof" in selected and (selected != ("dma-proof",) or set(args.app_bench or ()) != {"mr-gather"} or
                                    args.compare_prefault or args.prefault_only or hugepages):
        parser.error("--profile dma-proof runs by itself with --app-bench mr-gather")
    if args.compare_prefault and not args.app_bench:
        parser.error("--compare-prefault requires --app-bench")
    if args.prefault_only and not args.app_bench:
        parser.error("--prefault-only requires --app-bench")
    if hugepages and (not args.app_bench or args.mtu not in (None, 4200)):
        parser.error("hugepage profiles require an application benchmark at MTU 4200")
    mtus = (args.mtu,) if args.mtu is not None else (1500, 4200)
    if hugepages:
        mtus = (4200,)
    if args.two_qsfp:
        mtu = args.mtu or 1500
        profiles = tuple(name for name, profile in APP_TWO_QSFP_PROFILES.items() if profile.ethernet_mtu == mtu)
    elif args.app_aggregate:
        profiles = tuple(APP_AGGREGATE_PROFILES)
    elif args.aggregate:
        profiles = tuple(AGGREGATE_PROFILES)
    elif args.app_bench:
        variants = ("-prefault",) if args.prefault_only else ("",)
        if args.compare_prefault:
            variants += ("-prefault",)
        profiles = tuple(f"app-{mode}-mtu-{mtu}-{name}{suffix}" for mtu in mtus
                         for mode in dict.fromkeys(args.app_bench) for name in selected for suffix in variants)
        if args.compare_hugepages:
            profiles = tuple(variant for name in profiles for variant in (name, f"{name}-hugepages"))
        if args.hugepages_only:
            profiles = tuple(f"{name}-hugepages" for name in profiles)
    elif args.burst_style_bench:
        profiles = tuple(f"mtu-{mtu}-{name}" for mtu in mtus for name in selected)
    else:
        profiles = selected

    require(socket.gethostname().split(".")[0] == "spark-7e64", "run the benchmark from Spark-04")
    with tempfile.TemporaryDirectory(prefix="roce-bw-control-") as sockets:
        commands = Commands(sockets)
        verify = preflight_two_qsfp if args.two_qsfp else preflight
        links = verify(commands, profiles, args.destination, digests)
        output = args.output or Path(tempfile.mkdtemp(prefix="roce-bw-results-"))
        if args.output:
            output.mkdir()
        print(f"Benchmark output {output}", flush=True)
        report = {"sha256": digests["roce_write"], "destination": str(args.destination), "links": links}
        if application_run:
            report["files_sha256"] = digests
        if aggregate:
            trials = []
            report["aggregate"] = {"trials": trials}
            with (control_tunnels(commands, sockets, profiles),
                  aggregate_links(commands, profiles, links) as active_links):
                report["links"] = active_links
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
            with control_tunnels(commands, sockets, profiles):
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
