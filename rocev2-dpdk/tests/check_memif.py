#!/usr/bin/env python3
"""Run the production endpoint across two DPDK primary processes."""

from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import time

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


def check(size, rx_mtu, tx_mtu, rx_cpu, tx_cpu, options=(), writes=1, qps=1, workers=1, gate_port=0):
    total = size * writes * qps
    if writes != 1:
        options = (*options, "--writes", str(writes))
    if qps != 1:
        options = (*options, "--qps", str(qps))
    if workers != 1:
        options = (*options, "--workers", str(workers))
    tx_options = (*options, "--start-gate", str(gate_port)) if gate_port else options
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
                    command(tx_cpu, Path(directory), "tx", size, port, "192.0.2.1", tx_mtu) + list(tx_options),
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
                return port, sender.stdout
            except BaseException:
                if receiver.poll() is None:
                    receiver.terminate()
                stdout, stderr = receiver.communicate(timeout=5)
                print(stdout + stderr, end="")
                raise


def check_start_gate(cpus):
    with socket.socket() as gate, ThreadPoolExecutor(2) as pool, ExitStack() as connections:
        gate.bind(("127.0.0.1", 0))
        gate.listen(2)
        gate.settimeout(8)
        port = gate.getsockname()[1]
        trials = [
            pool.submit(check, 1048576, 1024, 1024, cpus[i], cpus[i + 1],
                        ("--report-bw",), writes=32, gate_port=port)
            for i in (0, 2)
        ]
        identities = {}
        for _ in trials:
            conn, _ = gate.accept()
            connections.enter_context(conn)
            conn.settimeout(8)
            stream = connections.enter_context(conn.makefile("rb"))
            control_port, = struct.unpack("!H", stream.read(2))
            assert control_port not in identities
            identities[control_port] = conn
        release = time.monotonic_ns() + 50_000_000
        for conn in identities.values():
            conn.sendall(struct.pack("!Q", release))
        for trial in trials:
            peer_port, output = trial.result()
            assert peer_port in identities, identities
            result = next(line for line in output.splitlines() if line.startswith("BANDWIDTH "))
            start = next(int(field.split("=")[1]) for field in result.split()
                         if field.startswith("start_ns="))
            assert start >= release, result
        print("Two verified transfers honored a shared sender release")


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
    check(8195, 1024, 1024, cpus[0], cpus[1], ("--queue-window", "7"), writes=3, qps=16)
    check(1048579, 1024, 1024, cpus[0], cpus[1],
          ("--report-bw", "--tx-burst", "16", "--send-window", "256"), writes=3, qps=16)
    if len(cpus) >= 4:
        check(1048579, 1024, 1024, cpus[:2], cpus[2:4],
              ("--report-bw", "--tx-burst", "16", "--send-window", "256"), writes=3, qps=3, workers=2)
        check_start_gate(cpus[:4])


if __name__ == "__main__":
    main()
