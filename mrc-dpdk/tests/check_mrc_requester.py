#!/usr/bin/env python3
"""Validate the DPDK requester against independent IPv4, IPv6, and SRv6 feedback."""

import argparse
import ipaddress
import json
import os
import socket
import struct
import tempfile
import time
from pathlib import Path

from memif_pair import MACS, collect, eal_args, require, start
from mrc_wire_reference import (
    Bth, IPPeer, decode_ipv4, decode_ipv6, encode_ipv4, encode_ipv6, unwrap_srv6, wrap_srv6,
)


REQUESTOR = 0x120345
RESPONDER = 0x230456
PKEY = 0xAF31
PSN_MASK = 0xFFFFFF
INITIAL_PSN = 0xFFFFFD
ADDRESS = 0x123400000
RKEY = 0xDECAFE
UDP_PORT = 4971
PDC_IDS = (REQUESTOR & 0xFFFF, RESPONDER & 0xFFFF)
ADDRESSES = {
    "ipv4": {"tx": ipaddress.ip_address("192.0.2.1"), "rx": ipaddress.ip_address("192.0.2.2")},
    "ipv6": {"tx": ipaddress.ip_address("2001:db8::1"), "rx": ipaddress.ip_address("2001:db8::2")},
}
SIDS = {"tx": ipaddress.ip_address("2001:db8:aaaa::1"), "rx": ipaddress.ip_address("2001:db8:bbbb::2")}
MODES = ("ipv4", "ipv6", "srv6", "srv6-srh")
WRITE_IMMEDIATES = (0xFFFFFFFF, 0x6B)
WRITE_LENGTHS = (4111, 7)
OPCODES = (0xC6, 0xC7, 0xC7, 0xC7, 0xC9, 0xCB)
FIRST_WRITE_END = 4
REJECTED_PACKET = 1
TRIM = 0x01
NO_PKT_BUFFER = 0x07
RTX = 0x20


def psn(index):
    return (INITIAL_PSN + index) & PSN_MASK


def payload(offset, length):
    # Application bytes expected from mrc_write, independently of its codec.
    return bytes((i ^ (i >> 8)) & 0xFF for i in range(offset, offset + length))


class ReferenceResponder:
    def __init__(self, wire, mode, nack_reason):
        self.wire = wire
        self.nack_reason = nack_reason
        self.ipv6 = mode != "ipv4"
        self.srv6 = mode in ("srv6", "srv6-srh")
        self.srh = mode == "srv6-srh"
        self.addresses = ADDRESSES["ipv6" if self.ipv6 else "ipv4"]
        self.peers = {
            role: [IPPeer(bytes.fromhex(mac.replace(":", "")), self.addresses[role].packed)
                   for mac in macs]
            for role, macs in MACS.items()
        }
        self.memory = bytearray(sum(WRITE_LENGTHS))
        self.received = set()
        self.cumulative = -1
        self.received_bytes = 0
        self.rejected_rail = None
        self.retry_rail = None
        self.first_write_acked = False
        self.finished = False

    def reply(self, rail, opcode, number, body, flags=0, *, original_udp_size=None):
        qp = 2 if opcode == 0xD9 else REQUESTOR
        source, destination = self.peers["rx"][rail], self.peers["tx"][rail]
        header = Bth(opcode, PKEY, qp, number, flags)
        if self.ipv6:
            frame = encode_ipv6(source, destination, 0xD000 + rail, UDP_PORT,
                                header, body, flow_label=0x98140 + rail, traffic_class=0xC0,
                                original_udp_size=original_udp_size)
            if self.srv6:
                # Routable outer fields need not carry the inner packet's EV.
                frame = wrap_srv6(frame, SIDS["rx"].packed, SIDS["tx"].packed, srh=self.srh,
                                  flow_label=0x2468 + rail, traffic_class=0xC0)
        else:
            frame = encode_ipv4(source, destination, 0xD000 + rail, UDP_PORT, header, body,
                                original_udp_size=original_udp_size)
        self.wire.send(bytes([rail]) + frame)

    def sack(self, rail, packet, index=None, probe_id=None, timestamp=0):
        # The first bitmap bit represents CACK + 1.
        bitmap = 0
        for received in self.received:
            offset = received - self.cumulative - 1
            if offset >= 0:
                bitmap |= 1 << offset
        cack = psn(self.cumulative)
        trigger = probe_id if probe_id is not None else (index - self.cumulative) & 0xFFFF
        ooo_count = bitmap.bit_count()
        body = struct.pack("!BBHIHH", 0, 2 if probe_id is not None else 0, trigger,
                           packet.entropy, *PDC_IDS)
        body += struct.pack("!IBBhQ", cack, 0, 0, 1, bitmap)
        wire_bytes = ((self.received_bytes + 255) // 256) & PSN_MASK
        body += struct.pack("!HHI", timestamp, ooo_count, wire_bytes)
        number = 0 if probe_id is not None else cack
        self.reply(rail, 0xDC, number, body, packet.bth.flags & RTX)

    def transport_ack(self, rail, msn):
        body = struct.pack("!I", (0x1F << 24) | msn)
        self.reply(rail, 0xD1, psn(self.cumulative), body)

    def write(self, rail, packet):
        index = (packet.bth.psn - INITIAL_PSN) & PSN_MASK
        require(index < len(OPCODES) and packet.bth.opcode == OPCODES[index],
                f"Unexpected WRITE opcode {packet.bth.opcode:#x} for packet {index}")
        is_retry = bool(packet.bth.flags & RTX)
        require(not (packet.bth.flags & 0x10), "Unexpected TSETH on benchmark WRITE")
        wr = 0 if index <= FIRST_WRITE_END else 1
        require(wr == 0 or self.first_write_acked,
                "WRITEIMM used its receive credit after only a reliability SACK")
        rqmsn, msn, address, rkey, length = struct.unpack_from("!HHQII", packet.body)
        offset = index * 1024 if wr == 0 else WRITE_LENGTHS[0]
        remaining = WRITE_LENGTHS[wr] - (offset if wr == 0 else 0)
        data_length = min(remaining, 1024)
        require((rqmsn, msn, address, rkey, length) ==
                (wr + 1, wr + 1, ADDRESS + offset, RKEY, WRITE_LENGTHS[wr]),
                f"Incorrect METH or RETH on WRITE packet {index}")
        at = 20
        if packet.bth.opcode in (0xC9, 0xCB):
            immediate, = struct.unpack_from("!I", packet.body, at)
            require(immediate == WRITE_IMMEDIATES[wr], f"Wrong immediate data on WRITE {wr}")
            at += 4
        data = packet.body[at:]
        require(data == payload(offset, data_length), f"Incorrect payload at WRITE packet {index}")

        if index == REJECTED_PACKET and not is_retry:
            require(self.rejected_rail is None, "Repeated first transmission without the RTX bit")
            self.rejected_rail = rail
            body = struct.pack("!HBBIHHI", 0, self.nack_reason, 0, packet.entropy,
                               *PDC_IDS, packet.bth.psn)
            body += struct.pack("!I", 0x20000000)
            original = packet.udp_size if self.nack_reason == TRIM else None
            self.reply(rail, 0xDD, packet.bth.psn, body, original_udp_size=original)
            return
        if index == REJECTED_PACKET and index not in self.received:
            require(is_retry and self.rejected_rail is not None and rail != self.rejected_rail,
                    "Mandatory NACK did not cause an alternate-rail retransmission")
            self.retry_rail = rail

        if index in self.received:
            require(is_retry, f"Duplicate WRITE packet {index} lacked RTX")
            self.sack(rail, packet, index=index)
            if index == 0 and self.cumulative == FIRST_WRITE_END and not self.first_write_acked:
                self.transport_ack(rail, 1)
                self.first_write_acked = True
            return

        self.received.add(index)
        self.memory[offset:offset + data_length] = data
        # OCP §8.3.1 counts accepted UDP bytes plus the nominal 40-byte overhead.
        self.received_bytes += packet.udp_size + 40
        while self.cumulative + 1 in self.received:
            self.cumulative += 1
        self.sack(rail, packet, index=index)
        if self.cumulative == len(OPCODES) - 1:
            self.transport_ack(rail, 2)
            self.finished = True

    def receive(self, message):
        require(len(message) > 1 and message[0] < 2, "Invalid relay datagram")
        rail = message[0]
        frame = message[1:]
        outer = None
        if self.srv6:
            outer = unwrap_srv6(frame, SIDS["tx"].packed, SIDS["rx"].packed, srh=self.srh)
            frame = outer.frame
        decode = decode_ipv6 if self.ipv6 else decode_ipv4
        packet = decode(frame, self.peers["tx"][rail], self.peers["rx"][rail], UDP_PORT)
        if outer:
            require((outer.flow_label, outer.traffic_class) == (packet.flow_label, packet.traffic_class),
                    "SRv6 envelope lost the configured flow label or traffic class")
        bth = packet.bth
        expected_flow = 0xA0100 + rail if self.ipv6 else 0
        expected_entropy = ((expected_flow & 0xFFFF) << 16) | (0xC000 + rail)
        require(bth.pkey == PKEY and packet.entropy == expected_entropy and packet.flow_label == expected_flow,
                "Incorrect requester partition or path entropy")
        require(bth.qp == (2 if bth.opcode == 0xD8 else RESPONDER), "Incorrect destination QP")
        if bth.opcode == 0xD8 and len(packet.body) == 16 and packet.body[1] == 0:
            self.reply(rail, 0xD9, bth.psn, bytes(36))
        elif bth.opcode == 0xDE:
            require(len(packet.body) == 16 and struct.unpack_from("!HH", packet.body, 8) == PDC_IDS,
                    "Invalid PETH identifiers")
            token, = struct.unpack_from("!H", packet.body, 4)
            timestamp, = struct.unpack_from("!H", packet.body, 12)
            self.sack(rail, packet, probe_id=token, timestamp=timestamp)
        else:
            self.write(rail, packet)


def run_requester(binary, relay_binary, mode, nack_reason):
    cpus = sorted(os.sched_getaffinity(0))
    require(len(cpus) >= 2, "The requester fixture needs two polling cores")
    with tempfile.TemporaryDirectory(prefix="mrc-peer-") as temporary, \
            socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as wire:
        directory = Path(temporary)
        fixture_socket = directory / "fixture.sock"
        relay_socket = directory / "relay.sock"
        wire.bind(str(fixture_socket))
        wire.settimeout(0.05)
        relay_args = eal_args(relay_binary, "rx", cpus[0], directory)
        relay = start(relay_args + ["--", str(relay_socket), str(fixture_socket)], directory)
        requester = None
        try:
            deadline = time.monotonic() + 4
            while not relay_socket.exists():
                if relay.poll() is not None:
                    _, error = relay.communicate()
                    raise RuntimeError(f"memif relay exited during setup\n{error}")
                require(time.monotonic() < deadline, "Timed out starting the memif relay")
                time.sleep(0.01)
            wire.connect(str(relay_socket))
            command = eal_args(binary, "tx", cpus[1], directory)
            responder = ReferenceResponder(wire, mode, nack_reason)
            command += ["--", "tx", "4111@0xffffffff,7@0x6b",
                        str(responder.addresses["tx"]), str(responder.addresses["rx"]),
                        "0", MACS["rx"][0], "1", MACS["rx"][1], "--initial-psn", str(INITIAL_PSN),
                        "--requestor-qpn", hex(REQUESTOR), "--responder-qpn", hex(RESPONDER),
                        "--p-key", hex(PKEY), "--remote-address", hex(ADDRESS), "--remote-key", hex(RKEY),
                        "--ack-timeout", "16", "--max-wimm-inflight", "1", "--timeout-ms", "3000"]
            if responder.srv6:
                command += ["--srv6-local", str(SIDS["tx"]), "--srv6-peer", str(SIDS["rx"])]
                if responder.srh:
                    command += ["--srv6-srh", "1"]
            requester = start(command, directory)
            deadline = time.monotonic() + 4
            while not responder.finished:
                require(time.monotonic() < deadline, "Independent peer did not complete the exchange")
                try:
                    message = wire.recv(2048)
                except TimeoutError:
                    if requester.poll() is not None:
                        raise RuntimeError(f"Requester exited before independent completion: {collect(requester)}")
                    if relay.poll() is not None:
                        _, error = relay.communicate()
                        raise RuntimeError(f"memif relay exited during exchange\n{error}")
                    continue
                responder.receive(message)

            counters = collect(requester)
            require(responder.memory == payload(0, sum(WRITE_LENGTHS)), "Incomplete peer memory placement")
            require(responder.retry_rail is not None and responder.first_write_acked,
                    "Fixture did not exercise both retransmission and delayed semantic completion")
            require(counters["bytes"] == sum(WRITE_LENGTHS) and counters["packets"] == len(OPCODES) and
                    counters["nack_received_mask"] == 1 << nack_reason and
                    counters["trim_nacks"] == (nack_reason == TRIM) and
                    counters["probe_replies"] > 0 and counters["transport_acks"] == 2 and
                    counters["write_completions"] == 2,
                    f"Unexpected requester counters: {counters}")
            wire.send(b"")
            _, error = relay.communicate(timeout=3)
            require(relay.returncode == 0, f"memif relay failed\n{error}")
            return counters
        finally:
            for process in (requester, relay):
                if process is not None:
                    if process.poll() is None:
                        process.kill()
                    process.communicate()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[1]
    parser.add_argument("--binary", type=Path, default=root / "mrc_write")
    parser.add_argument("--relay", type=Path, default=root / "tests" / "mrc_memif_relay")
    parser.add_argument("--mode", choices=MODES, action="append", help="Run only this mode")
    args = parser.parse_args()
    require(args.binary.is_file() and args.relay.is_file(), "Build mrc_write and tests/mrc_memif_relay first")
    for mode in args.mode or MODES:
        for reason, label in ((NO_PKT_BUFFER, "no_packet_buffer"), (TRIM, "trim")):
            counters = run_requester(args.binary.resolve(), args.relay.resolve(), mode, reason)
            print(json.dumps({"mode": mode, "injected_nack": label, **counters}))


if __name__ == "__main__":
    main()
