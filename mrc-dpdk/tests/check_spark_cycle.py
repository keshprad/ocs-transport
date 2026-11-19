#!/usr/bin/env python3
"""Run the three-Spark double-ring WRITE test from Spark-04."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time

from render_spark_cycle_units import TX_PEER, render_unit


HOSTS = ("spark-04", "spark-02", "spark-03")
EDGES = tuple((host, "spark-" + TX_PEER[host[-2:]]) for host in HOSTS)
PROFILES = ("smoke", "failover")
ROLES = ("rx", "tx")
TRANSFER_BYTES = 256 * 1024 * 1024
PACKETS = TRANSFER_BYTES // 1024
ACTIVE_STATES = ("activating", "active", "deactivating", "reloading", "refreshing")
PORT_LINE = re.compile(r"DPDK port [0-9]+: (\S+)")
INVOCATION = re.compile(r"[0-9a-f]{32}")


class CycleError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise CycleError(message)


@dataclass(frozen=True)
class Endpoint:
    host: str
    role: str
    profile: str

    @property
    def unit(self):
        return f"ocs-mrc-cycle-{self.role}-{self.profile}.service"

    @property
    def label(self):
        return f"{self.host} {self.unit}"

    @property
    def bdfs(self):
        function = 0 if self.role == "tx" else 1
        return f"0000:01:00.{function}", f"0002:01:00.{function}"

    @property
    def inject_loss(self):
        return self.host == "spark-04" and self.role == "tx" and self.profile == "failover"


@dataclass(frozen=True)
class Journal:
    ports: dict
    result: dict | None
    completed_us: int | None
    tail: tuple


class Commands:
    def __init__(self, socket_dir):
        self.socket_path = str(Path(socket_dir) / "%C")
        self.sudo_journals = set()

    def run(self, host, *args, timeout=15):
        argv = list(map(str, args))
        if host != HOSTS[0]:
            argv = [
                "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
                "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2",
                "-o", "ControlMaster=auto", "-o", f"ControlPath={self.socket_path}",
                "-o", "ControlPersist=10s", host, shlex.join(argv),
            ]
        try:
            result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise CycleError(f"{host}: {shlex.join(map(str, args))}: {exc}") from exc
        require(
            result.returncode == 0,
            f"{host}: {shlex.join(map(str, args))} failed: "
            f"{(result.stderr or result.stdout).strip() or result.returncode}",
        )
        return result.stdout

    def properties(self, endpoint, *names):
        output = self.run(
            endpoint.host, "systemctl", "show", "--no-pager",
            *(f"--property={name}" for name in names), endpoint.unit,
        )
        return dict(line.split("=", 1) for line in output.splitlines() if "=" in line)

    def journal(self, endpoint, *filters):
        args = (
            "/usr/bin/journalctl", "--quiet", "--no-pager", "--output=json",
            "--output-fields=MESSAGE,_SYSTEMD_INVOCATION_ID,__REALTIME_TIMESTAMP",
            "--unit", endpoint.unit,
        )
        if endpoint in self.sudo_journals:
            # A fixed command allows a rule for each individual service. This
            # path filters the invocation locally instead of through sudo.
            return self.run(endpoint.host, "sudo", "-n", *args, "--lines=200")
        return self.run(endpoint.host, *args, *filters)

    def check_journal(self, endpoint):
        try:
            self.journal(endpoint, "--lines=0")
        except CycleError as unprivileged:
            self.sudo_journals.add(endpoint)
            try:
                self.journal(endpoint)
            except CycleError as scoped:
                raise CycleError(
                    f"{endpoint.label}: neither ordinary nor scoped journal access is available\n"
                    f"{unprivileged}\n{scoped}"
                ) from scoped


def in_parallel(items, action):
    """Let every submitted operation settle before inspecting any failures."""
    with ThreadPoolExecutor(max_workers=len(items)) as pool:
        futures = {item: pool.submit(action, item) for item in items}
    values = {}
    failures = []
    for item, future in futures.items():
        try:
            values[item] = future.result()
        except CycleError as exc:
            failures.append(str(exc))
    if failures:
        raise CycleError("\n".join(failures))
    return values


def verify_exec_start(endpoint, directory, value):
    rendered = render_unit(endpoint.host[-2:], directory, endpoint.role, endpoint.profile)
    expected = next(line.removeprefix("ExecStart=") for line in rendered.splitlines() if line.startswith("ExecStart="))
    installed = re.findall(r"argv\[\]=(.*?) ;", value)
    require(
        installed == [expected],
        f"{endpoint.label}: installed ExecStart does not match the cycle unit definition",
    )


def preflight_host(commands, host, profile, revision):
    units = [Endpoint(host, role, name) for role in ROLES for name in PROFILES]
    for endpoint in units:
        props = commands.properties(endpoint, "LoadState", "ActiveState")
        require(
            props.get("ActiveState") not in ACTIVE_STATES,
            f"{endpoint.label}: another run is {props.get('ActiveState')}",
        )
        if endpoint.profile == profile:
            require(props.get("LoadState") == "loaded", f"{endpoint.label}: cycle unit is missing")

    legacy = commands.run(
        host, "systemctl", "list-units", "--no-pager", "--no-legend",
        "--plain", "--type=service", "ocs-mrc-04-*.service",
    )
    for line in legacy.splitlines():
        columns = line.split()
        require(
            len(columns) < 3 or columns[2] not in ACTIVE_STATES,
            f"{host}: pair unit {columns[0]} is running",
        )

    digests = set()
    for endpoint in (unit for unit in units if unit.profile == profile):
        for action in ("start", "stop"):
            commands.run(host, "sudo", "-n", "-l", "/usr/bin/systemctl", action, endpoint.unit)
        commands.check_journal(endpoint)

        props = commands.properties(
            endpoint, "WorkingDirectory", "ExecStart", "UnsetEnvironment",
        )
        require(
            "RUNTIME_DIRECTORY" in shlex.split(props.get("UnsetEnvironment", "")),
            f"{endpoint.label}: add UnsetEnvironment=RUNTIME_DIRECTORY to the unit",
        )
        directory = Path(props.get("WorkingDirectory", ""))
        require(
            directory.parent == Path("/opt/ocs-mrc") and directory.name.startswith(f"{revision[:7]}."),
            f"{endpoint.label}: expected an installed snapshot for {revision}",
        )
        verify_exec_start(endpoint, directory, props.get("ExecStart", ""))
        binary = str(directory / "mrc_write")
        require(
            commands.run(host, "realpath", "--", binary).strip() == binary,
            f"{endpoint.label}: executable must be inside the installed snapshot",
        )
        ownership = commands.run(host, "stat", "-c", "%u %a", directory, binary).splitlines()
        for record in ownership:
            owner, mode = record.split()
            require(
                owner == "0" and not (int(mode, 8) & 0o022),
                f"{endpoint.label}: installed snapshot is writable by non-root users",
            )
        digests.add(commands.run(host, "sha256sum", "--", binary).split()[0])
    require(len(digests) == 1, f"{host}: RX and TX use different snapshot binaries")
    return digests.pop()


def parse_journal(endpoint, invocation, output):
    ports = {}
    result = completed_us = None
    tail = []
    for line in output.splitlines():
        event = json.loads(line)
        message = event.get("MESSAGE")
        if event.get("_SYSTEMD_INVOCATION_ID") != invocation or not isinstance(message, str):
            continue
        timestamp = int(event["__REALTIME_TIMESTAMP"])
        tail.append(message)
        port = PORT_LINE.fullmatch(message)
        if port and port[1] in endpoint.bdfs:
            ports.setdefault(port[1], timestamp)
        if message.startswith('{"role":'):
            require(result is None, f"{endpoint.label}: multiple terminal results for {invocation}")
            result, completed_us = json.loads(message), timestamp
    return Journal(ports, result, completed_us, tuple(tail[-12:]))


def start(commands, endpoint):
    host_script = """set -eu
active=$(systemctl show --property=ActiveState --value "$1")
case "$active" in
  inactive|failed) ;;
  *) printf '%s is %s\\n' "$1" "$active" >&2; exit 3 ;;
esac
since=$(date +%s.%6N)
sudo -n /usr/bin/systemctl start "$1"
invocation=$(systemctl show --property=InvocationID --value "$1")
printf '%s\\n%s\\n' "$since" "$invocation"
"""
    output = commands.run(
        endpoint.host, "bash", "-c", host_script, "spark-cycle", endpoint.unit,
    ).splitlines()
    require(bool(output), f"{endpoint.label}: missing start confirmation")
    since, *invocations = output
    require(re.fullmatch(r"[0-9]+\.[0-9]{6}", since), f"{endpoint.label}: invalid start timestamp")
    if invocations and INVOCATION.fullmatch(invocations[0]):
        return invocations[0]

    # A short-lived service can release its systemd state before the second show.
    log = commands.journal(endpoint, f"--since=@{since}")
    since_us = int(since.replace(".", ""))
    ids = {
        event["_SYSTEMD_INVOCATION_ID"]
        for event in map(json.loads, log.splitlines())
        if int(event["__REALTIME_TIMESTAMP"]) >= since_us
        and INVOCATION.fullmatch(event.get("_SYSTEMD_INVOCATION_ID", ""))
    }
    require(len(ids) == 1, f"{endpoint.label}: could not identify its new invocation")
    return ids.pop()


def stop_owned(commands, endpoint, invocation):
    # Check ownership in the same remote process as the stop command.
    commands.run(
        endpoint.host, "bash", "-c",
        'if [ "$(systemctl show --property=InvocationID --value "$1")" = "$2" ]; then '
        'sudo -n /usr/bin/systemctl stop "$1"; fi',
        "spark-cycle", endpoint.unit, invocation,
    )


def wait_for(commands, endpoint, invocation, deadline, *, ready=False):
    while True:
        output = commands.journal(endpoint, f"_SYSTEMD_INVOCATION_ID={invocation}")
        log = parse_journal(endpoint, invocation, output)
        state = commands.properties(endpoint, "ActiveState", "InvocationID")
        active = state.get("ActiveState") in ACTIVE_STATES
        owns_unit = state.get("InvocationID") == invocation
        if ready and active and owns_unit and set(log.ports) == set(endpoint.bdfs):
            return log
        if not ready and log.result is not None and not active:
            require(state.get("ActiveState") == "inactive", f"{endpoint.label}: service exited unsuccessfully")
            return log
        if not active or not owns_unit or time.monotonic() >= deadline:
            # Fetch trailing messages again after observing exit to avoid a journal race.
            output = commands.journal(endpoint, f"_SYSTEMD_INVOCATION_ID={invocation}")
            log = parse_journal(endpoint, invocation, output)
            if not ready and log.result is not None and state.get("ActiveState") == "inactive":
                return log
            goal = "receiver readiness" if ready else "successful WRITE completion"
            raise CycleError(
                f"{endpoint.label}: did not reach {goal} for {invocation}\n"
                + "\n".join(log.tail)
            )
        time.sleep(0.25)


def clock_bounds(commands, host):
    if host == HOSTS[0]:
        return 0, 0

    samples = []
    for _ in range(3):
        before_us = time.time_ns() // 1000
        remote_us = int(commands.run(host, "date", "+%s%6N").strip())
        after_us = time.time_ns() // 1000
        samples.append((remote_us - after_us, remote_us - before_us))
    return min(samples, key=lambda bound: bound[1] - bound[0])


def validate_results(profile, logs, bounds):
    starts = []
    ends = []
    results = []

    for sender, receiver in EDGES:
        tx = Endpoint(sender, "tx", profile)
        rx = Endpoint(receiver, "rx", profile)
        tx_log, rx_log = logs[tx], logs[rx]
        edge = f"{sender} → {receiver}"

        for endpoint, log in ((tx, tx_log), (rx, rx_log)):
            result = log.result
            require(
                set(log.ports) == set(endpoint.bdfs)
                and result["role"] == endpoint.role
                and result["bytes"] == TRANSFER_BYTES
                and result["packets"] == PACKETS,
                f"{endpoint.label}: unexpected ports, role, or payload size: {result}",
            )

        sent = tx_log.result["sent"]
        received = rx_log.result["received"]
        require(
            len(sent) == len(received) == 2 and min(sent + received) > 0
            and sum(sent) == sum(received) == PACKETS,
            f"{edge}: WRITE did not stripe all expected packets across both rails",
        )
        require(
            tx_log.result["write_completions"] == 1 and tx_log.result["transport_acks"] > 0,
            f"{edge}: requester did not complete the WRITE via transport ACK",
        )
        if tx.inject_loss:
            require(
                tx_log.result["dropped"] > 0 and tx_log.result["retries"][0] > 0
                and tx_log.result["ev_failed_mask"] & 2 and tx_log.result["ev_recovered_mask"] & 2
                and received[1] > 512,
                f"{edge}: did not recover path 1 and resume WRITE traffic: {tx_log.result}",
            )
        else:
            require(tx_log.result["dropped"] == 0, f"{edge}: unexpected synthetic loss")

        lower, upper = bounds[sender]
        starts.append(max(tx_log.ports.values()) - lower)
        ends.append(tx_log.completed_us - upper)
        results.append(f"{edge}: {TRANSFER_BYTES} bytes, RX rails {received}, TX retries {tx_log.result['retries']}")

    overlap_us = min(ends) - max(starts)
    require(overlap_us > 0, f"Could not confirm overlapping sender intervals across the three hosts ({overlap_us / 1000:.1f} ms)")
    return results, overlap_us


def exercise(commands, profile):
    endpoints = [Endpoint(host, role, profile) for role in ROLES for host in HOSTS]
    receivers = endpoints[:len(HOSTS)]
    senders = endpoints[len(HOSTS):]
    invocations = {}

    def launch(endpoint):
        invocation = start(commands, endpoint)
        invocations[endpoint] = invocation
        print(f"{endpoint.label} invocation {invocation}", flush=True)

    try:
        in_parallel(receivers, launch)
        deadline = time.monotonic() + 30
        in_parallel(receivers, lambda ep: wait_for(commands, ep, invocations[ep], deadline, ready=True))
        print("All three receivers opened both rails.", flush=True)
        bounds = in_parallel(HOSTS, lambda host: clock_bounds(commands, host))
        in_parallel(senders, launch)
        deadline = time.monotonic() + 125
        logs = in_parallel(endpoints, lambda ep: wait_for(commands, ep, invocations[ep], deadline))
        results, overlap_us = validate_results(profile, logs, bounds)
        for result in results:
            print(result)
        print(f"Three sender port-ready intervals overlapped by at least {overlap_us / 1000:.1f} ms.")
    finally:
        if invocations:
            try:
                in_parallel(invocations, lambda ep: stop_owned(commands, ep, invocations[ep]))
            except CycleError as exc:
                print(f"Cycle cleanup failed:\n{exc}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", required=True, help="full commit hash on all Spark test worktrees")
    parser.add_argument("--profile", required=True, choices=PROFILES)
    parser.add_argument(
        "--checkout",
        type=Path,
        default=Path.home() / "Documents/Workspace/ocs-transport-mrc-dpdk",
        help="Spark-04 test worktree used by the revision gate",
    )
    parser.add_argument("--preflight-only", action="store_true", help="check readiness without starting a service")
    args = parser.parse_args()

    if not re.fullmatch(r"[0-9a-f]{40}", args.revision):
        parser.error("--revision requires a full lowercase Git commit hash")

    try:
        with tempfile.TemporaryDirectory(prefix="spark-cycle-") as socket_dir:
            commands = Commands(socket_dir)
            worktree = args.checkout.resolve()
            gate = worktree / "mrc-dpdk/tests/verify_spark_revisions.sh"
            print(commands.run(HOSTS[0], "bash", gate, args.revision, timeout=70), end="", flush=True)
            digests = in_parallel(HOSTS, lambda host: preflight_host(commands, host, args.profile, args.revision))
            require(len(set(digests.values())) == 1, f"Installed Spark snapshots differ: {digests}")
            print(f"All Spark {args.profile} services use snapshot SHA-256 {digests[HOSTS[0]]}.", flush=True)
            if not args.preflight_only:
                exercise(commands, args.profile)
    except (CycleError, OSError, ValueError, KeyError, TypeError) as exc:
        print(f"Spark cycle failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
