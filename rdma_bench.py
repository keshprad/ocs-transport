#!/usr/bin/env python3
"""Coordinate native perftest benchmarks on two Linux hosts (standard library only)."""

from __future__ import annotations

import argparse
import csv
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import platform
import re
import resource
import select
import shlex
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import time
import uuid


PROTOCOL = 1
SIZES = (64, 16 * 1024, 2**20, 4 * 2**20, 16 * 2**20, 64 * 2**20, 256 * 2**20, 2**30)
MODES = ("unidirectional-write-bw", "bidirectional-write-bw", "write-latency")
MAX_FRAME = 8 * 2**20
SYSFS = Path("/sys")


class BenchError(Exception):
    """An actionable preflight, protocol, or benchmark failure."""


def log(message):
    print(message, flush=True)


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def read_text(path, default=None):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return default


def run_text(argv):
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=15,
                                env={**os.environ, "LC_ALL": "C"})
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise BenchError(f"Cannot run {shlex.join(argv)}: {exc}") from exc
    if result.returncode:
        raise BenchError(f"{shlex.join(argv)} failed: {result.stderr.strip() or result.stdout.strip()}")
    return result.stdout


def size_label(size):
    for unit, divisor in (("GiB", 2**30), ("MiB", 2**20), ("KiB", 2**10)):
        if size >= divisor and size % divisor == 0:
            return f"{size // divisor} {unit}"
    return f"{size} B"


def parse_size(value):
    match = re.fullmatch(r"\s*(\d+)\s*(B|KiB|MiB|GiB)?\s*", value, re.I)
    if not match:
        raise argparse.ArgumentTypeError("Use bytes or a binary suffix, e.g. 64, 16KiB, 1GiB")
    size = int(match[1]) * {"b": 1, "kib": 2**10, "mib": 2**20, "gib": 2**30}[(match[2] or "B").lower()]
    if not 1 <= size <= 2**30:
        raise argparse.ArgumentTypeError("Message sizes must be between 1 B and 1 GiB")
    return size


@dataclass(frozen=True)
class Config:
    modes: tuple = MODES
    sizes: tuple = SIZES
    repetitions: int = 3
    duration: int = 20
    perftest_port: int = 18515
    startup_timeout: float = 30.0
    test_timeout: float = 180.0

    @classmethod
    def from_dict(cls, data):
        try:
            config = cls(**{**data, "modes": tuple(data["modes"]), "sizes": tuple(data["sizes"])})
            config.validate()
            return config
        except (KeyError, TypeError, ValueError) as exc:
            raise BenchError(f"Invalid test configuration: {exc}") from exc

    def validate(self):
        if not self.modes or any(m not in MODES for m in self.modes) or len(set(self.modes)) != len(self.modes):
            raise BenchError("Select at least one distinct supported benchmark mode")
        if not self.sizes or len(set(self.sizes)) != len(self.sizes):
            raise BenchError("Provide at least one distinct message size")
        if any(type(s) is not int or not 1 <= s <= 2**30 for s in self.sizes):
            raise BenchError("Message sizes must be integers between 1 B and 1 GiB")
        if type(self.repetitions) is not int or not 1 <= self.repetitions <= 100:
            raise BenchError("Repetitions must be between 1 and 100")
        if type(self.duration) is not int or not 5 <= self.duration <= 3600:
            raise BenchError("Bandwidth duration must be between 5 and 3600 seconds")
        if type(self.perftest_port) is not int or not 1024 <= self.perftest_port <= 65535:
            raise BenchError("Perftest port must be between 1024 and 65535")
        if any(not isinstance(t, (int, float)) or not math.isfinite(t) or t <= 0
               for t in (self.startup_timeout, self.test_timeout)):
            raise BenchError("Timeouts must be positive and finite")
        if self.test_timeout < self.duration + 15:
            raise BenchError("Test timeout must exceed bandwidth duration by at least 15 seconds")


@dataclass
class Endpoint:
    hostname: str
    interface: str
    ip: str
    device: str
    rdma_port: int
    gid_index: int
    gid: str
    details: dict
    tools: dict


def discover_endpoint(args):
    if not re.fullmatch(r"[a-zA-Z0-9_.:-]+", args.interface):
        raise BenchError("Invalid interface name")
    net = SYSFS / "class/net" / args.interface
    if not net.exists():
        raise BenchError(f"Interface {args.interface} does not exist")
    if read_text(net / "operstate") != "up":
        raise BenchError(f"Interface {args.interface} is not up")
    addresses = json.loads(run_text(["ip", "-j", "-4", "address", "show", "dev", args.interface]))
    ips = [a["local"] for link in addresses for a in link.get("addr_info", []) if a["family"] == "inet"]
    if args.ip:
        if args.ip not in ips:
            raise BenchError(f"--ip {args.ip} is not assigned to {args.interface}")
        ip = args.ip
    elif len(ips) == 1:
        ip = ips[0]
    else:
        raise BenchError(f"Expected one IPv4 address on {args.interface}, found {ips}; select --ip")

    matches = []
    for device in sorted((SYSFS / "class/infiniband").glob("*")):
        if args.device and device.name != args.device:
            continue
        for port in sorted((device / "ports").glob("*")):
            if args.rdma_port and int(port.name) != args.rdma_port:
                continue
            for gid_file in sorted((port / "gids").glob("*")):
                index = int(gid_file.name)
                if args.gid_index is not None and index != args.gid_index:
                    continue
                gid = read_text(gid_file)
                if read_text(port / "gid_attrs/ndevs" / gid_file.name) != args.interface:
                    continue
                if read_text(port / "gid_attrs/types" / gid_file.name) != "RoCE v2":
                    continue
                try:
                    if str(ipaddress.IPv6Address(gid).ipv4_mapped) != ip:
                        continue
                except (ipaddress.AddressValueError, TypeError):
                    continue
                matches.append((device.name, int(port.name), index, gid))
    if len(matches) != 1:
        raise BenchError(f"Expected one IPv4 RoCEv2 GID for {args.interface}/{ip}, found {matches}. "
                         "Check IP/RDMA setup or select --device, --rdma-port, and --gid-index.")
    device, port_number, gid_index, gid = matches[0]
    port = SYSFS / "class/infiniband" / device / "ports" / str(port_number)
    state = read_text(port / "state", "")
    if "ACTIVE" not in state or read_text(port / "link_layer") != "Ethernet":
        raise BenchError(f"{device} port {port_number} is not an active Ethernet RDMA port")
    device_info = run_text(["ibv_devinfo", "-d", device, "-i", str(port_number)])
    mtu = re.search(r"active_mtu:\s*(\d+)", device_info)
    details = {
        "os": platform.platform(), "architecture": platform.machine(),
        "ethernet_mtu": read_text(net / "mtu"), "speed_mbps": read_text(net / "speed"),
        "rdma_active_mtu": int(mtu[1]) if mtu else None, "rdma_state": state,
        "ibv_devinfo": device_info,
        "firmware": read_text(port.parent.parent / "fw_ver"),
        "memlock_bytes": list(resource.getrlimit(resource.RLIMIT_MEMLOCK)),
        "nvidia_driver": read_text("/proc/driver/nvidia/version"),
        "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    }
    return Endpoint(socket.gethostname(), args.interface, ip, device, port_number, gid_index, gid, details, {})


def verify_route(endpoint, peer_ip):
    routes = json.loads(run_text(["ip", "-j", "-4", "route", "get", peer_ip, "from", endpoint.ip]))
    if len(routes) != 1 or routes[0].get("dev") != endpoint.interface:
        raise BenchError(f"Route to {peer_ip} from {endpoint.ip} does not use {endpoint.interface}: {routes}")


def probe_tools(endpoint, config):
    names = {"ib_write_lat" if m == "write-latency" else "ib_write_bw" for m in config.modes}
    common = ("--ib-dev", "--ib-port", "--gid-index", "--connection", "--inline_size",
              "--size", "--port", "--bind_source_ip", "--iters")
    for name in sorted(names):
        path = shutil.which(name)
        if not path:
            raise BenchError(f"{name} not found on PATH; install perftest before running this script")
        outputs = {}
        for option in ("--help", "--version"):
            try:
                result = subprocess.run([path, option], capture_output=True, text=True, timeout=15,
                                        env={**os.environ, "LC_ALL": "C"})
            except (OSError, subprocess.TimeoutExpired) as exc:
                raise BenchError(f"Cannot inspect {path}: {exc}") from exc
            # perftest releases may return nonzero for informational options.
            outputs[option] = result.stdout + result.stderr
        required = list(common)
        if name == "ib_write_bw":
            required += ["--duration", "--qp", "--tx-depth", "--report_gbits", "--noPeak", "--perform_warm_up"]
            if "bidirectional-write-bw" in config.modes:
                required += ["--bidirectional", "--report-both"]
        missing = [flag for flag in required if flag not in outputs["--help"]]
        version = re.search(r"Version:\s*(\S+)", outputs["--version"], re.I)
        if missing or not version:
            raise BenchError(f"Unsupported {path}: missing options {missing} or unreadable version")
        endpoint.tools[name] = {"path": path, "version": version[1], "help": outputs["--help"]}


def verify_peer(local, peer):
    for name, tool in local.tools.items():
        other = peer.tools.get(name, {})
        if tool["version"] != other.get("version"):
            raise BenchError(f"{name} version mismatch: local {tool['version']}, peer {other.get('version')}")
    if local.details["script_sha256"] != peer.details.get("script_sha256"):
        raise BenchError("Runner scripts differ; copy the same rdma_bench.py to both hosts")


@dataclass(frozen=True)
class Job:
    id: str
    mode: str
    size: int
    phase: str
    direction: str = "client_to_server"
    repetition: int = 0
    iterations: int | None = None

    @property
    def listener(self):
        return "client" if self.direction == "server_to_client" else "server"

    def validate(self, config):
        if not re.fullmatch(r"[a-zA-Z0-9_-]{1,120}", self.id):
            raise BenchError("Invalid job ID")
        if self.mode not in config.modes or self.size not in config.sizes:
            raise BenchError("Job is outside negotiated test matrix")
        if self.phase not in ("smoke", "calibration", "warmup", "measured"):
            raise BenchError("Invalid job phase")
        directions = ("client_to_server", "server_to_client") if self.mode == MODES[0] else (
            ("both",) if self.mode == MODES[1] else ("pingpong",))
        if self.direction not in directions:
            raise BenchError("Invalid job direction")
        if not 0 <= self.repetition <= config.repetitions:
            raise BenchError("Invalid repetition")
        if self.iterations is not None and (type(self.iterations) is not int or not 10 <= self.iterations <= 100_000_000):
            raise BenchError("Iteration count must be between 10 and 100,000,000")
        if self.mode == MODES[2] and self.iterations is None:
            raise BenchError("Latency jobs require an iteration count")


def build_command(endpoint, peer_ip, role, config, job):
    name = "ib_write_lat" if job.mode == MODES[2] else "ib_write_bw"
    args = [endpoint.tools[name]["path"], "-d", endpoint.device, "-i", str(endpoint.rdma_port),
            "-x", str(endpoint.gid_index), "-c", "RC", "-I", "0", "-p", str(config.perftest_port),
            "-s", str(job.size), f"--bind_source_ip={endpoint.ip}"]
    if name == "ib_write_bw":
        args += ["-q", "1", "-t", "128", "--report_gbits", "-N", "--perform_warm_up"]
        if job.mode == MODES[1]:
            args += ["-b", "--report-both"]
    if job.iterations is not None:
        args += ["-n", str(job.iterations)]
    else:
        args += ["-D", str(config.duration)]
    if role != job.listener:
        args.append(peer_ip)
    return args


def parse_output(text, job):
    """Parse full-verbosity tables; never confuse the bidi aggregate with a direction."""
    section, header = "aggregate", False
    rows = {}
    latency = job.mode == MODES[2]
    for line in text.splitlines():
        if "Local results:" in line:
            section, header = "local", False
        elif "Remote results:" in line:
            section, header = "remote", False
        if "#bytes" in line and "#iterations" in line:
            expected = "t_typical[usec]" if latency else "BW average[Gb/sec]"
            if expected not in line:
                raise BenchError(f"Unexpected perftest units/columns: {line.strip()}")
            header = True
            continue
        tokens = line.split()
        if not header or len(tokens) != (9 if latency else 5) or not tokens[0].isdigit():
            continue
        try:
            numbers = [float(t) for t in tokens]
            if not all(math.isfinite(n) for n in numbers):
                raise ValueError("nonfinite measurement")
            if int(tokens[0]) != job.size:
                raise BenchError(f"Perftest reported {tokens[0]} bytes, requested {job.size}; refusing substituted size")
            if int(tokens[1]) <= 0:
                raise ValueError("no measured iterations")
            if job.iterations is not None and int(tokens[1]) != job.iterations:
                raise BenchError(f"Perftest reported {tokens[1]} iterations, requested {job.iterations}")
            if section in rows:
                raise BenchError(f"Multiple perftest result rows in section {section}")
            if latency:
                names = ("min_us", "max_us", "typical_us", "average_us", "stdev_us", "p99_us", "p999_us")
                row = dict(zip(names, numbers[2:]))
                positive = row["average_us"]
            else:
                row = dict(zip(("peak_gbps", "average_gbps", "message_rate_mpps"), numbers[2:]))
                positive = row["average_gbps"]
            if positive <= 0 or any(n < 0 for n in numbers):
                raise ValueError("invalid measurement")
            rows[section] = {"size_bytes": int(tokens[0]), "iterations": int(tokens[1]), **row}
        except ValueError as exc:
            raise BenchError(f"Malformed perftest result: {line.strip()}: {exc}") from exc
    required = {"aggregate", "local", "remote"} if job.mode == MODES[1] else {"aggregate"}
    if set(rows) != required:
        raise BenchError(f"Missing/unsupported perftest output: expected {sorted(required)}, found {sorted(rows)}")
    if job.mode == MODES[1]:
        total = rows["local"]["average_gbps"] + rows["remote"]["average_gbps"]
        if not math.isclose(total, rows["aggregate"]["average_gbps"], rel_tol=0.002, abs_tol=0.03):
            raise BenchError("Bidirectional aggregate does not match local + remote results")
    return rows


def calibrated_iterations(parsed):
    # Perftest WRITE latency is half RTT. Derive loop duration from its native
    # average, never from subprocess startup/registration/teardown time.
    average = parsed["aggregate"]["average_us"]
    count = max(100, math.ceil(15_000_000 / (2 * average)))
    if count > 100_000_000:
        raise BenchError("Calibration requires more than 100,000,000 exchanges; inspect latency output")
    return count


def snapshot(endpoint):
    values, unavailable = {}, []
    roots = {
        "net": SYSFS / "class/net" / endpoint.interface / "statistics",
        "rdma": SYSFS / "class/infiniband" / endpoint.device / "ports" / str(endpoint.rdma_port) / "counters",
        "rdma_hw": SYSFS / "class/infiniband" / endpoint.device / "ports" / str(endpoint.rdma_port) / "hw_counters",
    }
    for prefix, root in roots.items():
        if not root.exists():
            unavailable.append(str(root))
            continue
        for path in sorted(root.iterdir()):
            value = read_text(path)
            if value is not None and value.isdigit():
                values[f"{prefix}/{path.name}"] = int(value)
            elif value is None:
                unavailable.append(str(path))
    return {"time": utc_now(), "values": values, "unavailable": unavailable}


def counter_changes(before, after):
    deltas, alerts = {}, []
    suspicious = re.compile(r"error|err$|discard|drop|retry|retrans|out_of_buffer|link_downed|link_error_recovery")
    for key in before["values"].keys() & after["values"].keys():
        delta = after["values"][key] - before["values"][key]
        deltas[key] = delta
        if delta < 0:
            alerts.append(f"{key} decreased (counter reset?)")
        elif delta > 0 and suspicious.search(key):
            alerts.append(f"{key} increased by {delta}")
    if before["unavailable"] or after["unavailable"]:
        alerts.append("Some counters were unavailable; see snapshots")
    return {"deltas": deltas, "alerts": alerts}


class Channel:
    """Bounded newline-delimited JSON, with monotonic local deadlines."""

    def __init__(self, sock):
        self.sock, self.buffer = sock, b""
        sock.settimeout(5)

    def send(self, kind, **data):
        payload = json.dumps({"type": kind, **data}, allow_nan=False).encode() + b"\n"
        if len(payload) > MAX_FRAME:
            raise BenchError("Control message exceeds size limit")
        try:
            self.sock.sendall(payload)
        except OSError as exc:
            raise BenchError(f"Control connection send failed: {exc}") from exc

    def poll(self, timeout=0.1):
        try:
            if b"\n" not in self.buffer:
                if not select.select([self.sock], [], [], timeout)[0]:
                    return None
                data = self.sock.recv(65536)
                if not data:
                    raise BenchError("Peer disconnected; partial results have been retained")
                self.buffer += data
                if len(self.buffer) > MAX_FRAME:
                    raise BenchError("Control message exceeds size limit")
                if b"\n" not in self.buffer:
                    return None
            raw, self.buffer = self.buffer.split(b"\n", 1)
            message = json.loads(raw)
            if not isinstance(message, dict) or "type" not in message:
                raise BenchError("Invalid control message")
            if message["type"] == "error":
                raise BenchError(f"Peer failed: {message.get('message', 'unknown error')}")
            return message
        except (OSError, ValueError) as exc:
            raise BenchError(f"Invalid/lost control connection: {exc}") from exc

    def receive(self, expected, timeout, tick=None):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if tick:
                tick()
            message = self.poll(min(0.1, max(0, deadline - time.monotonic())))
            if message:
                if message["type"] != expected:
                    raise BenchError(f"Expected {expected}, got {message['type']}")
                return message
        raise BenchError(f"Timed out waiting for peer message: {expected}")

    def close(self):
        self.sock.close()


def owns_listener(pid, port):
    """Inspect LISTEN state and socket ownership without connecting to perftest."""
    proc = Path("/proc") / str(pid)
    try:
        sockets = set()
        for fd in (proc / "fd").iterdir():
            try:
                target = fd.readlink().as_posix()
            except OSError:
                continue
            if target.startswith("socket:["):
                sockets.add(target[8:-1])
        for table in ("tcp", "tcp6"):
            for line in (proc / "net" / table).read_text().splitlines()[1:]:
                fields = line.split()
                if fields[3] == "0A" and int(fields[1].split(":")[1], 16) == port and fields[9] in sockets:
                    return True
    except FileNotFoundError:
        return False
    except OSError as exc:
        raise BenchError(f"Cannot inspect perftest listener in /proc: {exc}") from exc
    return False


class NativeProcess:
    def __init__(self, argv, directory, job):
        self.job, self.directory, self.argv = job, directory, argv
        self.started = time.monotonic()
        self.out_path = directory / f"{job.id}.stdout.log"
        self.err_path = directory / f"{job.id}.stderr.log"
        # Persist the command before spawning, so a filesystem failure cannot
        # leave an untracked child process running.
        (directory / f"{job.id}.command.json").write_text(json.dumps({"argv": argv, "job": asdict(job)}, indent=2) + "\n")
        self.out_file = self.out_path.open("wb")
        try:
            self.err_file = self.err_path.open("wb")
        except OSError:
            self.out_file.close()
            raise
        try:
            self.process = subprocess.Popen(argv, stdout=self.out_file, stderr=self.err_file,
                                            start_new_session=True, env={**os.environ, "LC_ALL": "C"})
        except OSError as exc:
            self.out_file.close()
            self.err_file.close()
            raise BenchError(f"Cannot start {shlex.join(argv)}: {exc}") from exc

    def check(self, require_running=False):
        code = self.process.poll()
        if code is not None and (code != 0 or require_running):
            detail = read_text(self.err_path, "")[-2000:]
            raise BenchError(f"{self.job.id}: native process exited {code}. {detail} See {self.out_path}")
        return code

    def wait_ready(self, channel, config):
        deadline = time.monotonic() + config.startup_timeout
        while time.monotonic() < deadline:
            self.check(require_running=True)
            if owns_listener(self.process.pid, config.perftest_port):
                return
            message = channel.poll(0.05)
            if message:
                raise BenchError(f"Unexpected message while awaiting listener: {message['type']}")
        raise BenchError(f"{self.job.id}: listener was not ready within {config.startup_timeout}s")

    def record(self):
        self.check()
        self.out_file.close()
        self.err_file.close()
        stdout, stderr = self.out_path.read_text(errors="replace"), self.err_path.read_text(errors="replace")
        if len(stdout.encode()) + len(stderr.encode()) > MAX_FRAME // 2:
            raise BenchError(f"{self.job.id}: excessive native output; see local logs")
        record = {"argv": self.argv, "returncode": self.process.returncode,
                  "wall_seconds_diagnostic_only": time.monotonic() - self.started,
                  "parsed": parse_output(stdout, self.job), "stdout": stdout, "stderr": stderr}
        (self.directory / f"{self.job.id}.result.json").write_text(json.dumps(record, indent=2) + "\n")
        return record

    def stop(self):
        if self.process.poll() is None:
            try:
                os.killpg(self.process.pid, signal.SIGTERM)
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=2)
            except ProcessLookupError:
                self.process.wait(timeout=2)
        self.out_file.close()
        self.err_file.close()


def wait_native(native, channel, timeout, peer_result=False):
    deadline = time.monotonic() + timeout
    remote = None
    while time.monotonic() < deadline:
        done = native.check() is not None
        if done and (not peer_result or remote is not None):
            return native.record(), remote
        message = channel.poll(0.1)
        if message:
            if not peer_result or remote is not None or message["type"] != "result" or message.get("id") != native.job.id:
                raise BenchError(f"Unexpected message during {native.job.id}: {message['type']}")
            remote = message["record"]
    raise BenchError(f"{native.job.id}: benchmark/peer exceeded execution timeout of {timeout}s")


CSV_FIELDS = ("id", "mode", "size_bytes", "direction", "repetition", "iterations",
              "client_to_server_gbps", "server_to_client_gbps", "aggregate_gbps",
              "latency_typical_us", "latency_average_us", "latency_min_us", "latency_max_us",
              "latency_stdev_us", "latency_p99_us", "latency_p999_us")


def measured_row(job, client_record, server_record):
    parsed = client_record["parsed"]
    value = parsed["aggregate"]
    row = {"id": job.id, "mode": job.mode, "size_bytes": job.size, "direction": job.direction,
           "repetition": job.repetition, "iterations": value["iterations"]}
    if job.mode == MODES[2]:
        for key in ("typical", "average", "min", "max", "stdev", "p99", "p999"):
            row[f"latency_{key}_us"] = value[f"{key}_us"]
    elif job.mode == MODES[1]:
        row.update(client_to_server_gbps=parsed["local"]["average_gbps"],
                   server_to_client_gbps=parsed["remote"]["average_gbps"], aggregate_gbps=value["average_gbps"])
    else:
        sender = client_record if job.direction == "client_to_server" else server_record
        row[f"{job.direction}_gbps"] = sender["parsed"]["aggregate"]["average_gbps"]
    return row


class Store:
    def __init__(self, root, run_id, role):
        self.path = Path(root) / f"{run_id}-{role}"
        self.path.mkdir(parents=True, exist_ok=False)
        self.native_dir = self.path / "local"
        self.native_dir.mkdir()
        self.rows, self.alerts, self.role = [], [], role
        self.config, self.client, self.server = None, None, None
        self.status = "running"
        self.write("status.json", {"status": self.status, "started": utc_now()})

    def write(self, name, data):
        (self.path / name).write_text(json.dumps(data, indent=2, allow_nan=False) + "\n")

    def setup(self, config, client, server):
        self.config, self.client, self.server = config, client, server
        self.write("config.json", asdict(config))
        self.write("endpoints.json", {"client": asdict(client), "server": asdict(server)})

    def pair(self, job, local, remote):
        peer_dir = self.path / "peer"
        peer_dir.mkdir(exist_ok=True)
        (peer_dir / f"{job.id}.stdout.log").write_text(remote["stdout"])
        (peer_dir / f"{job.id}.stderr.log").write_text(remote["stderr"])
        self.write(f"peer/{job.id}.result.json", remote)
        with (self.path / "runs.jsonl").open("a") as stream:
            stream.write(json.dumps({"job": asdict(job), "client": local, "server": remote}) + "\n")
        if job.phase == "measured":
            self.rows.append(measured_row(job, local, remote))
            self.report()

    def report(self):
        with (self.path / "results.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS)
            writer.writeheader()
            writer.writerows(self.rows)
        lines = ["# RDMA benchmark results", "", f"Status: **{self.status}**. Measured runs: {len(self.rows)}.", ""]
        if self.client and self.server:
            lines += [f"Client: `{self.client.hostname}` ({self.client.ip}); server: `{self.server.hostname}` ({self.server.ip}).",
                      "Directions refer to these script roles, even when perftest roles swap.", ""]
        lines += ["Cells show median across repetitions [minimum–maximum]. Bandwidth is Gbit/s; latency is µs.",
                  "Latency summarizes perftest t_typical (half round-trip); it is not an independently measured one-way completion time.", ""]
        specs = [
            (MODES[0], "Unidirectional write bandwidth", ("client_to_server_gbps", "server_to_client_gbps"),
             ("Client → Server", "Server → Client")),
            (MODES[1], "Bidirectional write bandwidth", ("client_to_server_gbps", "server_to_client_gbps", "aggregate_gbps"),
             ("Client → Server", "Server → Client", "Aggregate")),
            (MODES[2], "Write ping-pong latency", ("latency_typical_us",), ("Reported latency (µs)",)),
        ]
        for mode, title, keys, labels in specs:
            lines += [f"## {title}", "", "| Message size | " + " | ".join(labels) + " |",
                      "|---|" + "---|" * len(keys)]
            for size in self.config.sizes if self.config else SIZES:
                cells = []
                for key in keys:
                    values = [r[key] for r in self.rows if r["mode"] == mode and r["size_bytes"] == size and key in r]
                    cells.append(f"{statistics.median(values):.3f} [{min(values):.3f}–{max(values):.3f}] (n={len(values)})" if values else "—")
                lines.append(f"| {size_label(size)} | " + " | ".join(cells) + " |")
            lines.append("")
        if self.alerts:
            lines += ["## Counter observations", ""] + [f"- {a}" for a in self.alerts] + [""]
        (self.path / "summary.md").write_text("\n".join(lines) + "\n")

    def finish(self, status, error=None):
        self.status = status
        self.write("status.json", {"status": status, "finished": utc_now(), "error": error,
                                   "measured_runs": len(self.rows) if self.role == "client" else None})
        if self.role == "client":
            self.report()


class Session:
    def __init__(self, channel, endpoint, peer, config, store):
        self.channel, self.endpoint, self.peer = channel, endpoint, peer
        self.config, self.store, self.active = config, store, None
        self.sequence = 0

    def launch(self, job, role):
        job.validate(self.config)
        argv = build_command(self.endpoint, self.peer.ip, role, self.config, job)
        self.active = NativeProcess(argv, self.store.native_dir, job)
        return self.active

    def cleanup(self):
        if self.active:
            self.active.stop()
            self.active = None

    def run_pair(self, mode, size, phase, direction, repetition=0, iterations=None):
        self.sequence += 1
        job = Job(f"{self.sequence:04d}-{phase}-{mode}-{size}-{repetition}-{direction}",
                  mode, size, phase, direction, repetition, iterations)
        job.validate(self.config)
        log(f"[{self.sequence:03d}] {phase}: {mode}, {size_label(size)}, {direction}, repetition {repetition or '-'}")
        self.channel.send("run", job=asdict(job))
        ready = self.channel.receive("ready", self.config.startup_timeout + 5)
        if ready.get("id") != job.id:
            raise BenchError("Mismatched listener readiness acknowledgement")
        try:
            native = self.launch(job, "client")
            if job.listener == "client":
                native.wait_ready(self.channel, self.config)
            self.channel.send("go", id=job.id)
            local, remote = wait_native(native, self.channel, self.config.test_timeout, peer_result=True)
            self.store.pair(job, local, remote)
            return local["parsed"]
        finally:
            self.cleanup()

    def counters(self, label):
        local = snapshot(self.endpoint)
        self.channel.send("snapshot", label=label)
        remote = self.channel.receive("snapshot", 20)["snapshot"]
        self.store.write(f"counters-{label}.json", {"client": local, "server": remote})
        return {"client": local, "server": remote}

    def suite(self):
        # Smoke all selected modes before any measured jobs. A failed 1 GiB test
        # must not masquerade as a completed, smaller-size-only benchmark.
        for mode in self.config.modes:
            direction = "both" if mode == MODES[1] else "pingpong" if mode == MODES[2] else "client_to_server"
            for size in sorted({min(self.config.sizes), max(self.config.sizes)}):
                self.run_pair(mode, size, "smoke", direction, iterations=100)
        for mode in self.config.modes:
            before = self.counters(f"{mode}-before")
            for size in self.config.sizes:
                if mode == MODES[2]:
                    if size >= 2**20:
                        parsed = self.run_pair(mode, size, "calibration", "pingpong", iterations=100)
                        count = calibrated_iterations(parsed)
                        warmup = max(10, math.ceil(count * 0.1))
                    else:
                        count, warmup = 10000, 1000
                    for repetition in range(1, self.config.repetitions + 1):
                        self.run_pair(mode, size, "warmup", "pingpong", repetition, warmup)
                        self.run_pair(mode, size, "measured", "pingpong", repetition, count)
                else:
                    directions = ("client_to_server", "server_to_client") if mode == MODES[0] else ("both",)
                    for direction in directions:
                        for repetition in range(1, self.config.repetitions + 1):
                            self.run_pair(mode, size, "measured", direction, repetition)
            after = self.counters(f"{mode}-after")
            changes = {role: counter_changes(before[role], after[role]) for role in before}
            self.store.write(f"counter-changes-{mode}.json", changes)
            for role, change in changes.items():
                for alert in change["alerts"]:
                    self.store.alerts.append(f"{mode}, {role}: {alert}")
                    log(f"Counter observation ({role}): {alert}")
        expected = len(self.config.sizes) * self.config.repetitions * sum(
            2 if mode == MODES[0] else 1 for mode in self.config.modes)
        if len(self.store.rows) != expected:
            raise BenchError(f"Expected {expected} measured runs, collected {len(self.store.rows)}")
        self.channel.send("finish")
        self.channel.receive("finished", 20)

    def serve(self):
        seen = set()
        idle_timeout = self.config.test_timeout + self.config.startup_timeout + 30
        while True:
            # The client may be parsing/reporting; never wait indefinitely after disconnect.
            deadline = time.monotonic() + idle_timeout
            message = None
            while message is None and time.monotonic() < deadline:
                message = self.channel.poll(0.1)
            if message is None:
                raise BenchError("Timed out waiting for next client command")
            kind = message["type"]
            if kind == "finish":
                self.channel.send("finished")
                return
            if kind == "snapshot":
                label = message["label"]
                if not re.fullmatch(r"[a-z-]+-(before|after)", label):
                    raise BenchError("Invalid counter snapshot label")
                data = snapshot(self.endpoint)
                self.store.write(f"counters-{label}.json", data)
                self.channel.send("snapshot", snapshot=data)
                continue
            if kind != "run":
                raise BenchError(f"Unexpected client command: {kind}")
            job = Job(**message["job"])
            job.validate(self.config)
            if job.id in seen:
                raise BenchError(f"Duplicate job {job.id}")
            seen.add(job.id)
            log(f"{job.id}: {size_label(job.size)}")
            try:
                native = None
                if job.listener == "server":
                    native = self.launch(job, "server")
                    native.wait_ready(self.channel, self.config)
                self.channel.send("ready", id=job.id)
                go = self.channel.receive("go", self.config.startup_timeout + 5,
                                          (lambda: native.check(require_running=True)) if native else None)
                if go.get("id") != job.id:
                    raise BenchError("Mismatched job start")
                if native is None:
                    native = self.launch(job, "server")
                record, _ = wait_native(native, self.channel, self.config.test_timeout)
                self.channel.send("result", id=job.id, record=record)
            finally:
                self.cleanup()


def new_run_id():
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]


def open_control(args, endpoint):
    if args.server:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((endpoint.ip, args.control_port))
            listener.listen(1)
            listener.settimeout(args.connect_timeout)
            log(f"Waiting for client on {endpoint.ip}:{args.control_port} ({endpoint.interface})")
            connection, _ = listener.accept()
            return Channel(connection)
    try:
        peer_ip = socket.gethostbyname(args.client)
    except OSError as exc:
        raise BenchError(f"Cannot resolve peer {args.client}: {exc}") from exc
    verify_route(endpoint, peer_ip)
    deadline = time.monotonic() + args.connect_timeout
    last_error = None
    while time.monotonic() < deadline:
        connection = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        connection.settimeout(min(2, max(0.1, deadline - time.monotonic())))
        try:
            connection.bind((endpoint.ip, 0))
            connection.connect((peer_ip, args.control_port))
            return Channel(connection)
        except OSError as exc:
            connection.close()
            last_error = exc
            time.sleep(min(0.25, max(0, deadline - time.monotonic())))
    raise BenchError(f"Cannot connect to {peer_ip}:{args.control_port}: {last_error}")


def preview(endpoint, config, role, peer):
    log(f"Dry run: {endpoint.hostname}, {endpoint.interface}/{endpoint.ip}, "
        f"{endpoint.device} port {endpoint.rdma_port}, GID {endpoint.gid_index}")
    total = 0
    for mode in config.modes:
        directions = ("client_to_server", "server_to_client") if mode == MODES[0] else (
            ("both",) if mode == MODES[1] else ("pingpong",))
        for size in config.sizes:
            iterations = (10000 if size < 2**20 else 100) if mode == MODES[2] else None
            for direction in directions:
                job = Job("preview", mode, size, "measured", direction, 1, iterations)
                log(f"{mode}: {size_label(size)}, {direction}, repeat {config.repetitions} times")
                log("  " + shlex.join(build_command(endpoint, peer, role, config, job)))
                total += config.repetitions
    log(f"{total} measured runs. Each selected mode also smoke-tests its smallest/largest size (100 exchanges).")
    log("Latency has a separate warm-up before every repetition. At >=1 MiB, a 100-exchange calibration "
        "sets iterations targeting 15 seconds using perftest's average half-RTT; displayed -n 100 is a placeholder.")
    log("No sockets opened, benchmarks started, or result directories created. Peer discovery/version checks occur in a real run.")


def parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    roles = p.add_mutually_exclusive_group(required=True)
    roles.add_argument("--server", action="store_true", help="Wait for a benchmark controller")
    roles.add_argument("--client", metavar="SERVER_IP", help="Connect and control the selected benchmarks")
    p.add_argument("--interface", default="enp1s0f1np1", help="Interface for both control and RDMA traffic")
    p.add_argument("--ip", help="Select a local IPv4 address if the interface has multiple addresses")
    p.add_argument("--device", help="RDMA device override (must match interface/IP)")
    p.add_argument("--rdma-port", type=int, help="RDMA device port override")
    p.add_argument("--gid-index", type=int, help="IPv4 RoCEv2 GID index override")
    for mode in MODES:
        p.add_argument("--" + mode, action="store_true", help="Select " + mode)
    p.add_argument("--all", action="store_true", help="Select all three benchmark modes")
    p.add_argument("--sizes", type=parse_size, nargs="+", default=list(SIZES), help="Message sizes in bytes or KiB/MiB/GiB")
    p.add_argument("--repetitions", type=int, default=3, help="Measured repetitions per combination")
    p.add_argument("--duration", type=int, default=20, help="Perftest bandwidth duration in seconds")
    p.add_argument("--output-dir", default="results", help="Parent directory for unique run directories")
    p.add_argument("--control-port", type=int, default=18516, help="TCP coordination port; must match on both hosts")
    p.add_argument("--perftest-port", type=int, default=18515, help="TCP perftest port, negotiated by client")
    p.add_argument("--connect-timeout", type=float, default=300, help="Seconds to wait for the other script")
    p.add_argument("--startup-timeout", type=float, default=30, help="Seconds allowed for a native listener to start")
    p.add_argument("--test-timeout", type=float, default=180, help="Execution timeout per native invocation")
    p.add_argument("--dry-run", action="store_true", help="Preview local commands without connecting or benchmarking")
    return p


def main(argv=None):
    p = parser()
    args = p.parse_args(argv)
    selected = tuple(m for m in MODES if args.all or getattr(args, m.replace("-", "_")))
    if not args.server and not selected:
        p.error("Select --all or at least one benchmark mode")
    if args.server and selected and not args.dry_run:
        p.error("The client selects benchmarks; omit mode flags on the server")
    if not 1024 <= args.control_port <= 65535:
        p.error("--control-port must be between 1024 and 65535")
    if not math.isfinite(args.connect_timeout) or args.connect_timeout <= 0:
        p.error("--connect-timeout must be positive and finite")
    config = Config(selected or MODES, tuple(args.sizes), args.repetitions, args.duration,
                    args.perftest_port, args.startup_timeout, args.test_timeout)
    channel, store, session, endpoint = None, None, None, None
    try:
        config.validate()
        if args.control_port == config.perftest_port:
            raise BenchError("Control and perftest ports must differ")
        endpoint = discover_endpoint(args)
        if args.dry_run:
            probe_tools(endpoint, config)
            preview(endpoint, config, "server" if args.server else "client", args.client or "<peer-interface-ip>")
            return 0
        if not args.server:
            probe_tools(endpoint, config)
            store = Store(args.output_dir, new_run_id(), "client")
            store.write("local-endpoint.json", asdict(endpoint))
        channel = open_control(args, endpoint)
        peer_ip = channel.sock.getpeername()[0]
        if args.server:
            hello = channel.receive("hello", 30)
            if hello.get("protocol") != PROTOCOL:
                raise BenchError("Incompatible control protocol")
            config = Config.from_dict(hello["config"])
            if config.perftest_port == args.control_port:
                raise BenchError("Control and perftest ports must differ")
            peer = Endpoint(**hello["endpoint"])
            if peer.ip != peer_ip:
                raise BenchError("Peer control address differs from its selected RDMA interface address")
            verify_route(endpoint, peer.ip)
            probe_tools(endpoint, config)
            verify_peer(endpoint, peer)
            run_id = hello["run_id"]
            if not re.fullmatch(r"[a-zA-Z0-9_-]{1,80}", run_id):
                raise BenchError("Invalid run ID")
            store = Store(args.output_dir, run_id, "server")
            store.setup(config, peer, endpoint)
            channel.send("hello_ack", protocol=PROTOCOL, endpoint=asdict(endpoint))
        else:
            channel.send("hello", protocol=PROTOCOL, config=asdict(config), endpoint=asdict(endpoint),
                         run_id=store.path.name.removesuffix("-client"))
            hello = channel.receive("hello_ack", 90)
            if hello.get("protocol") != PROTOCOL:
                raise BenchError("Incompatible control protocol")
            peer = Endpoint(**hello["endpoint"])
            if peer.ip != peer_ip:
                raise BenchError("Server control address differs from its selected RDMA interface address")
            verify_peer(endpoint, peer)
            store.setup(config, endpoint, peer)
        log(f"Results: {store.path.resolve()}")
        session = Session(channel, endpoint, peer, config, store)
        if args.server:
            session.serve()
        else:
            session.suite()
        store.finish("complete")
        log(f"Complete. Results: {store.path.resolve()}")
        return 0
    except (BenchError, OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError, KeyboardInterrupt) as exc:
        message = "Interrupted" if isinstance(exc, KeyboardInterrupt) else str(exc)
        log(f"ERROR: {message}")
        if channel:
            try:
                channel.send("error", message=message)
            except BenchError:
                pass
        if store:
            try:
                if endpoint:
                    store.write("counters-abort.json", snapshot(endpoint))
                store.finish("failed", message)
                log(f"Partial results: {store.path.resolve()}")
            except OSError:
                pass
        return 130 if isinstance(exc, KeyboardInterrupt) else 1
    finally:
        if session:
            session.cleanup()
        if channel:
            channel.close()


def interrupted(signum, frame):
    """Treat SIGTERM like Ctrl-C so both endpoints clean up native processes."""
    raise KeyboardInterrupt


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    sys.exit(main())
