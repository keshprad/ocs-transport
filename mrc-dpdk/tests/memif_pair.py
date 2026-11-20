"""Run two DPDK endpoints connected through local memif devices."""

import json
import os
import subprocess
import tempfile
import time
from pathlib import Path


MACS = {
    role: tuple(f"02:00:00:00:00:{base + path:02x}" for path in range(8))
    for role, base in (("rx", 0x10), ("tx", 0x20))
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def eal_args(binary, role, cpu, directory, *, rail_count=2):
    memif_role = "server" if role == "rx" else "client"
    args = [str(binary), f"--lcores=0@{cpu}", "--no-huge", "--no-pci",
            "--no-telemetry", f"--file-prefix={directory.name}-{role}", "-m", "64"]
    for path, mac in enumerate(MACS[role][:rail_count]):
        args += [f"--vdev=net_memif{path},role={memif_role},"
                 f"socket={directory / f'path{path}.sock'},socket-abstract=no,mac={mac}"]
    return args


def start(args, directory):
    env = {**os.environ, "XDG_RUNTIME_DIR": str(directory)}
    return subprocess.Popen(args, env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)


def collect(process, expected_error=None):
    stdout, stderr = process.communicate(timeout=12)
    if expected_error is not None:
        require(process.returncode != 0 and expected_error in stderr,
                f"Expected {expected_error!r} with failure, got {process.returncode}\n{stdout}\n{stderr}")
        return stderr
    require(process.returncode == 0,
            f"Endpoint exited {process.returncode}\n{stdout}\n{stderr}")
    return json.loads(stdout.strip().splitlines()[-1])


def run_pair(build_command, read_artifacts=None, expected_errors=None, sender_started=None, *,
             rail_count=2):
    expected_errors = expected_errors or {}
    cpus = sorted(os.sched_getaffinity(0))
    require(len(cpus) >= 2, "The two polling endpoints need at least two usable CPUs")
    with tempfile.TemporaryDirectory(prefix="mrc-") as temporary:
        directory = Path(temporary)
        processes = []
        try:
            receiver = start(build_command("rx", cpus[0], directory), directory)
            processes.append(receiver)
            deadline = time.monotonic() + 4
            while not all((directory / f"path{path}.sock").exists() for path in range(rail_count)):
                if receiver.poll() is not None:
                    collect(receiver)
                    raise RuntimeError(f"Receiver exited before creating {rail_count} memif sockets")
                require(time.monotonic() < deadline, "Timed out waiting for memif sockets")
                time.sleep(0.01)
            sender = start(build_command("tx", cpus[1], directory), directory)
            processes.append(sender)
            if sender_started:
                sender_started(sender)
            tx = collect(sender, expected_errors.get("tx"))
            rx = collect(receiver, expected_errors.get("rx"))
            return tx, rx, read_artifacts(directory) if read_artifacts else None
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.communicate()
