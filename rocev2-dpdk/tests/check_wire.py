#!/usr/bin/env python3
"""Independent Ethernet, IP, UDP, BTH, RETH, AETH and ICRC vectors."""

import binascii
import ipaddress
import struct
import subprocess
from pathlib import Path

CLI = Path(__file__).with_name("roce_wire_cli")
PAYLOAD = bytes.fromhex("2435465768")
OPCODES = (0x0A, 0x06, 0x07, 0x08, 0x11)


def checksum(header):
    values = struct.unpack(f"!{len(header) // 2}H", header)
    total = sum(values)
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return ~total & 0xFFFF


def fixture(opcode, *, tos=0, ttl=64):
    write = opcode != 0x11
    data = PAYLOAD if write else b""
    pad = (-len(data)) % 4
    bth = struct.pack(
        "!BBHII", opcode, pad << 4, 0xFFFF, 0x123456, (int(write) << 31) | 0xABCDEF
    )
    ext = b""
    if opcode in (0x0A, 0x06):
        ext = struct.pack("!QII", 0xFEDCBA9876543210, 0x10A0B0C0, 5)
    elif opcode == 0x11:
        ext = struct.pack("!I", 0x60C0FFEE)
    tail = ext + data + bytes(pad)
    udp_size = 8 + len(bth) + len(tail) + 4
    udp = struct.pack("!4H", 0xC123, 4791, udp_size, 0)
    ip = bytearray(
        struct.pack("!BBHHHBBH", 0x45, tos, udp_size + 20, 0, 0x4000, ttl, 17, 0)
        + ipaddress.ip_address("192.0.2.1").packed
        + ipaddress.ip_address("192.0.2.2").packed
    )
    struct.pack_into("!H", ip, 10, checksum(ip))
    invariant = bytearray(ip + udp + bth + tail)
    for offset in (1, 8, 10, 11, 26, 27, 32):
        invariant[offset] = 0xFF
    crc = struct.pack("<I", binascii.crc32(b"\xff" * 8 + invariant))
    return bytes.fromhex("0200000000020200000000010800") + ip + udp + bth + tail + crc


def decode(frame):
    return subprocess.run([CLI, "decode"], input=frame, capture_output=True, check=False)


def main():
    for index, opcode in enumerate(OPCODES):
        expected = fixture(opcode)
        result = subprocess.check_output([CLI, str(index)])
        assert result == expected, (hex(opcode), result.hex(), expected.hex())
        header = (opcode, 0x123456, 0xABCDEF, int(opcode != 0x11))
        reth = (0xFEDCBA9876543210, 0x10A0B0C0, 5) if opcode in (0x0A, 0x06) else (0, 0, 0)
        aeth = (0x60, 0xC0FFEE, 0) if opcode == 0x11 else (0, 0, len(PAYLOAD))
        for frame in (expected, fixture(opcode, tos=0x2B, ttl=31), expected + bytes(8)):
            result = decode(frame)
            assert result.returncode == 0 and tuple(map(int, result.stdout.split())) == header + reth + aeth
        for length in range(len(expected)):
            assert decode(expected[:length]).returncode == 1, (hex(opcode), length)
        for offset in (14, 16, 20, 23, 24, 36, 39, 42, len(expected) - 1):
            damaged = bytearray(expected)
            damaged[offset] ^= 1
            assert decode(damaged).returncode == 1, (hex(opcode), offset)
    print("independent RoCEv2 frame vectors passed")


if __name__ == "__main__":
    main()
