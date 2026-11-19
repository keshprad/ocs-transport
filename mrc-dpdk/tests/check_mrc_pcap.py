#!/usr/bin/env python3
"""Replay independently constructed IPv4 MRC requests and inspect the replies."""

import argparse
import ipaddress
import json
import os
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path


REQUESTOR = 0x120345
RESPONDER = 0x230456
PKEY = 0xAF31
INITIAL_PSN = 0xFFFFFF
ADDRESS = 0x123400000
RKEY = 0xDECAFE
UDP_PORT = 4971
PEER_IPV4 = ipaddress.IPv4Address("192.0.2.1").packed
LOCAL_IPV4 = ipaddress.IPv4Address("192.0.2.2").packed
PEER_MACS = [bytes.fromhex("0200000000a0"), bytes.fromhex("0200000000a1")]
PCAP_MACS = [bytes.fromhex("027063617000"), bytes.fromhex("027063617001")]
ENTROPIES = [0xE241, 0xE3B2]
PDC_IDS = (REQUESTOR & 0xFFFF, RESPONDER & 0xFFFF)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def ipv4_checksum(header):
    value = sum(struct.unpack("!10H", header))
    while value >> 16:
        value = (value & 0xFFFF) + (value >> 16)
    return value ^ 0xFFFF


def icrc(protected):
    # RoCEv2 IPv4 invariant fields, including the all-ones masked LRH.
    ip_udp_bth = bytearray(protected[:40])
    for index in (1, 8, 10, 11, 26, 27, 32):
        ip_udp_bth[index] = 0xFF
    return struct.pack("<I", zlib.crc32(b"\xff" * 8 + ip_udp_bth + protected[40:]))


def request(path, opcode, psn, body, flags=0):
    pad = (-len(body)) % 4
    bth = struct.pack("!BBHII", opcode, pad << 4, PKEY, RESPONDER, flags << 24 | psn)
    transport = bth + body + b"\0" * pad
    udp_size = 8 + len(transport) + 4
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + udp_size, 0, 0x4000,
                     63, 17, 0, PEER_IPV4, LOCAL_IPV4)
    ip = ip[:10] + struct.pack("!H", ipv4_checksum(ip)) + ip[12:]
    protected = ip + struct.pack("!4H", ENTROPIES[path], UDP_PORT, udp_size, 0) + transport
    return PCAP_MACS[path] + PEER_MACS[path] + b"\x08\x00" + protected + icrc(protected)


def write_request(path, opcode, psn, msn, rqmsn, offset, length, payload, immediate=None, flags=0x80):
    headers = struct.pack("!HHQII", rqmsn, msn, ADDRESS + offset, RKEY, length)
    if immediate is not None:
        headers += struct.pack("!I", immediate)
    return request(path, opcode, psn, headers + payload, flags)


def captured_requests():
    # Match the benchmark's application payload check without its wire codec.
    data = bytes((i ^ (i >> 8)) & 0xFF for i in range(13 + 1025))
    first = write_request(0, 0xCA, INITIAL_PSN, 1, 0, 0, 13, data[:13])
    corrupt = first[:-1] + bytes([first[-1] ^ 1])
    outside = (INITIAL_PSN + 1024) & 0xFFFFFF
    outside_write = write_request(0, 0xCA, outside, 1, 0, 0, 13, data[:13])
    tail = write_request(0, 0xC9, 1, 2, 1, 1037, 1025, data[1037:], 0xFEC01234)
    duplicate = write_request(0, 0xC9, 1, 2, 1, 1037, 1025,
                              data[1037:], 0xFEC01234, flags=0xA0)
    head = write_request(1, 0xC6, 0, 2, 1, 13, 1025, data[13:1037])
    # PETH token, PDC identifiers, and a type-1 timestamp extension.
    probe = request(0, 0xDE, 0, struct.pack("!7HBB", 0, 0, 0x91BC, 0, *PDC_IDS,
                                           0x1731, 0, 1))
    return [(outside_write, probe, corrupt, first, tail, duplicate), (head,)], outside


def write_capture(path, frames):
    with path.open("wb") as file:
        file.write(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        for tick, frame in enumerate(frames):
            file.write(struct.pack("<4I", 1, tick, len(frame), len(frame)))
            file.write(frame)


def read_capture(path):
    with path.open("rb") as file:
        header = file.read(24)
        byte_order = {
            b"\xd4\xc3\xb2\xa1": "<", b"\x4d\x3c\xb2\xa1": "<",
            b"\xa1\xb2\xc3\xd4": ">", b"\xa1\xb2\x3c\x4d": ">",
        }.get(header[:4])
        require(len(header) == 24 and byte_order, f"{path} is not a classic pcap")
        require(struct.unpack_from(byte_order + "I", header, 20)[0] == 1,
                f"{path} link type is not Ethernet")
        while record := file.read(16):
            require(len(record) == 16, f"{path} has a truncated packet header")
            _, _, captured, original = struct.unpack(byte_order + "4I", record)
            frame = file.read(captured)
            require(len(frame) == captured == original, f"{path} has a truncated packet")
            yield frame


def decode_response(path, frame):
    require(frame[:14] == PEER_MACS[path] + PCAP_MACS[path] + b"\x08\x00",
            f"Reply used the wrong Ethernet path {path}")
    ip = frame[14:]
    require(len(ip) >= 44, "Truncated MRC response")
    require(ip[0] == 0x45 and ip[9] == 17 and ip[12:20] == LOCAL_IPV4 + PEER_IPV4,
            "Unexpected response IP envelope")
    require(ipv4_checksum(ip[:20]) == 0 and int.from_bytes(ip[2:4], "big") == len(ip),
            "Response has an invalid IPv4 checksum or length")
    _, destination, udp_size, checksum = struct.unpack_from("!4H", ip, 20)
    require((destination, udp_size, checksum) == (UDP_PORT, len(ip) - 20, 0),
            "Unexpected UDP port, length, or checksum")
    require(ip[-4:] == icrc(ip[:-4]), "Response iCRC did not match the independent calculation")
    opcode, _, pkey, qp, flags_psn = struct.unpack_from("!BBHII", ip, 28)
    require(pkey == PKEY and qp == (2 if opcode in (0xD8, 0xD9) else REQUESTOR),
            "Response used the wrong partition or destination QP")
    return opcode, flags_psn & 0xFFFFFF, flags_psn >> 24, ip[40:-4]


def check_responses(output_paths, outside):
    responses = [[decode_response(path, frame) for frame in read_capture(output)]
                 for path, output in enumerate(output_paths)]
    aeth = [[(psn, body) for opcode, psn, _, body in rail if opcode == 0xD1] for rail in responses]
    require(aeth[0] and all(body == b"\x1f\x00\x00\x01" and psn == INITIAL_PSN
                            for psn, body in aeth[0]), "Transport ACK completed WR 2 before its first packet")
    require((1, b"\x1f\x00\x00\x02") in aeth[1], "Final transport ACK did not complete MSN 2")

    sacks = [(psn, flags, body) for opcode, psn, flags, body in responses[0] if opcode == 0xDC]
    require(any(body[1] & 2 and struct.unpack_from("!H", body, 2)[0] == 0x91BC and
                struct.unpack_from("!H", body, 28)[0] == 0x1731 for _, _, body in sacks),
            "Probe response did not echo its token and timestamp")
    require(any(psn == INITIAL_PSN and not body[1] & 2 and
                struct.unpack_from("!h", body, 2)[0] == 2 and
                struct.unpack_from("!Q", body, 20)[0] and
                struct.unpack_from("!H", body, 30)[0] == 1 for psn, _, body in sacks),
            "Selective ACK did not report the segment beyond the gap")
    require(any(flags & 0x20 and psn == INITIAL_PSN for psn, flags, _ in sacks),
            "Selective ACK did not reflect the retransmission flag")
    nacks = [(psn, body) for opcode, psn, _, body in responses[0] if opcode == 0xDD]
    require(len(nacks) == 1 and nacks[0][0] == outside and nacks[0][1][2] == 0x0B and
            int.from_bytes(nacks[0][1][13:16], "big") == outside,
            "Out-of-window request did not produce the expected NACK")
    require(any(opcode == 0xDC and psn == 1 and body[17] == 0 and
                struct.unpack_from("!H", body, 30)[0] == 0
                for opcode, psn, _, body in responses[1]), "Final SACK did not close the receive gap")
    for path, rail in enumerate(responses):
        for opcode, _, _, body in rail:
            if opcode in (0xDC, 0xDD):
                require(struct.unpack_from("!IHH", body, 4) == (ENTROPIES[path], *PDC_IDS),
                        f"Reliability response on path {path} did not reflect entropy and PDC IDs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path(__file__).resolve().parents[1] / "mrc_write")
    binary = parser.parse_args().binary.resolve()
    require(binary.is_file(), f"Build mrc_write first: {binary}")
    with tempfile.TemporaryDirectory(prefix="mrc-pcap-") as temp:
        directory = Path(temp)
        incoming = [directory / f"rx{path}.pcap" for path in range(2)]
        outgoing = [directory / f"tx{path}.pcap" for path in range(2)]
        requests, outside = captured_requests()
        for capture, frames in zip(incoming, requests):
            write_capture(capture, frames)
        core = min(os.sched_getaffinity(0))
        command = [str(binary), f"--lcores=0@{core}", "--no-huge", "--no-pci", "--no-telemetry",
                   f"--file-prefix={directory.name}", "-m", "64"]
        for path in range(2):
            command.append(f"--vdev=net_pcap{path},rx_pcap={incoming[path]},tx_pcap={outgoing[path]}")
        command += ["--", "rx", "13,1025@0xfec01234", "192.0.2.2", "192.0.2.1",
                    "0", PEER_MACS[0].hex(":"), "1", PEER_MACS[1].hex(":"),
                    "--initial-psn", str(INITIAL_PSN), "--requestor-qpn", hex(REQUESTOR),
                    "--responder-qpn", hex(RESPONDER), "--p-key", hex(PKEY),
                    "--remote-address", hex(ADDRESS), "--remote-key", hex(RKEY),
                    "--port-stats", "1", "--timeout-ms", "2000"]
        result = subprocess.run(command, env={**os.environ, "XDG_RUNTIME_DIR": str(directory)},
                                capture_output=True, text=True, timeout=8)
        require(result.returncode == 0, f"pcap endpoint failed\n{result.stdout}\n{result.stderr}")
        counters = json.loads(result.stdout.strip().splitlines()[-1])
        require(counters["bytes"] == 1038 and counters["received"] == [2, 1] and
                counters["duplicates"] == 1 and counters["immediate_completions"] == 1 and
                counters["nack_sent_mask"] == 1 << 0x0B and "decode-rejected=1" in result.stderr,
                f"Unexpected placement or error counters: {counters}\n{result.stderr}")
        check_responses(outgoing, outside)
        print("Independent IPv4 packet replay passed WRITE, WRITEIMM, SACK, AETH, PETH, and NETH checks")


if __name__ == "__main__":
    main()
