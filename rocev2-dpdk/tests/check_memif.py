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
    cpus = (cpu,) if isinstance(cpu, int) else cpu
    lcores = ",".join(f"{index}@{core}" for index, core in enumerate(cpus))
    return [
        str(BINARY), f"--lcores={lcores}", "--no-huge", "--no-pci", "--no-telemetry",
        f"--file-prefix=roce-{directory.name}-{role}",
        f"--vdev=net_memif0,role={driver_role},socket={directory / 'link'},socket-abstract=no",
        "--", role, str(size), "127.0.0.1", str(port), ip, str(pmtu), "net_memif0",
    ]


def check(size, rx_mtu, tx_mtu, rx_cpu, tx_cpu, options=(), writes=1, qps=1, workers=1):
    total = size * writes * qps
    if writes != 1:
        options = (*options, "--writes", str(writes))
    if qps != 1:
        options = (*options, "--qps", str(qps))
    if workers != 1:
        options = (*options, "--workers", str(workers))
    with tempfile.TemporaryDirectory(prefix="roce-memif-") as directory:
        with socket.socket() as unused:
            unused.bind(("127.0.0.1", 0))
            port = unused.getsockname()[1]
        with subprocess.Popen(
            command(rx_cpu, Path(directory), "rx", size, port, "192.0.2.2", rx_mtu) + list(options),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        ) as receiver:
            try:
                assert receiver.stdout is not None
                ready, _, _ = select.select([receiver.stdout], [], [], 8)
                marker = receiver.stdout.readline() if ready else ""
                if marker != "control listener ready\n":
                    raise AssertionError(f"receiver startup: {marker!r}")
                sender = subprocess.run(
                    command(tx_cpu, Path(directory), "tx", size, port, "192.0.2.1", tx_mtu) + list(options),
                    capture_output=True, text=True, timeout=15, check=False,
                )
                assert sender.returncode == 0, sender.stdout + sender.stderr
                assert f"WRITE complete: {total} bytes" in sender.stdout, sender.stdout
                stdout, stderr = receiver.communicate(timeout=8)
                assert receiver.returncode == 0, stdout + stderr
                assert f"WRITE verified: {total} bytes" in stdout, stdout
                print(sender.stdout.strip())
                if "--report-bw" in options:
                    for side, output in (("tx", sender.stdout), ("rx", stdout)):
                        assert f"BANDWIDTH role={side} payload_bytes={total} " in output, output
                        assert f" writes={writes} qps={qps} workers={workers}" in output, output
                    print(stdout.strip())
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
    check(3, 1024, 1024, cpus[0], cpus[1])
    check(1048579, 1024, 512, cpus[0], cpus[1],
          ("--report-bw", "--tx-burst", "16", "--send-window", "64"))
    check(8195, 1024, 1024, cpus[0], cpus[1],
          ("--report-bw", "--send-window", "256"), writes=65)
    check(3, 1024, 1024, cpus[0], cpus[1], ("--report-bw",), qps=16)
    check(1048579, 1024, 1024, cpus[0], cpus[1],
          ("--report-bw", "--tx-burst", "16", "--send-window", "256"), writes=3, qps=16)
    if len(cpus) >= 4:
        check(1048579, 1024, 1024, cpus[:2], cpus[2:4],
              ("--report-bw", "--tx-burst", "16", "--send-window", "256"), writes=3, qps=3, workers=2)


if __name__ == "__main__":
    main()
