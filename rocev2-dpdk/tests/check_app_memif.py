#!/usr/bin/env python3
"""Run application-backed RoCEv2 WRITEs with four independent processes."""

import argparse
from contextlib import ExitStack
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile

from check_memif import BINARY, command

APPLICATION = BINARY.with_name("roce_app_write")


def process(stack, args):
    child = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def stop():
        if child.poll() is None:
            child.kill()
        child.wait(timeout=5)
        child.stdout.close()
        child.stderr.close()

    stack.callback(stop)
    return child


def expect_ready(child, marker):
    ready, _, _ = select.select([child.stdout], [], [], 8)
    line = child.stdout.readline() if ready else ""
    if line != marker + "\n":
        if child.poll() is None:
            child.terminate()
        stdout, stderr = child.communicate(timeout=5)
        raise AssertionError(f"expected {marker!r}, got {line!r}\n{stdout}{stderr}")


def expect_success(child, *markers):
    stdout, stderr = child.communicate(timeout=15)
    assert child.returncode == 0 and all(marker in stdout for marker in markers), stdout + stderr
    print(stdout.strip())


def check(cpus, size, writes, qps, workers, device, *, scatter_gather=False, cm=False):
    with tempfile.TemporaryDirectory(prefix="roce-app-memif-") as directory, ExitStack() as stack:
        with socket.socket() as unused:
            unused.bind(("127.0.0.1", 0))
            port = unused.getsockname()[1]
        directory = Path(directory)
        options = ["--report-bw", "--writes", str(writes), "--qps", str(qps), "--workers", str(workers)]
        app_options = ["--kernel-mr", device] if device else []
        tx_mac = "02:00:c0:00:02:01" if cm else None
        rx_mac = "02:00:c0:00:02:02" if cm else None
        rx_cm = ["--cm-peer", "192.0.2.1", tx_mac] if cm else []
        tx_cm = ["--cm-peer", "192.0.2.2", rx_mac] if cm else []

        rx_path = directory / "rx.sock"
        rx = process(stack, command(cpus[:workers], directory, "rx", size, port, "192.0.2.2", 1024, mac=rx_mac)
                     + options + rx_cm + ["--app-socket", str(rx_path)])
        expect_ready(rx, "application listener ready")
        rx_app = process(stack, [APPLICATION, str(rx_path), *app_options])
        expect_ready(rx, "control listener ready")

        tx_path = directory / "tx.sock"
        tx_options = options + (["--scatter-gather"] if scatter_gather else [])
        tx = process(stack, command(cpus[workers:2 * workers], directory, "tx", size, port, "192.0.2.1", 1024, mac=tx_mac)
                     + tx_options + tx_cm + ["--app-socket", str(tx_path)])
        expect_ready(tx, "application listener ready")
        tx_app = process(stack, [APPLICATION, str(tx_path), *app_options])

        total = size * writes * qps
        cm_marker = ("RoCEv2 CM established",) if cm else ()
        expect_success(tx, f"WRITE complete: {total} bytes", *cm_marker)
        expect_success(rx, f"WRITE verified: {total} bytes", *cm_marker)
        app_marker = ("Application CM established",) if cm else ()
        expect_success(tx_app, f"Application completed {total} bytes", *app_marker)
        expect_success(rx_app, f"Application verified {total} bytes", *app_marker)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-mr", metavar="DEVICE", help="register through this loaded kernel device")
    args = parser.parse_args()
    cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) < 2:
        raise RuntimeError("application test needs two available CPUs")
    check(cpus, 8195, 1, 1, 1, args.kernel_mr)
    check(cpus, 8195, 1, 1, 1, args.kernel_mr, scatter_gather=True)
    check(cpus, 8195, 3, 1, 1, args.kernel_mr, scatter_gather=True, cm=True)
    if len(cpus) >= 4:
        check(cpus, 262147, 3, 4, 2, args.kernel_mr)
        check(cpus, 262147, 3, 4, 2, args.kernel_mr, scatter_gather=True)


if __name__ == "__main__":
    main()
