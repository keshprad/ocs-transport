#!/usr/bin/env python3
"""Test double: real TCP listener/handshake, synthetic native-format measurements.

This never accesses RDMA hardware. Tests copy it to temporary executable paths.
"""

import argparse
import json
import os
from pathlib import Path
import socket
import sys
import time


if "--version" in sys.argv:
    print("Version: " + os.getenv("FAKE_PERFTEST_VERSION", "6.30"))
    sys.exit(0)
if "--help" in sys.argv:
    print("Usage: fake_perftest\n--ib-dev --ib-port --gid-index --connection --inline_size "
          "--size --port --bind_source_ip --iters --duration --margin --qp --tx-depth --report_gbits "
          "--noPeak --perform_warm_up --bidirectional --report-both")
    sys.exit(1)  # Like perftest's informational exits on some versions.

p = argparse.ArgumentParser()
for flag in ("-d", "-i", "-x", "-c", "-I", "-p", "-s", "-q", "-t", "-n", "-D", "-f", "--bind_source_ip"):
    p.add_argument(flag)
for flag in ("-b", "-N", "--report_gbits", "--report-both", "--perform_warm_up"):
    p.add_argument(flag, action="store_true")
p.add_argument("peer", nargs="?")
a = p.parse_args()
size, iterations = int(a.s), int(a.n or 100000)
ip = a.bind_source_ip
listener = a.peer is None
latency = "lat" in Path(sys.argv[0]).name
def event(kind):
    if os.getenv("FAKE_EVENTS"):
        with open(os.environ["FAKE_EVENTS"], "a") as f:
            f.write(json.dumps({"event": kind, "time": time.monotonic(), "pid": os.getpid(),
                                "ip": ip, "listener": listener, "argv": sys.argv[1:]}) + "\n")

event("start")
if (a.D and os.getenv("FAKE_FAIL_MEASURED_SIZE") == str(size) and
        os.getenv("FAKE_FAIL_IP", ip) == ip):
    print("injected native failure", file=sys.stderr, flush=True)
    sys.exit(7)
if os.getenv("FAKE_REQUIRE_WARMUP") and not latency and not a.perform_warm_up:
    print("native bandwidth warm-up was not requested", file=sys.stderr, flush=True)
    sys.exit(7)

# Deliberately print the banner before listening, matching native perftest.
if listener:
    print("* Waiting for client to connect... *", flush=True)
    if os.getenv("FAKE_NO_LISTEN"):
        time.sleep(60)
    time.sleep(0.03)
    with socket.socket() as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((ip, int(a.p)))
        sock.listen(1)
        event("listening")
        sock.settimeout(5)
        conn, _ = sock.accept()
else:
    conn = socket.socket()
    conn.settimeout(5)
    conn.bind((ip, 0))
    conn.connect((a.peer, int(a.p)))
with conn:
    event("connected")
    conn.settimeout(5)
    expected = json.dumps([size, iterations, bool(a.b), latency, a.D, a.f, a.perform_warm_up]).encode() + b"\n"
    conn.sendall(expected)
    data = b""
    while b"\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            raise RuntimeError("fake peer closed before handshake")
        data += chunk
    if data != expected:
        raise RuntimeError("fake endpoints received different configurations")
    if os.getenv("FAKE_HANG"):
        time.sleep(60)
    time.sleep(0.02)

print("RDMA_Write test\nConnection type : RC\nLink type : Ethernet")
if latency:
    # Native half-RTT average of 50 ms -> 150 iterations target 15 seconds.
    print(" #bytes #iterations    t_min[usec]    t_max[usec]  t_typical[usec]    t_avg[usec]    t_stdev[usec]   99% percentile[usec]   99.9% percentile[usec]")
    print(f" {size} {iterations} 45000.00 60000.00 49000.00 50000.00 100.00 57000.00 59000.00")
else:
    second = ip in ("127.0.0.3", "127.0.0.4")
    client_side = ip in ("127.0.0.1", "127.0.0.3")
    forward, reverse = (90, 80) if second else (180, 170)
    def table(value):
        print(" #bytes     #iterations    BW peak[Gb/sec]    BW average[Gb/sec]   MsgRate[Mpps]")
        print(f" {size} {iterations} 0.00 {value:.2f} 0.123456")
    if a.b:
        table(forward + reverse)
        print("\n Local results: ")
        table(forward if client_side else reverse)
        print("\n Remote results: ")
        table(reverse if client_side else forward)
    else:
        table(forward if client_side != listener else reverse)
event("finished")
