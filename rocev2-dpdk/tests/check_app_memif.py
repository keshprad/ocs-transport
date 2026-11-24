#!/usr/bin/env python3
"""Run application-backed RoCEv2 WRITEs with four independent processes."""

import argparse
from contextlib import ExitStack
import os
from pathlib import Path
import re
import select
import socket
import subprocess
import tempfile

from check_memif import BINARY, command

APPLICATION = BINARY.with_name("roce_app_write")


def process(stack, args, env=None):
    child = subprocess.Popen(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

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
    return stdout


def check(cpus, size, writes, qps, workers, device, *, scatter_gather=False, cm=False,
          provider=None, post_batch=None, native_queues=False, rdmacm=False):
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
        rx_mr = ["--mr-socket", str(directory / "rx-mr.sock")] if provider else []
        tx_mr = ["--mr-socket", str(directory / "tx-mr.sock")] if provider else []
        rx_cma = ["--rdmacm-socket", str(directory / "rx-cma.sock")] if rdmacm else []
        tx_cma = ["--rdmacm-socket", str(directory / "tx-cma.sock")] if rdmacm else []

        rx_path = directory / "rx.sock"
        rx = process(stack, command(cpus[:workers], directory, "rx", size, port, "192.0.2.2", 1024, mac=rx_mac)
                     + options + rx_cm + rx_mr + rx_cma + ["--app-socket", str(rx_path)])
        expect_ready(rx, "application listener ready")
        rx_env = {**os.environ, "ROCE_DPDK_MR_SOCKET": rx_mr[1], "RDMAV_DRIVERS": provider} if provider else None
        if rdmacm:
            rx_env["ROCE_DPDK_CM_SOCKET"] = rx_cma[1]
        rx_app = process(stack, [APPLICATION, str(rx_path), *app_options], env=rx_env)
        expect_ready(rx, "control listener ready")

        tx_path = directory / "tx.sock"
        tx_options = options + (["--scatter-gather"] if scatter_gather else [])
        if post_batch:
            tx_options += ["--app-post-batch", str(post_batch)]
        if native_queues:
            tx_options.append("--verbs-queues")
        tx = process(stack, command(cpus[workers:2 * workers], directory, "tx", size, port, "192.0.2.1", 1024, mac=tx_mac)
                     + tx_options + tx_cm + tx_mr + tx_cma + ["--app-socket", str(tx_path)])
        expect_ready(tx, "application listener ready")
        tx_env = {**os.environ, "ROCE_DPDK_MR_SOCKET": tx_mr[1], "RDMAV_DRIVERS": provider} if provider else None
        if rdmacm:
            tx_env["ROCE_DPDK_CM_SOCKET"] = tx_cma[1]
        tx_app = process(stack, [APPLICATION, str(tx_path), *app_options], env=tx_env)

        total = size * writes * qps
        cm_marker = ("RoCEv2 CM established", "RoCEv2 CM disconnected") if cm else ()
        tx_log = expect_success(tx, f"WRITE complete: {total} bytes", *cm_marker)
        rx_log = expect_success(rx, f"WRITE verified: {total} bytes", *cm_marker)
        app_marker = ("Application CM established",) if cm else ()
        if rdmacm:
            app_marker = ("Application librdmacm established", "Application librdmacm disconnected")
        completion_suffix = " via libibverbs" if native_queues else ""
        tx_app_log = expect_success(tx_app, f"Application completed {total} bytes",
                                    f"Application observed {writes} WRITE completions per QP{completion_suffix}",
                                    *app_marker)
        rx_app_log = expect_success(rx_app, f"Application verified {total} bytes", *app_marker)
        if rdmacm:
            pattern = r"RoCEv2 CM established local_qpn=([0-9a-f]{6}) peer_qpn=([0-9a-f]{6})"
            tx_qps = re.search(pattern, tx_log)
            rx_qps = re.search(pattern, rx_log)
            assert tx_qps and rx_qps, tx_log + rx_log
            assert tx_qps.groups() == rx_qps.groups()[::-1], tx_log + rx_log
            for qps, app_log in ((tx_qps, tx_app_log), (rx_qps, rx_app_log)):
                created = f"Application created verbs QP {qps[1]} before CM"
                assert created in app_log.split("Application librdmacm established", 1)[0], app_log


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    registration = parser.add_mutually_exclusive_group()
    registration.add_argument("--kernel-mr", metavar="DEVICE", help="register through this loaded kernel device")
    registration.add_argument("--verbs-provider", metavar="PREFIX", type=lambda path: str(Path(path).resolve()),
                              help="register through this libibverbs provider path without -rdmav34.so")
    args = parser.parse_args()
    cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) < 2:
        raise RuntimeError("application test needs two available CPUs")
    if args.verbs_provider:
        check(cpus, 8195, 3, 1, 1, None, cm=True, provider=args.verbs_provider,
              post_batch=1, native_queues=True, rdmacm=True)
        check(cpus, 8195, 3, 1, 1, None, scatter_gather=True, cm=True, provider=args.verbs_provider,
              post_batch=1, native_queues=True, rdmacm=True)
        if len(cpus) >= 4:
            check(cpus, 8195, 3, 4, 2, None, provider=args.verbs_provider, post_batch=1, native_queues=True)
        return
    check(cpus, 8195, 1, 1, 1, args.kernel_mr)
    check(cpus, 8195, 1, 1, 1, args.kernel_mr, scatter_gather=True)
    check(cpus, 8195, 3, 1, 1, args.kernel_mr, scatter_gather=True, cm=True, post_batch=1)
    if len(cpus) >= 4:
        check(cpus, 262147, 3, 4, 2, args.kernel_mr, post_batch=1)
        check(cpus, 262147, 3, 4, 2, args.kernel_mr, scatter_gather=True, post_batch=1)


if __name__ == "__main__":
    main()
