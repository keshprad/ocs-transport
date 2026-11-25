"""Commands and systemd service lifecycle helpers for Spark benchmarks."""

import json
from pathlib import Path
import re
import shlex
import subprocess


LOCAL_HOST = "spark-04"
ACTIVE_STATES = ("activating", "active", "deactivating", "reloading", "refreshing")
INVOCATION = re.compile(r"[0-9a-f]{32}")


class CycleError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise CycleError(message)


class Commands:
    def __init__(self, socket_dir):
        self.socket_path = str(Path(socket_dir) / "%C")
        self.sudo_journals = set()

    def run(self, host, *args, timeout=15):
        argv = list(map(str, args))
        if host != LOCAL_HOST:
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
        endpoint.host, "bash", "-c", host_script, "spark-service", endpoint.unit,
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
        "spark-service", endpoint.unit, invocation,
    )
