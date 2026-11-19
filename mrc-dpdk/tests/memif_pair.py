"""Run two DPDK endpoints connected only through two local memif devices."""

import json
import os
import subprocess
import tempfile
import time
from pathlib import Path


MACS = {
    "rx": ("02:00:00:00:00:10", "02:00:00:00:00:11"),
    "tx": ("02:00:00:00:00:20", "02:00:00:00:00:21"),
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def eal_args(binary, role, cpu, directory):
    memif_role = "server" if role == "rx" else "client"
    args = [str(binary), f"--lcores=0@{cpu}", "--no-huge", "--no-pci",
            "--no-telemetry", f"--file-prefix={directory.name}-{role}", "-m", "64"]
    for path, mac in enumerate(MACS[role]):
        args += [f"--vdev=net_memif{path},role={memif_role},"
                 f"socket={directory / f'path{path}.sock'},socket-abstract=no,mac={mac}"]
    return args


def start(args, directory):
    env = {**os.environ, "XDG_RUNTIME_DIR": str(directory)}
    return subprocess.Popen(args, env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)


def collect(process):
    stdout, stderr = process.communicate(timeout=12)
    require(process.returncode == 0,
            f"Endpoint exited {process.returncode}\n{stdout}\n{stderr}")
    return json.loads(stdout.strip().splitlines()[-1])


def run_pair(build_command, read_artifacts=None):
    cpus = sorted(os.sched_getaffinity(0))
    require(len(cpus) >= 2, "The two polling endpoints need at least two usable CPUs")
    with tempfile.TemporaryDirectory(prefix="mrc-") as temporary:
        directory = Path(temporary)
        processes = []
        try:
            receiver = start(build_command("rx", cpus[0], directory), directory)
            processes.append(receiver)
            deadline = time.monotonic() + 4
            while not all((directory / f"path{path}.sock").exists() for path in range(2)):
                if receiver.poll() is not None:
                    collect(receiver)
                    raise RuntimeError("Receiver exited before creating both memif sockets")
                require(time.monotonic() < deadline, "Timed out waiting for memif sockets")
                time.sleep(0.01)
            sender = start(build_command("tx", cpus[1], directory), directory)
            processes.append(sender)
            tx = collect(sender)
            rx = collect(receiver)
            return tx, rx, read_artifacts(directory) if read_artifacts else None
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.communicate()
