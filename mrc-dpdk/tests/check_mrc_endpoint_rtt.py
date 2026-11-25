#!/usr/bin/env python3
"""Independent wire inputs and numeric RTT oracle for endpoint feedback processing.

The C harness executes the production decoder and requester control path using
explicit send/arrival clocks. This tests software integration, not live-network
clock accuracy or hardware timestamping.
"""
import argparse
import ipaddress
import itertools
import struct
import subprocess
import tempfile
from pathlib import Path

from mrc_wire_reference import Bth, IPPeer, encode_ipv4, encode_ipv6, wrap_srv6


def frame(mode, kind, timestamp, retry):
    ipv6 = mode != "ipv4"
    source = IPPeer(bytes.fromhex("020000000002"),
                    ipaddress.ip_address("2001:db8::2" if ipv6 else "192.0.2.2").packed)
    destination = IPPeer(bytes.fromhex("020000000001"),
                         ipaddress.ip_address("2001:db8::1" if ipv6 else "192.0.2.1").packed)
    psn = 0xFFFFFD
    if kind in ("sack", "probe"):
        probe = kind == "probe"
        body = struct.pack("!BBHIHH", 0, 2 if probe else 0, 7 if probe else 0,
                           0xC000, 0x0345, 0x0456)
        body += struct.pack("!IBBhQ", psn, 0, 0, 1, 0)
        body += struct.pack("!HHI", timestamp, 0, 1)
        opcode = 0xDC
    else:
        body = struct.pack("!HBBIHHI", 0, 7, 0, 0xC000, 0x0345, 0x0456, psn)
        body += struct.pack("!I", 0x20000000 | timestamp)
        opcode = 0xDD
    encode = encode_ipv6 if ipv6 else encode_ipv4
    data = encode(source, destination, 0xD001, 4971,
                  Bth(opcode, 0xAF31, 0x120345, psn, 0x20 if retry else 0), body)
    if mode.startswith("srv6"):
        data = wrap_srv6(data, ipaddress.ip_address("2001:db8:bbbb::1").packed,
                         ipaddress.ip_address("2001:db8:aaaa::1").packed,
                         srh=mode == "srv6-srh")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path,
                        default=Path(__file__).resolve().with_name("mrc_endpoint_rtt"))
    args = parser.parse_args()
    cases = 0
    with tempfile.TemporaryDirectory(prefix="mrc-rtt-") as directory:
        path = Path(directory) / "feedback.bin"
        def check(mode, local, peer, kind, timestamp, elapsed, retries, sent_at=1000):
            path.write_bytes(frame(mode, kind, timestamp, retries > 0))
            service = timestamp * 128 if peer else 0
            expected = elapsed - service if service < elapsed and retries < 2 else 0
            subprocess.run([str(args.binary), str(path), str(local), str(peer), str(elapsed),
                            str(retries), str(expected), str(int(kind == "probe")), str(sent_at)], check=True)

        for mode, local, peer, kind, (timestamp, elapsed) in itertools.product(
                ("ipv4", "ipv6", "srv6", "srv6-srh"), (0, 1), (0, 1),
                ("sack", "probe", "nack"),
                ((0, 10000), (3, 10000), (65535, 10000000),
                 (1, 128), (2, 128), (1, 129))):
            attempts = (0,) if kind == "probe" else (0, 1, 2)
            for retries in attempts:
                check(mode, local, peer, kind, timestamp, elapsed, retries)
                cases += 1
        # Translating both local clocks across timestamp-wrap and clock-unit
        # boundaries must preserve RTT. The remote field remains a duration,
        # never an absolute timestamp, even when it resembles a local clock.
        for mode, local, peer, kind, epoch in itertools.product(
                ("ipv4", "ipv6", "srv6", "srv6-srh"), (0, 1), (0, 1),
                ("sack", "probe", "nack"),
                (65536 * 128 - 128, 65536 * 128 + 128, 10**9 - 128, 10**9 + 128)):
            check(mode, local, peer, kind, 3, 10000, 0, epoch)
            cases += 1
    print(f"Passed {cases} exact-clock endpoint RTT cases: all four capability directions, "
          "SACK/probe/NACK, four IP envelopes, timestamp boundaries, retry ambiguity, "
          "stored-clock translation and byte-credit preservation")


if __name__ == "__main__":
    main()
