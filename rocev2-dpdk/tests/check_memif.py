#!/usr/bin/env python3
"""Run the production endpoint across two DPDK primary processes."""

import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile

BINARY = Path(__file__).resolve().parents[1] / "roce_write"


def command(cpu, directory, role, size, port, ip, pmtu):
    driver_role = "server" if role == "rx" else "client"
    return [
        str(BINARY), f"--lcores=0@{cpu}", "--no-huge", "--no-pci", "--no-telemetry",
        f"--file-prefix=roce-{directory.name}-{role}",
        f"--vdev=net_memif0,role={driver_role},socket={directory / 'link'},socket-abstract=no",
        "--", role, str(size), "127.0.0.1", str(port), ip, str(pmtu), "net_memif0",
    ]


def check(size, rx_mtu, tx_mtu, rx_cpu, tx_cpu):
    with tempfile.TemporaryDirectory(prefix="roce-memif-") as directory:
        with socket.socket() as unused:
            unused.bind(("127.0.0.1", 0))
            port = unused.getsockname()[1]
        with subprocess.Popen(
            command(rx_cpu, Path(directory), "rx", size, port, "192.0.2.2", rx_mtu),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        ) as receiver:
            try:
                assert receiver.stdout is not None
                ready, _, _ = select.select([receiver.stdout], [], [], 8)
                marker = receiver.stdout.readline() if ready else ""
                if marker != "control listener ready\n":
                    raise AssertionError(f"receiver startup: {marker!r}")
                sender = subprocess.run(
                    command(tx_cpu, Path(directory), "tx", size, port, "192.0.2.1", tx_mtu),
                    capture_output=True, text=True, timeout=15, check=False,
                )
                assert sender.returncode == 0, sender.stdout + sender.stderr
                assert f"WRITE complete: {size} bytes" in sender.stdout, sender.stdout
                stdout, stderr = receiver.communicate(timeout=8)
                assert receiver.returncode == 0, stdout + stderr
                assert f"WRITE verified: {size} bytes" in stdout, stdout
                print(sender.stdout.strip())
            except BaseException:
                if receiver.poll() is None:
                    receiver.terminate()
                stdout, stderr = receiver.communicate(timeout=5)
                print(stdout + stderr, end="")
                raise


def main():
    cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) < 2:
        raise RuntimeError("memif test needs two available CPUs")
    for size, rx_mtu, tx_mtu in ((3, 1024, 1024), (1048579, 1024, 512)):
        check(size, rx_mtu, tx_mtu, cpus[0], cpus[1])


if __name__ == "__main__":
    main()
