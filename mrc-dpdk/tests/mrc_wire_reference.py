"""Independent RoCEv2/MRC wire helpers for the pcap and memif fixtures."""

import struct
import zlib
from typing import NamedTuple

from memif_pair import require


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
    udp_size: int
    body: bytes
    flow_label: int = 0
    traffic_class: int = 0

    @property
    def entropy(self):
        return ((self.flow_label & 0xFFFF) << 16) | self.source_port


class Routed(NamedTuple):
    frame: bytes
    flow_label: int
    traffic_class: int


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


def ipv6_icrc(protected):
    masked = bytearray(protected)
    masked[0] |= 0x0F
    masked[1:4] = b"\xff" * 3
    for index in (7, 46, 47, 52):
        masked[index] = 0xFF
    return struct.pack("<I", zlib.crc32(b"\xff" * 8 + masked))


def encode_udp(source_port, udp_port, bth, body, original_udp_size):
    pad = (-len(body)) % 4
    header = struct.pack("!BBHII", bth.opcode, pad << 4, bth.pkey, bth.qp,
                         bth.flags << 24 | bth.psn)
    transport = header + body + b"\0" * pad
    udp_size = 8 + len(transport) + 4
    if original_udp_size is not None:
        require(bth.opcode == 0xDD and len(body) == 20 and body[2] in (1, 2) and
                udp_size <= original_udp_size <= 65535, "Invalid nominal length for TRIM NACK")
        udp_size = original_udp_size
    return struct.pack("!4H", source_port, udp_port, udp_size, 0) + transport


def decode_udp(datagram, udp_port, flow_label, traffic_class):
    require(len(datagram) >= 24, "Truncated MRC datagram")
    source_port, port, udp_size, checksum = struct.unpack_from("!4H", datagram)
    require(port == udp_port and checksum == 0, "Unexpected UDP destination port or checksum")
    opcode, pad_version, pkey, qp, flags_psn = struct.unpack_from("!BBHII", datagram, 8)
    pad = pad_version >> 4 & 3
    require(not pad_version & 0xF and len(datagram) >= 24 + pad, "Invalid BTH version or padding")
    body = datagram[20:len(datagram) - 4 - pad]
    # TRIM NACKs carry the original request's length in the UDP length field.
    trim_nack = opcode == 0xDD and len(body) == 20 and body[2] in (1, 2)
    require(udp_size >= 44 if trim_nack else udp_size == len(datagram),
            "Unexpected wire UDP length")
    bth = Bth(opcode, pkey, qp & 0xFFFFFF, flags_psn & 0xFFFFFF, flags_psn >> 24)
    return Received(bth, source_port, udp_size, body, flow_label, traffic_class)


def encode_ipv4(source, destination, source_port, udp_port, bth, body, *, original_udp_size=None):
    udp = encode_udp(source_port, udp_port, bth, body, original_udp_size)
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp) + 4, 0, 0x4000,
                     63, 17, 0, source.ip, destination.ip)
    ip = ip[:10] + struct.pack("!H", ipv4_checksum(ip)) + ip[12:]
    protected = ip + udp
    return destination.mac + source.mac + b"\x08\x00" + protected + ipv4_icrc(protected)


def decode_ipv4(frame, source, destination, udp_port):
    require(frame[:14] == destination.mac + source.mac + b"\x08\x00", "Unexpected Ethernet envelope")
    ip = frame[14:]
    require(len(ip) >= 44, "Truncated MRC frame")
    require(ip[0] == 0x45 and ip[9] == 17 and ip[12:20] == source.ip + destination.ip,
            "Unexpected IPv4 envelope")
    require(ipv4_checksum(ip[:20]) == 0 and int.from_bytes(ip[2:4], "big") == len(ip),
            "MRC frame has an invalid IPv4 checksum or length")
    require(ip[-4:] == ipv4_icrc(ip[:-4]), "iCRC did not match the independent calculation")
    return decode_udp(ip[20:], udp_port, 0, ip[1])


def ipv6_header(source, destination, payload_size, next_header, flow_label, traffic_class):
    require(len(source) == len(destination) == 16, "IPv6 addresses must have 16 octets")
    require(0 <= flow_label <= 0xFFFFF and 0 <= traffic_class <= 0xFF, "Invalid IPv6 flow label or traffic class")
    vtc_flow = 6 << 28 | traffic_class << 20 | flow_label
    return struct.pack("!IHBB16s16s", vtc_flow, payload_size, next_header, 63, source, destination)


def decode_ipv6_header(packet, source, destination, next_header):
    require(len(packet) >= 40, "Truncated IPv6 header")
    vtc_flow, payload_size, actual_next, hops, src, dst = struct.unpack_from("!IHBB16s16s", packet)
    require(vtc_flow >> 28 == 6 and hops > 0 and (actual_next, src, dst) == (next_header, source, destination),
            "Unexpected IPv6 envelope")
    require(payload_size == len(packet) - 40, "Invalid IPv6 payload length")
    return vtc_flow & 0xFFFFF, (vtc_flow >> 20) & 0xFF


def encode_ipv6(source, destination, source_port, udp_port, bth, body, *,
                flow_label=0, traffic_class=0, original_udp_size=None):
    udp = encode_udp(source_port, udp_port, bth, body, original_udp_size)
    ip = ipv6_header(source.ip, destination.ip, len(udp) + 4, 17, flow_label, traffic_class)
    protected = ip + udp
    return destination.mac + source.mac + b"\x86\xdd" + protected + ipv6_icrc(protected)


def decode_ipv6(frame, source, destination, udp_port):
    require(frame[:14] == destination.mac + source.mac + b"\x86\xdd", "Unexpected Ethernet envelope")
    ip = frame[14:]
    require(len(ip) >= 64, "Truncated MRC frame")
    flow_label, traffic_class = decode_ipv6_header(ip, source.ip, destination.ip, 17)
    require(ip[-4:] == ipv6_icrc(ip[:-4]), "iCRC did not match the independent calculation")
    return decode_udp(ip[40:], udp_port, flow_label, traffic_class)


def final_srh(destination_sid):
    # RFC 8754. Routing type 4, one IPv6 segment, Segments Left is zero.
    return struct.pack("!6BH", 41, 2, 4, 0, 0, 0, 0) + destination_sid


def wrap_srv6(frame, source_sid, destination_sid, *, srh=False, flow_label=0, traffic_class=0):
    require(frame[12:14] == b"\x86\xdd", "Fixture requires an inner IPv6 frame")
    payload = (final_srh(destination_sid) if srh else b"") + frame[14:]
    ip = ipv6_header(source_sid, destination_sid, len(payload), 43 if srh else 41,
                     flow_label, traffic_class)
    return frame[:14] + ip + payload


def unwrap_srv6(frame, source_sid, destination_sid, *, srh=False):
    require(frame[12:14] == b"\x86\xdd", "Unexpected outer Ethernet type")
    flow, tc = decode_ipv6_header(frame[14:], source_sid, destination_sid, 43 if srh else 41)
    payload = frame[54:]
    if srh:
        require(payload.startswith(final_srh(destination_sid)), "Unexpected final SRH segment list")
        payload = payload[24:]
    return Routed(frame[:14] + payload, flow, tc)
