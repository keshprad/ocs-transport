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
                                encode_ipv6, ipv4_checksum, unwrap_srv6, wrap_srv6)

REQUESTOR = 0x120345
RESPONDER = 0x230456
PKEY = 0xAF31
INITIAL_PSN = 0xFFFFFF
ADDRESS = 0x123400000
RKEY = 0xDECAFE
UDP_PORT = 4971
TRIM_DSCP = 49
TRIM_TIMESTAMP = 0x4C12
TRIM_LAST_HOP_DSCP = 51
TRIM_LAST_HOP_TIMESTAMP = 0x7A5C
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
            traffic_class=None, outer_traffic_class=None, dqp=RESPONDER):
    source = IPPeer(PEER_MACS[path], ipaddress.ip_address(replay.peer_ip).packed)
    destination = IPPeer(PCAP_MACS[path], ipaddress.ip_address(replay.local_ip).packed)
    bth = Bth(opcode, PKEY, dqp, psn, flags)
    if not replay.ipv6:
        frame = encode_ipv4(source, destination, SOURCE_PORTS[path], UDP_PORT, bth, body)
        if traffic_class is None:
            return frame
        # The RoCE invariant CRC masks IPv4 traffic class and header checksum.
        marked = bytearray(frame)
        marked[15] = traffic_class
        marked[24:26] = bytes(2)
        struct.pack_into("!H", marked, 24, ipv4_checksum(marked[14:34]))
        return bytes(marked)
    if traffic_class is None:
        traffic_class = 3 if ce and replay.srh is None else 2
    frame = encode_ipv6(source, destination, SOURCE_PORTS[path], UDP_PORT, bth, body,
                        flow_label=FLOW_LABELS[path], traffic_class=traffic_class)
    if replay.srh is None:
        return frame
    if outer_traffic_class is None:
        outer_traffic_class = 3 if ce else 2
    return wrap_srv6(frame, source_sid, LOCAL_SID, srh=replay.srh,
                     flow_label=0xD0300 + path, traffic_class=outer_traffic_class)


def timestamp_extension(timestamp, opaque):
    if timestamp is None:
        return bytes(4)
    return struct.pack("!HBB", timestamp, 0x80 if opaque else 0, 1)


def write_request(replay, path, opcode, psn, msn, rqmsn, offset, length, payload,
                  immediate=None, flags=0x80, *, timestamp=None, opaque=False, **envelope):
    headers = struct.pack("!HH", rqmsn, msn)
    if timestamp is not None:
        headers += timestamp_extension(timestamp, opaque)
        flags |= 0x10
    headers += struct.pack("!QII", ADDRESS + offset, RKEY, length)
    if immediate is not None:
        headers += struct.pack("!I", immediate)
    return request(replay, path, opcode, psn, headers + payload, flags, **envelope)


def trimmed_request(replay, data, *, timestamp=TRIM_TIMESTAMP, opaque=True,
                    dscp=TRIM_DSCP, truncate=True, flags=0x80):
    traffic_class = dscp << 2
    full = write_request(replay, 0, 0xCA, INITIAL_PSN, 1, 0, 0, len(data), data,
                         timestamp=timestamp, opaque=opaque, flags=flags,
                         traffic_class=traffic_class if replay.srh is None else None,
                         outer_traffic_class=traffic_class if replay.srh is not None else None)
    # The switch preserves the original declared lengths and removes data and iCRC.
    udp_offset = 14 + (40 if replay.ipv6 else 20)
    if replay.srh is not None:
        udp_offset += 40 + (24 if replay.srh else 0)
    original_udp_size = struct.unpack_from("!H", full, udp_offset + 4)[0]
    keep = udp_offset + 8 + 12 + 4 + 4  # UDP, BTH, METH, TSETH
    return (full[:keep] if truncate else full), original_udp_size


def captured_requests(replay):
    # Match the benchmark's application payload check without its wire codec.
    data = bytes((i ^ (i >> 8)) & 0xFF for i in range(13 + 1025))
    first = write_request(replay, 0, 0xCA, INITIAL_PSN, 1, 0, 0, 13, data[:13], flags=0xA0)
    corrupt = first[:-1] + bytes([first[-1] ^ 1])
    outside = (INITIAL_PSN + 1024) & 0xFFFFFF
    outside_write = write_request(replay, 0, 0xCA, outside, 1, 0, 0, 13, data[:13])
    tail_write = data[1037:]
    tail = write_request(replay, 0, 0xC9, 1, 2, 1, 1037, 1025, tail_write,
                         0xFEC01234, ce=replay.ipv6)
    duplicate = write_request(replay, 0, 0xC9, 1, 2, 1, 1037, 1025,
                              tail_write, 0xFEC01234, flags=0xA0)
    head = write_request(replay, 1, 0xC6, 0, 2, 1, 13, 1025, data[13:1037])
    # Only the PETH token identifies the probe. The BTH PSN is reserved.
    probe = request(replay, 0, 0xDE, 0x7FED03, struct.pack("!7HBB", 0, 0, 0x91BC, 0, *PDC_IDS,
                                                         0x1731, 0, 1))
    wrong_context = request(replay, 0, 0xDE, 0, struct.pack("!7HBB", 0, 0, 0x92CD, 0,
                                                           PDC_IDS[0], PDC_IDS[1] ^ 1, 0, 0, 0))

    rejected = [corrupt]
    if replay.srh is not None:
        rejected.append(write_request(replay, 0, 0xCA, INITIAL_PSN, 1, 0, 0, 13,
                                      data[:13], source_sid=WRONG_SID))
    if replay.srh:
        transit = bytearray(first)
        transit[14 + 40 + 3] = 1  # SRH Segments Left
        rejected.append(bytes(transit))
    trim, trim_size = trimmed_request(replay, data[:13])
    # The intact marked WRITE has valid headers and iCRC. A probe checks that
    # neither trim advanced delivery before the real WRITE places its data.
    wrong_data = bytes(value ^ 0xFF for value in data[:13])
    last_hop, last_hop_size = trimmed_request(replay, wrong_data, timestamp=TRIM_LAST_HOP_TIMESTAMP,
                                              dscp=TRIM_LAST_HOP_DSCP, truncate=False, flags=0xA0)
    rail = [outside_write, wrong_context, *rejected, trim, last_hop, probe, first, tail, duplicate]
    return (rail, [head]), outside, len(rejected), (trim_size, last_hop_size)


def service_time_requests(replay):
    data = bytes((i ^ (i >> 8)) & 0xFF for i in range(13 + 1025))
    standalone = (None, False)
    compensated = (0xFFFF, False)
    opaque = (0x923B, True)
    rail, expected = [], {}

    # Start with opaque timestamps so the positive samples follow earlier feedback
    # preparation in the same receive burst, even on a fast idle core.
    for index, timing in enumerate((opaque, standalone, compensated)):
        timestamp, is_opaque = timing
        extension = timestamp_extension(timestamp, is_opaque)
        endpoint_id = 0x7710 + index
        erth = struct.pack("!III", 0, 3, 0) + extension
        rail.append(request(replay, 0, 0xD8, 0x900000 | endpoint_id, erth, dqp=2))
        expected["EETH", endpoint_id] = timing

        probe_id = 0x7620 + index
        peth = struct.pack("!6H", 0, 0, probe_id, 0, *PDC_IDS) + extension
        rail.append(request(replay, 0, 0xDE, 0x7FED03, peth))
        expected["probe SACK", probe_id] = timing

        outside = (INITIAL_PSN + 1024 + index) & 0xFFFFFF
        rail.append(write_request(replay, 0, 0xCA, outside, 1, 0, 0, 13, data[:13],
                                  timestamp=timestamp, opaque=is_opaque))
        expected["NETH", 0x0B, outside] = timing

    trim_size = None
    if replay.srh:
        trim, trim_size = trimmed_request(replay, data[:13], timestamp=compensated[0], opaque=False)
        rail.append(trim)
        expected["NETH", 0x01, INITIAL_PSN] = compensated

    rail.append(write_request(replay, 0, 0xCA, INITIAL_PSN, 1, 0, 0, 13, data[:13]))
    expected["WRITE SACK", INITIAL_PSN, False] = standalone
    rail.append(write_request(replay, 0, 0xC9, 1, 2, 1, 1037, 1025, data[1037:],
                              0xFEC01234, timestamp=compensated[0]))
    expected["WRITE SACK", 1, False] = compensated
    rail.append(write_request(replay, 0, 0xC9, 1, 2, 1, 1037, 1025, data[1037:],
                              0xFEC01234, flags=0xA0, timestamp=opaque[0], opaque=True))
    expected["WRITE SACK", 1, True] = opaque
    head = write_request(replay, 1, 0xC6, 0, 2, 1, 13, 1025, data[13:1037])
    return (rail, [head]), expected, trim_size


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


def check_responses(replay, output_paths, outside, trim_sizes):
    responses = [[decode_response(replay, path, frame) for frame in read_capture(output)]
                 for path, output in enumerate(output_paths)]
    aeth = [[(packet.bth.psn, packet.body) for packet in rail if packet.bth.opcode == 0xD1]
            for rail in responses]
    require(aeth == [[(INITIAL_PSN, b"\x1f\x00\x00\x01")] * 2, [(1, b"\x1f\x00\x00\x02")]],
            f"Unexpected semantic ACKs after two trims and a duplicate: {aeth}")

    sacks = [(packet.bth.psn, packet.bth.flags, packet.body)
             for packet in responses[0] if packet.bth.opcode == 0xDC]
    probes = [(psn, body) for psn, _, body in sacks if body[1] & 2]
    require(len(probes) == 1, "Expected a reply only to the probe with matching PDC identifiers")
    probe_psn, probe = probes[0]
    require(probe_psn == 0 and struct.unpack_from("!H", probe, 2)[0] == 0x91BC and
            struct.unpack_from("!H", probe, 28)[0] == 0x1731 and
            int.from_bytes(probe[13:16], "big") == ((INITIAL_PSN - 1) & 0xFFFFFF) and
            int.from_bytes(probe[33:36], "big") == 0,
            "Probe response did not echo PETH or changed the initial packet delivery state")
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
    expected_reasons = {0x01, 0x02, 0x0B}
    require(len(nacks) == len(expected_reasons) and by_reason.keys() == expected_reasons,
            "Unexpected reliability NACKs")
    window = by_reason[0x0B]
    require(window.bth.psn == outside and int.from_bytes(window.body[13:16], "big") == outside,
            "Out-of-window NACK did not reflect the request PSN")
    for reason, size, timestamp, rtx in (
        (0x01, trim_sizes[0], TRIM_TIMESTAMP, False),
        (0x02, trim_sizes[1], TRIM_LAST_HOP_TIMESTAMP, True),
    ):
        trimmed = by_reason[reason]
        require(trimmed.bth.psn == INITIAL_PSN and trimmed.udp_size == size and
                bool(trimmed.bth.flags & 0x20) == rtx and
                int.from_bytes(trimmed.body[13:16], "big") == INITIAL_PSN and
                struct.unpack_from("!H", trimmed.body, 18)[0] == timestamp,
                f"Trim NACK {reason:#x} did not reflect the PSN, RTX flag, original UDP size, and timestamp")
    require(any(packet.bth.opcode == 0xDC and packet.bth.psn == 1 and packet.body[17] == 0 and
                struct.unpack_from("!H", packet.body, 30)[0] == 0 for packet in responses[1]),
            "Final SACK did not close the receive gap")
    for path, rail in enumerate(responses):
        for packet in rail:
            if packet.bth.opcode in (0xDC, 0xDD):
                require(struct.unpack_from("!IHH", packet.body, 4) == (replay.reflected_entropy(path), *PDC_IDS),
                        f"Reliability response on path {path} did not reflect entropy and PDC IDs")


@contextmanager
def run_capture(binary, replay, requests, *, role="rx", work="13,1025@0xfec01234", pmtu=None,
                trim=False, last_hop=False, service_time=False):
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
        command += ["--", role, work, replay.local_ip, replay.peer_ip,
                    "0", PEER_MACS[0].hex(":"), "1", PEER_MACS[1].hex(":"),
                    "--initial-psn", str(INITIAL_PSN), "--requestor-qpn", hex(REQUESTOR),
                    "--responder-qpn", hex(RESPONDER), "--p-key", hex(PKEY),
                    "--remote-address", hex(ADDRESS), "--remote-key", hex(RKEY),
                    "--port-stats", "1", "--timeout-ms", "2000"]
        if pmtu is not None:
            command += ["--pmtu", str(pmtu)]
        if role == "tx":
            command += ["--linear-retries", "0", "--exp-retries", "0"]
        if trim:
            command += ["--trim-dscp", str(TRIM_DSCP), "--trim-nack", "1"]
        if last_hop:
            command += ["--trim-last-hop-dscp", str(TRIM_LAST_HOP_DSCP)]
        if service_time:
            command += ["--service-time", "1"]
        if replay.srh is not None:
            command += ["--srv6-local", str(ipaddress.IPv6Address(LOCAL_SID)),
                        "--srv6-peer", str(ipaddress.IPv6Address(PEER_SID))]
            if replay.srh:
                command += ["--srv6-srh", "1"]
        result = subprocess.run(command, env={**os.environ, "XDG_RUNTIME_DIR": str(directory)},
                                capture_output=True, text=True, timeout=8)
        yield result, outgoing


def run_replay(binary, replay):
    requests, outside, rejected, trim_sizes = captured_requests(replay)
    with run_capture(binary, replay, requests, trim=True, last_hop=True) as (result, outgoing):
        require(result.returncode == 0, f"pcap endpoint failed\n{result.stdout}\n{result.stderr}")
        counters = json.loads(result.stdout.strip().splitlines()[-1])
        require(counters["bytes"] == 1038 and counters["received"] == [2, 1] and
                counters["duplicates"] == 1 and counters["immediate_completions"] == 1 and
                counters["probe_requests"] == 1 and
                counters["trimmed"] == counters["trim_nacks"] == 2 and
                counters["nack_sent_mask"] == (1 << 0x0B | 1 << 0x02 | 1 << 0x01) and
                f"decode-rejected={rejected} other-peer=0" in result.stderr,
                f"Unexpected {replay.name} placement or error counters: {counters}\n{result.stderr}")
        check_responses(replay, outgoing, outside, trim_sizes)
        print(f"Independent {replay.name} packet replay passed placement, SACK, AETH, PETH, and both trim NACKs")


def check_retransmitted_ack_request(binary, replay):
    data = bytes((offset ^ (offset >> 8)) & 0xFF for offset in range(2049))
    first = write_request(replay, 0, 0xC6, INITIAL_PSN, 1, 1, 0, len(data),
                          data[:1024], flags=0)
    middle = write_request(replay, 0, 0xC7, 0, 1, 1, 1024, len(data),
                           data[1024:2048], flags=0)
    middle_retry = write_request(replay, 0, 0xC7, 0, 1, 1, 1024, len(data),
                                 data[1024:2048], flags=0xA0)
    tail = write_request(replay, 0, 0xC9, 1, 1, 1, 2048, len(data),
                         data[2048:], 0xFEC01234)
    # One capture keeps the order fixed. The unmarked first segment fills
    # the gap before the tail arrives to trigger write completion.
    requests = ([middle, middle_retry, first, tail], [])
    with run_capture(binary, replay, requests, work="2049@0xfec01234") as (result, outgoing):
        require(result.returncode == 0, f"pcap endpoint failed\n{result.stdout}\n{result.stderr}")
        counters = json.loads(result.stdout.strip().splitlines()[-1])
        require(counters["received"] == [3, 0] and counters["duplicates"] == 1
                and counters["immediate_completions"] == 1,
                f"Unexpected {replay.name} placement or completion: {counters}")
        sacks = [(packet.bth.psn, struct.unpack_from("!h", packet.body, 2)[0])
                 for frame in read_capture(outgoing[0])
                 if (packet := decode_response(replay, 0, frame)).bth.opcode == 0xDC]
        require(sacks == [(INITIAL_PSN - 1, 2), (0, -1), (1, 0)],
                f"Expected a cumulative SACK after filling the gap before the AR PSN: {sacks}")
        print(f"Independent {replay.name} replay preserved the retransmitted ACK request")


def check_jumbo_pmtu(binary, replay):
    pmtu = 4096
    data = bytes((offset ^ (offset >> 8)) & 0xFF for offset in range(2 * pmtu + 33))
    first = write_request(replay, 0, 0xC6, INITIAL_PSN, 1, 1, 0, len(data),
                          data[:pmtu], flags=0)
    middle = write_request(replay, 0, 0xC7, 0, 1, 1, pmtu, len(data), data[pmtu:2 * pmtu])
    tail = write_request(replay, 0, 0xC9, 1, 1, 1, 2 * pmtu, len(data),
                         data[2 * pmtu:], 0xFEC01234)
    # Keep capture order fixed so the responder stages the middle before
    # the tail anchors the write. The first segment closes the PSN gap.
    requests = ([middle, tail, first], [])
    with run_capture(binary, replay, requests, work=f"{len(data)}@0xfec01234", pmtu=pmtu) as (result, outgoing):
        # A successful standalone receive checks every byte of the reassembled write.
        require(result.returncode == 0, f"Jumbo pcap endpoint failed\n{result.stdout}\n{result.stderr}")
        counters = json.loads(result.stdout.strip().splitlines()[-1])
        require(counters["bytes"] == len(data) and counters["received"] == [3, 0]
                and counters["immediate_completions"] == 1,
                f"Unexpected {replay.name} jumbo placement or completion: {counters}")
        responses = [decode_response(replay, 0, frame) for frame in read_capture(outgoing[0])]
        sacks = [(packet.bth.psn, struct.unpack_from("!h", packet.body, 2)[0],
                  struct.unpack_from("!H", packet.body, 30)[0])
                 for packet in responses if packet.bth.opcode == 0xDC]
        require(sacks == [(INITIAL_PSN - 1, 2, 1), (INITIAL_PSN - 1, 3, 2), (1, -2, 0)],
                f"Unexpected {replay.name} jumbo SACK sequence: {sacks}")
        aeth = [(packet.bth.psn, packet.body) for packet in responses if packet.bth.opcode == 0xD1]
        require(aeth == [(1, b"\x1f\x00\x00\x01")],
                f"Unexpected {replay.name} jumbo transport ACK: {aeth}")
        print(f"Independent {replay.name} 4096-byte PMTU replay passed reordered WRITEIMM placement")


def check_jumbo_requester(binary, pmtu):
    replay = REPLAYS[0]
    length = 2 * pmtu + 33
    immediate = 0xFEC01234
    with run_capture(binary, replay, ([], []), role="tx",
                     work=f"{length}@{immediate:#x}", pmtu=pmtu) as (result, outgoing):
        require(result.returncode == 1 and "Retry limit exhausted for PSN" in result.stderr,
                f"Jumbo requester did not reach its retry limit\n{result.stdout}\n{result.stderr}")
        fragments = {}
        for path, output in enumerate(outgoing):
            source = IPPeer(PCAP_MACS[path], ipaddress.ip_address(replay.local_ip).packed)
            destination = IPPeer(PEER_MACS[path], ipaddress.ip_address(replay.peer_ip).packed)
            for frame in read_capture(output):
                packet = decode_ipv4(frame, source, destination, UDP_PORT)
                if 0xC6 <= packet.bth.opcode <= 0xCB:
                    require(packet.bth.psn not in fragments and not packet.bth.flags & 0x20,
                            f"Unexpected retransmission of PSN {packet.bth.psn:#x}")
                    fragments[packet.bth.psn] = packet
        psns = (INITIAL_PSN, 0, 1)
        require(fragments.keys() == set(psns), f"Unexpected jumbo WRITE PSNs: {list(fragments)}")
        for index, (psn, opcode) in enumerate(zip(psns, (0xC6, 0xC7, 0xC9))):
            packet = fragments[psn]
            require((packet.bth.opcode, packet.bth.pkey, packet.bth.qp) == (opcode, PKEY, RESPONDER),
                    f"Wrong jumbo WRITE opcode, P_Key, or QP at PSN {psn:#x}")
            offset = index * pmtu
            headers = struct.pack("!HHQII", 1, 1, ADDRESS + offset, RKEY, length)
            if opcode == 0xC9:
                headers += struct.pack("!I", immediate)
            data = bytes((i ^ (i >> 8)) & 0xFF for i in range(offset, min(offset + pmtu, length)))
            require(packet.body == headers + data, f"Wrong jumbo WRITE geometry or payload at PSN {psn:#x}")
        print(f"Independent IPv4 requester capture passed WRITEIMM wire geometry at {pmtu}-byte PMTU")


def check_service_time(binary, replay):
    requests, expected, trim_size = service_time_requests(replay)
    with run_capture(binary, replay, requests, trim=trim_size is not None,
                     service_time=True) as (result, outgoing):
        require(result.returncode == 0, f"pcap service-time endpoint failed\n{result.stdout}\n{result.stderr}")
        counters = json.loads(result.stdout.strip().splitlines()[-1])
        require(counters["bytes"] == 1038 and counters["received"] == [2, 1]
                and counters["duplicates"] == 1 and counters["immediate_completions"] == 1
                and counters["probe_requests"] == 3 and counters["port_status_updates"] == 3,
                f"Unexpected {replay.name} service-time placement or controls: {counters}")

        actual = {}
        for frame in read_capture(outgoing[0]):
            packet = decode_response(replay, 0, frame)
            body = packet.body
            if packet.bth.opcode == 0xDC:
                require(len(body) == 36, "Wrong service-time SACK length")
                if body[1] & 2:
                    key = ("probe SACK", struct.unpack_from("!H", body, 2)[0])
                else:
                    offset, = struct.unpack_from("!h", body, 2)
                    trigger_psn = (packet.bth.psn + offset) & 0xFFFFFF
                    key = ("WRITE SACK", trigger_psn, bool(packet.bth.flags & 0x20))
                field = 28
            elif packet.bth.opcode == 0xDD:
                require(len(body) == 20 and int.from_bytes(body[13:16], "big") == packet.bth.psn,
                        "Service-time NETH did not reflect the request PSN")
                key = ("NETH", body[2], packet.bth.psn)
                if body[2] == 0x01:
                    require(packet.udp_size == trim_size, "Service-time trim NACK changed the original UDP length")
                field = 18
            elif packet.bth.opcode == 0xD9:
                require(len(body) == 36 and (body[1] >> 2) & 3 == 0,
                        "Unexpected service-time EETH format")
                key = ("EETH", packet.bth.psn)
                field = 28
            else:
                continue
            require(key not in actual, f"Duplicate service-time feedback {key}")
            actual[key], = struct.unpack_from("!H", body, field)

        require(actual.keys() == expected.keys(),
                f"Unexpected {replay.name} service-time feedback: {actual}, expected {expected.keys()}")
        measured = []
        for key, (timestamp, opaque) in expected.items():
            observed = actual[key]
            if opaque:
                require(observed == timestamp,
                        f"{replay.name} {key} changed opaque timestamp {timestamp:#x} to {observed:#x}")
                continue
            ticks = (observed - (timestamp or 0)) & 0xFFFF
            # Half the 16-bit range is 4.194 ms at the specification's 128 ns resolution.
            require(0 < ticks < 0x8000,
                    f"{replay.name} {key} did not report a positive local service interval: {ticks * 128} ns")
            if timestamp is not None:
                require(observed < timestamp, f"{replay.name} {key} did not wrap the adjusted timestamp")
            measured.append(ticks)
        print(f"Independent {replay.name} service time passed standalone, wrapped, and opaque checks "
              f"({min(measured) * 128}–{max(measured) * 128} ns)")


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
        check_retransmitted_ack_request(binary, replay)
        check_jumbo_pmtu(binary, replay)
        check_reserved_opcode(binary, replay)
        check_service_time(binary, replay)
    for pmtu in (2048, 4096):
        check_jumbo_requester(binary, pmtu)


if __name__ == "__main__":
    main()
