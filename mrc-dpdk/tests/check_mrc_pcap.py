#!/usr/bin/env python3
"""Replay independent IPv4, IPv6, and preserved SRv6 MRC requests."""

import argparse
import ipaddress
import json
import os
import struct
import subprocess
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import NamedTuple, Optional

from memif_pair import require
from mrc_wire_reference import (Bth, IPPeer, decode_ipv4, decode_ipv6, encode_ipv4,
                                encode_ipv6, unwrap_srv6, wrap_srv6)

REQUESTOR = 0x120345
RESPONDER = 0x230456
PKEY = 0xAF31
INITIAL_PSN = 0xFFFFFF
ADDRESS = 0x123400000
RKEY = 0xDECAFE
UDP_PORT = 4971
TRIM_DSCP = 49
TRIM_TIMESTAMP = 0x4C12
RESERVED_OPCODE = 0xDF
PEER_MACS = [bytes.fromhex("0200000000a0"), bytes.fromhex("0200000000a1")]
PCAP_MACS = [bytes.fromhex("027063617000"), bytes.fromhex("027063617001")]
SOURCE_PORTS = [0xE241, 0xE3B2]
FLOW_LABELS = [0xAD2E4, 0x3B40F]
PEER_SID = ipaddress.IPv6Address("2001:db8:200::1").packed
LOCAL_SID = ipaddress.IPv6Address("2001:db8:200::2").packed
WRONG_SID = ipaddress.IPv6Address("2001:db8:200::ff").packed
PDC_IDS = (REQUESTOR & 0xFFFF, RESPONDER & 0xFFFF)


class Replay(NamedTuple):
    name: str
    peer_ip: str
    local_ip: str
    srh: Optional[bool] = None

    @property
    def ipv6(self):
        return ":" in self.local_ip

    def reflected_entropy(self, path):
        flow_bits = FLOW_LABELS[path] & 0xFFFF if self.ipv6 else 0
        return flow_bits << 16 | SOURCE_PORTS[path]


REPLAYS = (
    Replay("IPv4", "192.0.2.1", "192.0.2.2"),
    Replay("IPv6", "2001:db8:100::1", "2001:db8:100::2"),
    Replay("SRv6", "2001:db8:100::1", "2001:db8:100::2", srh=False),
    Replay("SRv6 with final SRH", "2001:db8:100::1", "2001:db8:100::2", srh=True),
)


def request(replay, path, opcode, psn, body, flags=0, *, ce=False, source_sid=PEER_SID,
            outer_traffic_class=None):
    source = IPPeer(PEER_MACS[path], ipaddress.ip_address(replay.peer_ip).packed)
    destination = IPPeer(PCAP_MACS[path], ipaddress.ip_address(replay.local_ip).packed)
    bth = Bth(opcode, PKEY, RESPONDER, psn, flags)
    if not replay.ipv6:
        return encode_ipv4(source, destination, SOURCE_PORTS[path], UDP_PORT, bth, body)
    frame = encode_ipv6(source, destination, SOURCE_PORTS[path], UDP_PORT, bth, body,
                        flow_label=FLOW_LABELS[path], traffic_class=3 if ce and replay.srh is None else 2)
    if replay.srh is None:
        return frame
    if outer_traffic_class is None:
        outer_traffic_class = 3 if ce else 2
    return wrap_srv6(frame, source_sid, LOCAL_SID, srh=replay.srh,
                     flow_label=0xD0300 + path, traffic_class=outer_traffic_class)


def write_request(replay, path, opcode, psn, msn, rqmsn, offset, length, payload,
                  immediate=None, flags=0x80, **envelope):
    headers = struct.pack("!HHQII", rqmsn, msn, ADDRESS + offset, RKEY, length)
    if immediate is not None:
        headers += struct.pack("!I", immediate)
    return request(replay, path, opcode, psn, headers + payload, flags, **envelope)


def trimmed_request(replay, data):
    headers = struct.pack("!HHHBBQII", 0, 1, TRIM_TIMESTAMP, 0x80, 1, ADDRESS, RKEY, len(data))
    full = request(replay, 0, 0xCA, INITIAL_PSN, headers + data, flags=0x90,
                   outer_traffic_class=TRIM_DSCP << 2)
    # The switch preserves the original declared lengths and removes data and iCRC.
    udp_offset = 14 + 40 + 24 + 40  # Ethernet, outer IPv6, single-segment SRH, inner IPv6
    original_udp_size = struct.unpack_from("!H", full, udp_offset + 4)[0]
    keep = udp_offset + 8 + 12 + 4 + 4  # UDP, BTH, METH, TSETH
    return full[:keep], original_udp_size


def captured_requests(replay):
    # Match the benchmark's application payload check without its wire codec.
    data = bytes((i ^ (i >> 8)) & 0xFF for i in range(13 + 1025))
    first = write_request(replay, 0, 0xCA, INITIAL_PSN, 1, 0, 0, 13, data[:13])
    corrupt = first[:-1] + bytes([first[-1] ^ 1])
    outside = (INITIAL_PSN + 1024) & 0xFFFFFF
    outside_write = write_request(replay, 0, 0xCA, outside, 1, 0, 0, 13, data[:13])
    tail_write = data[1037:]
    tail = write_request(replay, 0, 0xC9, 1, 2, 1, 1037, 1025, tail_write,
                         0xFEC01234, ce=replay.ipv6)
    duplicate = write_request(replay, 0, 0xC9, 1, 2, 1, 1037, 1025,
                              tail_write, 0xFEC01234, flags=0xA0)
    head = write_request(replay, 1, 0xC6, 0, 2, 1, 13, 1025, data[13:1037])
    # PETH token, PDC identifiers, and a type-1 timestamp extension.
    probe = request(replay, 0, 0xDE, 0, struct.pack("!7HBB", 0, 0, 0x91BC, 0, *PDC_IDS,
                                                  0x1731, 0, 1))

    rejected = [corrupt]
    if replay.srh is not None:
        rejected.append(write_request(replay, 0, 0xCA, INITIAL_PSN, 1, 0, 0, 13,
                                      data[:13], source_sid=WRONG_SID))
    if replay.srh:
        transit = bytearray(first)
        transit[14 + 40 + 3] = 1  # SRH Segments Left
        rejected.append(bytes(transit))
    rail = [outside_write, probe, *rejected]
    trim_size = None
    if replay.srh:
        trim, trim_size = trimmed_request(replay, data[:13])
        rail.append(trim)
    rail.extend((first, tail, duplicate))
    return (rail, [head]), outside, len(rejected), trim_size


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


def decode_response(replay, path, frame):
    source = IPPeer(PCAP_MACS[path], ipaddress.ip_address(replay.local_ip).packed)
    destination = IPPeer(PEER_MACS[path], ipaddress.ip_address(replay.peer_ip).packed)
    if replay.srh is not None:
        outer = unwrap_srv6(frame, LOCAL_SID, PEER_SID, srh=replay.srh)
        frame = outer.frame
    decoder = decode_ipv6 if replay.ipv6 else decode_ipv4
    packet = decoder(frame, source, destination, UDP_PORT)
    if replay.srh is not None:
        require(outer.traffic_class == packet.traffic_class,
                "Reply did not carry its traffic class through the SRv6 envelope")
    bth = packet.bth
    require(bth.pkey == PKEY and bth.qp == (2 if bth.opcode in (0xD8, 0xD9) else REQUESTOR),
            "Response used the wrong partition or destination QP")
    return packet


def check_responses(replay, output_paths, outside, trim_size):
    responses = [[decode_response(replay, path, frame) for frame in read_capture(output)]
                 for path, output in enumerate(output_paths)]
    aeth = [[(packet.bth.psn, packet.body) for packet in rail if packet.bth.opcode == 0xD1]
            for rail in responses]
    require(aeth[0] and all(body == b"\x1f\x00\x00\x01" and psn == INITIAL_PSN
                            for psn, body in aeth[0]), "Transport ACK completed WR 2 before its first packet")
    require((1, b"\x1f\x00\x00\x02") in aeth[1], "Final transport ACK did not complete MSN 2")

    sacks = [(packet.bth.psn, packet.bth.flags, packet.body)
             for packet in responses[0] if packet.bth.opcode == 0xDC]
    require(any(body[1] & 2 and struct.unpack_from("!H", body, 2)[0] == 0x91BC and
                struct.unpack_from("!H", body, 28)[0] == 0x1731 for _, _, body in sacks),
            "Probe response did not echo its token and timestamp")
    require(any(psn == INITIAL_PSN and not body[1] & 2 and
                struct.unpack_from("!h", body, 2)[0] == 2 and
                struct.unpack_from("!Q", body, 20)[0] and
                struct.unpack_from("!H", body, 30)[0] == 1 for psn, _, body in sacks),
            "Selective ACK did not report the segment beyond the gap")
    if replay.ipv6:
        require(any(psn == INITIAL_PSN and not body[1] & 2 and ((body[1] >> 5) & 3) == 1 and
                    struct.unpack_from("!h", body, 2)[0] == 2 for psn, _, body in sacks),
                "CE on the reordered WRITE did not produce a skip mark")
    require(any(flags & 0x20 and psn == INITIAL_PSN for psn, flags, _ in sacks),
            "Selective ACK did not reflect the retransmission flag")
    nacks = [packet for packet in responses[0] if packet.bth.opcode == 0xDD]
    by_reason = {packet.body[2]: packet for packet in nacks}
    expected_reasons = {0x01, 0x0B} if trim_size is not None else {0x0B}
    require(len(nacks) == len(expected_reasons) and by_reason.keys() == expected_reasons,
            "Unexpected reliability NACKs")
    window = by_reason[0x0B]
    require(window.bth.psn == outside and int.from_bytes(window.body[13:16], "big") == outside,
            "Out-of-window NACK did not reflect the request PSN")
    if trim_size is not None:
        trimmed = by_reason[0x01]
        require(trimmed.bth.psn == INITIAL_PSN and trimmed.udp_size == trim_size and
                int.from_bytes(trimmed.body[13:16], "big") == INITIAL_PSN and
                struct.unpack_from("!H", trimmed.body, 18)[0] == TRIM_TIMESTAMP,
                "Trim NACK did not reflect the request PSN, original UDP size, and timestamp")
    require(any(packet.bth.opcode == 0xDC and packet.bth.psn == 1 and packet.body[17] == 0 and
                struct.unpack_from("!H", packet.body, 30)[0] == 0 for packet in responses[1]),
            "Final SACK did not close the receive gap")
    for path, rail in enumerate(responses):
        for packet in rail:
            if packet.bth.opcode in (0xDC, 0xDD):
                require(struct.unpack_from("!IHH", packet.body, 4) == (replay.reflected_entropy(path), *PDC_IDS),
                        f"Reliability response on path {path} did not reflect entropy and PDC IDs")


@contextmanager
def run_capture(binary, replay, requests, *, trim=False):
    with tempfile.TemporaryDirectory(prefix="mrc-pcap-") as temp:
        directory = Path(temp)
        incoming = [directory / f"rx{path}.pcap" for path in range(2)]
        outgoing = [directory / f"tx{path}.pcap" for path in range(2)]
        for capture, frames in zip(incoming, requests):
            write_capture(capture, frames)
        core = min(os.sched_getaffinity(0))
        command = [str(binary), f"--lcores=0@{core}", "--no-huge", "--no-pci", "--no-telemetry",
                   f"--file-prefix={directory.name}", "-m", "64"]
        for path in range(2):
            command.append(f"--vdev=net_pcap{path},rx_pcap={incoming[path]},tx_pcap={outgoing[path]}")
        command += ["--", "rx", "13,1025@0xfec01234", replay.local_ip, replay.peer_ip,
                    "0", PEER_MACS[0].hex(":"), "1", PEER_MACS[1].hex(":"),
                    "--initial-psn", str(INITIAL_PSN), "--requestor-qpn", hex(REQUESTOR),
                    "--responder-qpn", hex(RESPONDER), "--p-key", hex(PKEY),
                    "--remote-address", hex(ADDRESS), "--remote-key", hex(RKEY),
                    "--port-stats", "1", "--timeout-ms", "2000"]
        if trim:
            command += ["--trim-dscp", str(TRIM_DSCP), "--trim-nack", "1"]
        if replay.srh is not None:
            command += ["--srv6-local", str(ipaddress.IPv6Address(LOCAL_SID)),
                        "--srv6-peer", str(ipaddress.IPv6Address(PEER_SID))]
            if replay.srh:
                command += ["--srv6-srh", "1"]
        result = subprocess.run(command, env={**os.environ, "XDG_RUNTIME_DIR": str(directory)},
                                capture_output=True, text=True, timeout=8)
        yield result, outgoing


def run_replay(binary, replay):
    requests, outside, rejected, trim_size = captured_requests(replay)
    with run_capture(binary, replay, requests, trim=trim_size is not None) as (result, outgoing):
        require(result.returncode == 0, f"pcap endpoint failed\n{result.stdout}\n{result.stderr}")
        counters = json.loads(result.stdout.strip().splitlines()[-1])
        trimmed = int(trim_size is not None)
        require(counters["bytes"] == 1038 and counters["received"] == [2, 1] and
                counters["duplicates"] == 1 and counters["immediate_completions"] == 1 and
                counters["trimmed"] == counters["trim_nacks"] == trimmed and
                counters["nack_sent_mask"] == ((1 << 0x0B) | (trimmed << 1)) and
                f"decode-rejected={rejected} other-peer=0" in result.stderr,
                f"Unexpected {replay.name} placement or error counters: {counters}\n{result.stderr}")
        check_responses(replay, outgoing, outside, trim_size)
        print(f"Independent {replay.name} packet replay passed WRITE, WRITEIMM, SACK, AETH, PETH, and NETH checks")


def check_reserved_opcode(binary, replay):
    # Give the corrupt iCRC, wrong outer peer, and actual fault distinct PSNs.
    bad_integrity = request(replay, 0, RESERVED_OPCODE, INITIAL_PSN, b"")
    requests = [bad_integrity[:-1] + bytes([bad_integrity[-1] ^ 1])]
    if replay.srh is not None:
        requests.append(request(replay, 0, RESERVED_OPCODE, 0, b"", source_sid=WRONG_SID))
    accepted_psn = 1
    requests.append(request(replay, 0, RESERVED_OPCODE, accepted_psn, b""))
    # A later valid WRITE must not be placed after the invalid request faults the QP.
    requests.append(write_request(replay, 0, 0xCA, INITIAL_PSN, 1, 0, 0, 13, bytes(range(13))))

    with run_capture(binary, replay, (requests, ())) as (result, outgoing):
        require(result.returncode == 1 and "MRC QP state=ERROR" in result.stderr and
                "MRC WRITE failed: Protocol error" in result.stderr,
                f"The {replay.name} responder did not fault its QP on the reserved opcode\n"
                f"{result.stdout}\n{result.stderr}")
        replies = [
            (path, packet)
            for path, capture in enumerate(outgoing)
            for frame in read_capture(capture)
            if (packet := decode_response(replay, path, frame)).bth.opcode not in (0xD8, 0xD9)
        ]
        require(len(replies) == 1, f"Unexpected {replay.name} feedback after the invalid request: {replies}")
        path, packet = replies[0]
        require(path == 0 and packet.bth.opcode == 0xD1 and packet.bth.psn == accepted_psn and
                len(packet.body) == 4 and packet.body[0] == 0x61,
                f"The {replay.name} responder did not return Invalid Request for PSN {accepted_psn}: {packet}")
        print(f"Independent {replay.name} reserved opcode replay produced Invalid Request and QP ERROR")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path(__file__).resolve().parents[1] / "mrc_write")
    binary = parser.parse_args().binary.resolve()
    require(binary.is_file(), f"Build mrc_write first: {binary}")
    for replay in REPLAYS:
        run_replay(binary, replay)
        check_reserved_opcode(binary, replay)


if __name__ == "__main__":
    main()
