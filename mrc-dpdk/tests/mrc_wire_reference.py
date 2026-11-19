"""Independent IPv4 wire helpers for the pcap and memif fixtures."""

import struct
import zlib
from typing import NamedTuple

from memif_pair import require


class IPv4Peer(NamedTuple):
    mac: bytes
    ip: bytes


class Bth(NamedTuple):
    opcode: int
    pkey: int
    qp: int
    psn: int
    flags: int = 0


class Received(NamedTuple):
    bth: Bth
    source_port: int
    udp_size: int
    body: bytes


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


def encode_ipv4(source, destination, source_port, udp_port, bth, body):
    pad = (-len(body)) % 4
    header = struct.pack("!BBHII", bth.opcode, pad << 4, bth.pkey, bth.qp,
                         bth.flags << 24 | bth.psn)
    transport = header + body + b"\0" * pad
    udp_size = 8 + len(transport) + 4
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + udp_size, 0, 0x4000,
                     63, 17, 0, source.ip, destination.ip)
    ip = ip[:10] + struct.pack("!H", ipv4_checksum(ip)) + ip[12:]
    protected = ip + struct.pack("!4H", source_port, udp_port, udp_size, 0) + transport
    return destination.mac + source.mac + b"\x08\x00" + protected + icrc(protected)


def decode_ipv4(frame, source, destination, udp_port):
    require(frame[:14] == destination.mac + source.mac + b"\x08\x00", "Unexpected Ethernet envelope")
    ip = frame[14:]
    require(len(ip) >= 44, "Truncated MRC frame")
    require(ip[0] == 0x45 and ip[9] == 17 and ip[12:20] == source.ip + destination.ip,
            "Unexpected IPv4 envelope")
    require(ipv4_checksum(ip[:20]) == 0 and int.from_bytes(ip[2:4], "big") == len(ip),
            "MRC frame has an invalid IPv4 checksum or length")
    source_port, port, udp_size, checksum = struct.unpack_from("!4H", ip, 20)
    require((port, udp_size, checksum) == (udp_port, len(ip) - 20, 0),
            "Unexpected UDP port, length, or checksum")
    require(ip[-4:] == icrc(ip[:-4]), "iCRC did not match the independent calculation")
    opcode, pad_version, pkey, qp, flags_psn = struct.unpack_from("!BBHII", ip, 28)
    pad = pad_version >> 4 & 3
    require(not pad_version & 0xF and len(ip) >= 44 + pad, "Invalid BTH version or padding")
    bth = Bth(opcode, pkey, qp & 0xFFFFFF, flags_psn & 0xFFFFFF, flags_psn >> 24)
    return Received(bth, source_port, udp_size, ip[40:len(ip) - 4 - pad])
