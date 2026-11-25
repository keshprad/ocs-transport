"""Independent IPv4 RoCEv2 wire helpers for the memif peer."""

import struct
import zlib
from typing import NamedTuple


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class IPPeer(NamedTuple):
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
    body: bytes


def ipv4_checksum(header):
    value = sum(struct.unpack("!10H", header))
    while value >> 16:
        value = (value & 0xFFFF) + (value >> 16)
    return value ^ 0xFFFF


def ipv4_icrc(protected):
    # RoCEv2 invariant fields, including the all-ones masked LRH.
    masked = bytearray(protected)
    for index in (1, 8, 10, 11, 26, 27, 32):
        masked[index] = 0xFF
    return struct.pack("<I", zlib.crc32(b"\xff" * 8 + masked))


def encode_udp(source_port, udp_port, bth, body):
    pad = (-len(body)) % 4
    header = struct.pack("!BBHII", bth.opcode, pad << 4, bth.pkey, bth.qp,
                         bth.flags << 24 | bth.psn)
    transport = header + body + b"\0" * pad
    udp_size = 8 + len(transport) + 4
    return struct.pack("!4H", source_port, udp_port, udp_size, 0) + transport


def decode_udp(datagram, udp_port):
    require(len(datagram) >= 24, "Truncated RoCEv2 datagram")
    source_port, port, udp_size, checksum = struct.unpack_from("!4H", datagram)
    require(port == udp_port and checksum == 0, "Unexpected UDP destination port or checksum")
    opcode, pad_version, pkey, qp, flags_psn = struct.unpack_from("!BBHII", datagram, 8)
    pad = pad_version >> 4 & 3
    require(not pad_version & 0xF and len(datagram) >= 24 + pad, "Invalid BTH version or padding")
    body = datagram[20:len(datagram) - 4 - pad]
    require(udp_size == len(datagram), "Unexpected wire UDP length")
    bth = Bth(opcode, pkey, qp & 0xFFFFFF, flags_psn & 0xFFFFFF, flags_psn >> 24)
    return Received(bth, source_port, body)


def encode_ipv4(source, destination, source_port, udp_port, bth, body):
    udp = encode_udp(source_port, udp_port, bth, body)
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp) + 4, 0, 0x4000,
                     63, 17, 0, source.ip, destination.ip)
    ip = ip[:10] + struct.pack("!H", ipv4_checksum(ip)) + ip[12:]
    protected = ip + udp
    return destination.mac + source.mac + b"\x08\x00" + protected + ipv4_icrc(protected)


def decode_ipv4(frame, source, destination, udp_port):
    require(frame[:14] == destination.mac + source.mac + b"\x08\x00", "Unexpected Ethernet envelope")
    ip = frame[14:]
    require(len(ip) >= 44, "Truncated RoCEv2 frame")
    require(ip[0] == 0x45 and ip[9] == 17 and ip[12:20] == source.ip + destination.ip,
            "Unexpected IPv4 envelope")
    require(ipv4_checksum(ip[:20]) == 0 and int.from_bytes(ip[2:4], "big") == len(ip),
            "Invalid IPv4 checksum or length")
    require(ip[-4:] == ipv4_icrc(ip[:-4]), "iCRC did not match the independent calculation")
    return decode_udp(ip[20:], udp_port)
