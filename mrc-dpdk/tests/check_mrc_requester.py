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
TRIM_LAST_HOP = 0x02
TRIM_NACKS = (TRIM, TRIM_LAST_HOP)
NO_PKT_BUFFER = 0x07
RTX = 0x20
ENDPOINT_QPN = 2
ENDPOINT_REQUEST = 0xD8
ENDPOINT_RESPONSE = 0xD9
CONTROL_DSCP = 48


def psn(index):
    return (INITIAL_PSN + index) & PSN_MASK


def payload(offset, length):
    # Application bytes expected from mrc_write, independently of its codec.
    return bytes((i ^ (i >> 8)) & 0xFF for i in range(offset, offset + length))


class ReferenceResponder:
    write_lengths = WRITE_LENGTHS
    opcodes = OPCODES

    def __init__(self, wire, mode, nack_reason):
        self.wire = wire
        self.nack_reason = nack_reason
        self.data_dscp = 0
        self.retry_dscp = 0
        self.ipv6 = mode != "ipv4"
        self.srv6 = mode in ("srv6", "srv6-srh")
        self.srh = mode == "srv6-srh"
        self.addresses = ADDRESSES["ipv6" if self.ipv6 else "ipv4"]
        self.peers = {
            role: [IPPeer(bytes.fromhex(mac.replace(":", "")), self.addresses[role].packed)
                   for mac in macs]
            for role, macs in MACS.items()
        }
        self.memory = bytearray(sum(self.write_lengths))
        self.first_write_end = max(0, (self.write_lengths[0] - 1) // 1024)
        self.received = set()
        self.cumulative = -1
        self.received_bytes = 0
        self.sacks_sent = 0
        self.rejected_rail = None
        self.retry_rail = None
        self.first_write_acked = False
        self.finished = False

    def reply(self, rail, opcode, number, body, flags=0, *, original_udp_size=None):
        qp = ENDPOINT_QPN if opcode in (ENDPOINT_REQUEST, ENDPOINT_RESPONSE) else REQUESTOR
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

    def sack(self, rail, packet, index=None, probe_id=None, timestamp=0, *,
             penalty=0, restore=False, entropy=None, probe_psn=0):
        # The first bitmap bit represents CACK + 1.
        bitmap = 0
        for received in self.received:
            offset = received - self.cumulative - 1
            if offset >= 0:
                bitmap |= 1 << offset
        cack = psn(self.cumulative)
        trigger = probe_id if probe_id is not None else (index - self.cumulative) & 0xFFFF
        ooo_count = bitmap.bit_count()
        reflected = packet.entropy if entropy is None else entropy
        body = struct.pack("!BBHIHH", 0, 2 if probe_id is not None else 0, trigger,
                           reflected, *PDC_IDS)
        body += struct.pack("!IBBhQ", cack, 0, 0, 1, bitmap)
        wire_bytes = ((self.received_bytes + 255) // 256) & PSN_MASK
        flow_control = (0x80 if restore else 0) | penalty
        body += struct.pack("!HHI", timestamp, ooo_count, flow_control << 24 | wire_bytes)
        number = probe_psn if probe_id is not None else cack
        self.reply(rail, 0xDC, number, body, packet.bth.flags & RTX)
        self.sacks_sent += 1

    def transport_ack(self, rail, msn):
        body = struct.pack("!I", (0x1F << 24) | msn)
        self.reply(rail, 0xD1, psn(self.cumulative), body)

    def nack(self, rail, packet, reason):
        body = struct.pack("!HBBIHHI", 0, reason, 0, packet.entropy, *PDC_IDS, packet.bth.psn)
        body += struct.pack("!I", 0x20000000)
        original = packet.udp_size if reason in TRIM_NACKS else None
        self.reply(rail, 0xDD, packet.bth.psn, body, packet.bth.flags & RTX,
                   original_udp_size=original)

    def write(self, rail, packet):
        index = (packet.bth.psn - INITIAL_PSN) & PSN_MASK
        require(index < len(self.opcodes) and packet.bth.opcode == self.opcodes[index],
                f"Unexpected WRITE opcode {packet.bth.opcode:#x} for packet {index}")
        is_retry = bool(packet.bth.flags & RTX)
        require(not (packet.bth.flags & 0x10), "Unexpected TSETH on benchmark WRITE")
        wr = 0 if index <= self.first_write_end else 1
        require(wr == 0 or self.first_write_acked,
                "WRITEIMM used its receive credit after only a reliability SACK")
        rqmsn, msn, address, rkey, length = struct.unpack_from("!HHQII", packet.body)
        segment = index if wr == 0 else index - self.first_write_end - 1
        offset = segment * 1024 + (self.write_lengths[0] if wr else 0)
        data_length = min(self.write_lengths[wr] - segment * 1024, 1024)
        require((rqmsn, msn, address, rkey, length) ==
                (wr + 1, wr + 1, ADDRESS + offset, RKEY, self.write_lengths[wr]),
                f"Incorrect METH or RETH on WRITE packet {index}")
        at = 20
        if packet.bth.opcode in (0xC9, 0xCB):
            immediate, = struct.unpack_from("!I", packet.body, at)
            require(immediate == WRITE_IMMEDIATES[wr], f"Wrong immediate data on WRITE {wr}")
            at += 4
        data = packet.body[at:]
        require(data == payload(offset, data_length), f"Incorrect payload at WRITE packet {index}")

        if self.nack_reason is not None and index == REJECTED_PACKET and not is_retry:
            require(self.rejected_rail is None, "Repeated first transmission without the RTX bit")
            self.rejected_rail = rail
            self.nack(rail, packet, self.nack_reason)
            return
        if self.nack_reason is not None and index == REJECTED_PACKET and index not in self.received:
            require(is_retry and self.rejected_rail is not None and rail != self.rejected_rail,
                    "NACK did not cause an alternate-rail retransmission")
            self.retry_rail = rail

        if index in self.received:
            require(is_retry, f"Duplicate WRITE packet {index} lacked RTX")
            self.sack(rail, packet, index=index)
            if (self.nack_reason is not None and index == 0 and self.cumulative == self.first_write_end
                    and not self.first_write_acked):
                self.transport_ack(rail, 1)
                self.first_write_acked = True
            return

        self.received.add(index)
        self.memory[offset:offset + data_length] = data
        # OCP §8.3.1 counts accepted UDP bytes plus the nominal 40-byte overhead.
        self.received_bytes += packet.udp_size + 40
        while self.cumulative + 1 in self.received:
            self.cumulative += 1
        self.acknowledge_write(rail, packet, index)

    def acknowledge_write(self, rail, packet, index):
        self.sack(rail, packet, index=index)
        if self.cumulative == len(self.opcodes) - 1:
            self.finish_write(rail)

    def finish_write(self, rail):
        self.transport_ack(rail, len(self.write_lengths))
        self.finished = True

    def acknowledge_probe(self, rail, packet, token, timestamp):
        self.sack(rail, packet, probe_id=token, timestamp=timestamp)

    def port_status_request(self, rail, token, mask):
        self.reply(rail, ENDPOINT_RESPONSE, token, bytes(36))

    def port_status_response(self, rail, token):
        raise RuntimeError(f"Unexpected Port Status response on rail {rail} for identifier {token:#06x}")

    def tick(self, now):
        pass

    def idle(self):
        pass

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
        endpoint_op = bth.opcode in (ENDPOINT_REQUEST, ENDPOINT_RESPONSE)
        require(bth.qp == (ENDPOINT_QPN if endpoint_op else RESPONDER), "Incorrect destination QP")
        if endpoint_op:
            require(packet.traffic_class >> 2 == CONTROL_DSCP, "Port Status did not use Control traffic class")
            token = bth.psn & 0xffff
            if bth.opcode == ENDPOINT_REQUEST:
                require(len(packet.body) == 16 and not packet.body[1] & 3, "Invalid Port Status ERTH")
                mask, = struct.unpack_from("!I", packet.body, 4)
                self.port_status_request(rail, token, mask)
            else:
                require(len(packet.body) == 36 and not (packet.body[1] >> 2) & 3, "Invalid Port Status EETH")
                self.port_status_response(rail, token)
        elif bth.opcode == 0xDE:
            require(packet.traffic_class == self.data_dscp << 2,
                    "Reliability probe did not use the configured data DSCP")
            require(len(packet.body) == 16 and struct.unpack_from("!HH", packet.body, 8) == PDC_IDS,
                    "Invalid PETH identifiers")
            token, = struct.unpack_from("!H", packet.body, 4)
            timestamp, = struct.unpack_from("!H", packet.body, 12)
            self.acknowledge_probe(rail, packet, token, timestamp)
        else:
            dscp = self.retry_dscp if bth.flags & RTX else self.data_dscp
            require(packet.traffic_class == (dscp << 2 | 2),
                    "WRITE did not use its configured DSCP with ECT(0)")
            self.write(rail, packet)


class RetryTimerResponder(ReferenceResponder):
    write_lengths = (7,)
    opcodes = (0xCB,)
    timeout_exponent = 17
    base_timeout = (1024 << timeout_exponent) / 1e9

    def __init__(self, wire, mode, exhausted):
        super().__init__(wire, mode, None)
        self.exhausted = exhausted
        self.attempts = []
        self.pending_nack = None
        self.nack_sent_at = None

    def write(self, rail, packet):
        now = time.monotonic()
        attempt = len(self.attempts)
        require(packet.bth.psn == INITIAL_PSN and packet.bth.opcode == self.opcodes[0] and
                bool(packet.bth.flags & RTX) == bool(attempt),
                f"Incorrect PSN, opcode, or RTX on retry timer attempt {attempt}")
        require(not self.exhausted or attempt == 0, "Requester retransmitted after NACK exhaustion")
        self.attempts.append((now, rail))
        if self.exhausted or attempt == 2:
            # The previous rail recovers on a probe. Give its reply time to
            # arrive before rejecting the currently outstanding attempt.
            delay = 0 if self.exhausted else self.base_timeout / 8
            self.pending_nack = (now + delay, rail, packet)
        elif attempt == 3:
            require(self.nack_sent_at is not None and now - self.nack_sent_at < self.base_timeout / 2,
                    "NACK did not trigger an immediate retransmission")
            require(rail != self.attempts[2][1], "NACK retransmission did not use the alternate rail")
        elif attempt == 4:
            super().write(rail, packet)

    def tick(self, now):
        if self.pending_nack is not None and now >= self.pending_nack[0]:
            _, rail, packet = self.pending_nack
            self.nack_sent_at = now
            self.nack(rail, packet, NO_PKT_BUFFER)
            self.pending_nack = None

    def check_intervals(self):
        initial, first_retry, second_retry, nack_retry, next_retry = (at for at, _ in self.attempts)
        first_interval = first_retry - initial
        second_interval = second_retry - first_retry
        reset_interval = next_retry - nack_retry
        base = self.base_timeout
        require(0.7 * base <= first_interval <= 1.4 * base and
                1.6 * base <= second_interval <= 2.5 * base,
                f"Timeout retries did not back off from base to double: {first_interval:.3f}s, "
                f"{second_interval:.3f}s (base {base:.3f}s)")
        require(0.7 * base <= reset_interval <= 1.4 * base,
                f"Timeout after NACK retry did not reset to base: {reset_interval:.3f}s (base {base:.3f}s)")
        return {
            "timeout_retry_intervals_ms": [round(1000 * first_interval), round(1000 * second_interval)],
            "nack_retry_delay_ms": round(1000 * (nack_retry - self.nack_sent_at)),
            "timeout_after_nack_retry_ms": round(1000 * reset_interval),
        }

    def check_exhaustion(self, exited_at):
        require(len(self.attempts) == 1 and self.nack_sent_at is not None,
                "Requester exited before the exhausted NACK was sent")
        delay = exited_at - self.nack_sent_at
        require(delay < self.base_timeout * 0.7,
                f"Retry exhaustion was not immediate after the NACK: {delay:.3f}s "
                f"(base {self.base_timeout:.3f}s)")
        return {"nack_exhaustion_delay_ms": round(1000 * delay)}


class LateNackResponder(ReferenceResponder):
    write_lengths = (7,)
    opcodes = (0xCB,)

    def __init__(self, wire, mode):
        super().__init__(wire, mode, None)
        self.attempts = []
        self.pending_ack = None

    def acknowledge_probe(self, rail, packet, token, timestamp):
        super().acknowledge_probe(1 - rail, packet, token, timestamp)

    def write(self, rail, packet):
        attempt = len(self.attempts)
        require(attempt < 3 and packet.bth.psn == INITIAL_PSN and packet.bth.opcode == self.opcodes[0]
                and bool(packet.bth.flags & RTX) == bool(attempt),
                f"Unexpected WRITE attempt {attempt}")
        if self.attempts:
            require(rail != self.attempts[-1][0], "Retransmission did not alternate rails")
        self.attempts.append((rail, packet))
        if attempt < 2:
            self.nack(1 - rail, packet, NO_PKT_BUFFER)
            return
        # Both retries have RTX set. Replay the first retry's NACK while the
        # second is outstanding, then delay its ACK by one quiet interval.
        prior_rail, prior_packet = self.attempts[1]
        self.nack(1 - prior_rail, prior_packet, NO_PKT_BUFFER)
        self.pending_ack = (1 - rail, packet)

    def idle(self):
        if self.pending_ack is not None:
            super().write(*self.pending_ack)
            self.pending_ack = None


class FastLossResponder(ReferenceResponder):
    packets = 24
    write_lengths = (packets * 1024,)
    opcodes = (0xC6, *(0xC7,) * (packets - 2), 0xC9)
    timeout_exponent = 18
    base_timeout = (1024 << timeout_exponent) / 1e9

    def __init__(self, wire, mode, case):
        super().__init__(wire, mode, None)
        self.case = case
        self.original = None
        self.original_at = None
        self.delayed = []
        self.release_at = None
        self.retries = 0
        self.first_retry_at = None
        self.retry_interval = None

    def write(self, rail, packet):
        now = time.monotonic()
        index = (packet.bth.psn - INITIAL_PSN) & PSN_MASK
        if packet.bth.flags & RTX:
            if self.case == "base_timer" and self.retries == 1:
                require(index == 0, f"Timeout retransmitted unexpected packet {index}")
                self.retry_interval = now - self.first_retry_at
                require(0.7 * self.base_timeout <= self.retry_interval <= 1.4 * self.base_timeout,
                        f"Timeout after fast retry was {self.retry_interval:.3f}s; "
                        f"expected base {self.base_timeout:.3f}s")
                self.retries += 1
                super().write(rail, packet)
                return
            require(self.case != "sparse" and index == 0 and self.retries == 0,
                    f"Spurious retransmission in {self.case} case at packet {index}")
            require(self.original_at is not None and now - self.original_at < 0.85 * self.base_timeout,
                    "Requester waited for the local ACK timer instead of recovering early")
            self.retries += 1
            if self.case == "base_timer":
                # Drop the SACK-driven retry itself to observe its timeout.
                self.first_retry_at = now
                return
            if self.case == "nack":
                # The SACKs for the original flight remain visible while
                # this NACK-triggered retry takes longer than the fast timer.
                self.delayed.append((rail, packet))
                self.release_at = now + self.base_timeout / 2
                return
        elif self.case == "sparse":
            if index != self.packets - 1:
                self.delayed.append((rail, packet))
                return
            # Only this far-ahead PSN has arrived so far.
            self.release_at = now + self.base_timeout / 2
        elif index == 0:
            self.original = (rail, packet)
            self.original_at = now
            self.reject_original_if_ready()
            return

        super().write(rail, packet)
        self.reject_original_if_ready()

    def reject_original_if_ready(self):
        if self.case == "nack" and self.original is not None and len(self.received) == self.packets - 1:
            self.nack(*self.original, NO_PKT_BUFFER)
            self.original = None

    def tick(self, now):
        if self.release_at is not None and now >= self.release_at:
            for rail, packet in self.delayed:
                super().write(rail, packet)
            self.delayed.clear()
            self.release_at = None

    def check_counters(self, counters):
        expected_retries = {"sparse": 0, "nack": 1, "loss": 1, "base_timer": 2}
        require(self.retries == sum(counters["retries"]) == expected_retries[self.case]
                and counters["fast_retries"] == (self.case in ("loss", "base_timer"))
                and counters["nack_received_mask"] == (1 << NO_PKT_BUFFER if self.case == "nack" else 0),
                f"Incorrect fast-loss accounting in {self.case}: {counters}")
        if self.case == "base_timer":
            counters["timeout_after_fast_retry_ms"] = round(1000 * self.retry_interval)


class PortStatusResponder(ReferenceResponder):
    RAIL = 1
    # The second identifier appears older even under 16-bit serial-number arithmetic.
    BLOCK_TOKEN = 0x7123
    FOLLOWUP_TOKEN = 0x0029

    def __init__(self, wire, mode, expire):
        super().__init__(wire, mode, nack_reason=None)
        self.expire = expire
        self.first_report = None
        self.report_interval = None
        self.block_sent = False
        self.blocked_at = None
        self.followup_sent_at = None
        self.followup_echoed = False
        self.resume_delay = None

    def advertise(self, token, mask):
        body = struct.pack("!IIII", 0, mask, 0, 0)
        self.reply(self.RAIL, ENDPOINT_REQUEST, token, body)

    def port_status_request(self, rail, token, mask):
        require(mask & (1 << rail) and not mask & ~3, f"Invalid local reachability mask {mask:#x}")
        if rail != self.RAIL:
            return
        now = time.monotonic()
        if self.first_report is None:
            if mask == 3:
                # Withhold this EETH to exercise another independent update.
                self.first_report = (token, now)
        elif self.report_interval is None:
            require(mask == 3 and token != self.first_report[0],
                    "The next local update must retain both links and use a fresh identifier")
            self.report_interval = now - self.first_report[1]
            require(0.15 <= self.report_interval <= 0.50,
                    f"Local Port Status refresh interval {self.report_interval:.3f}s was outside the expected cadence")
            super().port_status_request(rail, token, mask)

    def port_status_response(self, rail, token):
        require(rail == self.RAIL, "Port Status response changed rails")
        if token == self.BLOCK_TOKEN:
            require(self.block_sent and self.blocked_at is None, "Duplicate or unsolicited zero-mask response")
            self.blocked_at = time.monotonic()
            self.first_write_acked = True
            self.transport_ack(rail, 1)
        else:
            require(token == self.FOLLOWUP_TOKEN and self.followup_sent_at is not None and not self.followup_echoed,
                    f"Port Status response did not echo the follow-up identifier: {token:#06x}")
            self.followup_echoed = True

    def tick(self, now):
        # The credit is available for a measurable interval with no reachable rails.
        if self.blocked_at is None or self.report_interval is None or self.followup_sent_at is not None:
            return
        pause = 0.35 if self.expire else 0.10
        if now - self.blocked_at >= pause:
            self.followup_sent_at = now
            self.advertise(self.FOLLOWUP_TOKEN, 0 if self.expire else 1 << self.RAIL)

    def write(self, rail, packet):
        if packet.bth.psn == psn(FIRST_WRITE_END + 1):
            require(self.followup_echoed, "WRITE progressed while the peer still advertised no reachable rails")
            now = time.monotonic()
            self.resume_delay = now - self.followup_sent_at
            if self.expire:
                require(0.85 <= self.resume_delay <= 1.45,
                        f"Refreshed zero mask became stale after {self.resume_delay:.3f}s")
            else:
                require(rail == self.RAIL, f"Only rail {self.RAIL} was advertised after the second update")
                require(self.resume_delay <= 0.4 and now - self.blocked_at < 0.9,
                        f"The lower identifier did not promptly unblock sending ({self.resume_delay:.3f}s)")
        super().write(rail, packet)
        if self.cumulative == FIRST_WRITE_END and not self.block_sent:
            self.block_sent = True
            self.advertise(self.BLOCK_TOKEN, 0)


class FlowControlResponder(ReferenceResponder):
    write_lengths = (WRITE_LENGTHS[0], 7169)
    opcodes = OPCODES[:FIRST_WRITE_END + 1] + (0xC6,) + (0xC7,) * 6 + (0xC9,)
    PENALTY = 127

    def __init__(self, wire, mode):
        super().__init__(wire, mode, nack_reason=None)
        self.held_packet = None
        self.restore_at = None
        self.restricted_stages = 0
        self.restored_full_packets = 0

    def acknowledge_probe(self, rail, packet, token, timestamp):
        # A probe response would also return byte credits during the observation.
        if not self.first_write_acked:
            super().acknowledge_probe(rail, packet, token, timestamp)

    def acknowledge_write(self, rail, packet, index):
        if index <= FIRST_WRITE_END:
            if index == FIRST_WRITE_END:
                require(packet.bth.flags & 0x80, "Credit-exhausting WRITEIMM did not request an ACK")
            if self.cumulative == FIRST_WRITE_END:
                self.sack(rail, packet, index=index, penalty=self.PENALTY, restore=True)
                # Both messages use the same rail, so the penalty precedes the credit.
                self.transport_ack(rail, 1)
                self.first_write_acked = True
            else:
                super().acknowledge_write(rail, packet, index)
        elif self.restore_at is None:
            require(self.held_packet is None and index == FIRST_WRITE_END + 1 + self.restricted_stages,
                    f"Receiver penalty allowed more than one outstanding WRITE packet at index {index}")
            require(packet.bth.flags & 0x80, f"Flow-controlled WRITE packet {index} did not request an ACK")
            self.held_packet = (rail, packet, index)
        else:
            if self.restored_full_packets < 2:
                if index < len(self.opcodes) - 1:
                    self.restored_full_packets += 1
                if self.restored_full_packets < 2:
                    return
            super().acknowledge_write(rail, packet, index)

    def idle(self):
        if self.held_packet is None:
            return
        # Poll to a quiet interval before returning any credit for this packet.
        rail, packet, index = self.held_packet
        self.held_packet = None
        self.restricted_stages += 1
        if self.restricted_stages == 1:
            self.sack(rail, packet, index=index, penalty=self.PENALTY)
        else:
            self.restore_at = time.monotonic()
            self.sack(rail, packet, index=index, restore=True)

    def tick(self, now):
        if self.restore_at is not None and now - self.restore_at > 0.4:
            require(self.restored_full_packets == 2,
                    "Zero penalty with restore did not reopen room for multiple outstanding WRITE packets")


class ProbeCorrelationResponder(FlowControlResponder):
    RESERVED_PSN = 0x5A5A5A

    def __init__(self, wire, mode):
        super().__init__(wire, mode)
        self.initial_tokens = {}
        self.target_probe = None
        self.lost_reply_retried = False
        self.data_complete = False
        self.uncorrelated_replies_sent = 0

    def acknowledge_probe(self, rail, packet, token, timestamp):
        if rail not in self.initial_tokens:
            self.initial_tokens[rail] = token
            if self.target_probe is None:
                self.target_probe = (rail, packet, token, timestamp)
            return

        target_rail = self.target_probe[0]
        if rail != target_rail and not self.lost_reply_retried:
            require(token != self.initial_tokens[rail], "Retry reused the lost probe identifier")
            self.lost_reply_retried = True
            self.sack(rail, packet, probe_id=token, timestamp=timestamp, probe_psn=self.RESERVED_PSN)
            if self.data_complete:
                self.finish_write(rail)

    def idle(self):
        if self.held_packet is None or self.target_probe is None:
            return
        rail, packet, token, timestamp = self.target_probe
        if self.uncorrelated_replies_sent == 0:
            self.sack(rail, packet, probe_id=token ^ 0x8000, timestamp=timestamp, restore=True,
                      probe_psn=self.RESERVED_PSN)
        elif self.uncorrelated_replies_sent == 1:
            self.sack(rail, packet, probe_id=token, timestamp=timestamp, restore=True,
                      entropy=packet.entropy ^ 0x80, probe_psn=self.RESERVED_PSN)
        else:
            self.held_packet = None
            self.restore_at = time.monotonic()
            self.sack(rail, packet, probe_id=token, timestamp=timestamp, restore=True,
                      probe_psn=self.RESERVED_PSN)
            # An already consumed identifier cannot apply a new receiver penalty.
            self.sack(rail, packet, probe_id=token, timestamp=timestamp, penalty=self.PENALTY,
                      probe_psn=self.RESERVED_PSN)
        self.uncorrelated_replies_sent += 1

    def finish_write(self, rail):
        self.data_complete = True
        # Keep the requester active until the other rail reissues its probe.
        if self.lost_reply_retried:
            super().finish_write(rail)


def stop_relay(wire, relay):
    wire.send(b"")
    _, error = relay.communicate(timeout=3)
    require(relay.returncode == 0, f"memif relay failed\n{error}")


def run_requester(binary, relay_binary, mode, nack_reason=None, *,
                  port_status=None, flow_control=False, probe_correlation=False, retry_timer=None,
                  late_nack=False, fast_loss=None):
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
            ack_timeout = 16 if nack_reason is not None else 20
            if retry_timer is not None:
                responder = RetryTimerResponder(wire, mode, exhausted=retry_timer == "exhausted")
                ack_timeout = responder.timeout_exponent
                wire.settimeout(0.01)
            elif late_nack:
                responder = LateNackResponder(wire, mode)
            elif fast_loss is not None:
                responder = FastLossResponder(wire, mode, fast_loss)
                ack_timeout = responder.timeout_exponent
                wire.settimeout(0.005)
            elif probe_correlation:
                responder = ProbeCorrelationResponder(wire, mode)
            elif flow_control:
                responder = FlowControlResponder(wire, mode)
            elif port_status is not None:
                responder = PortStatusResponder(wire, mode, expire=port_status == "expire")
            else:
                responder = ReferenceResponder(wire, mode, nack_reason)
            writes = ",".join(f"{length}@{immediate:#x}"
                              for length, immediate in zip(responder.write_lengths, WRITE_IMMEDIATES))
            command += ["--", "tx", writes,
                        str(responder.addresses["tx"]), str(responder.addresses["rx"]),
                        "0", MACS["rx"][0], "1", MACS["rx"][1], "--initial-psn", str(INITIAL_PSN),
                        "--requestor-qpn", hex(REQUESTOR), "--responder-qpn", hex(RESPONDER),
                        "--p-key", hex(PKEY), "--remote-address", hex(ADDRESS), "--remote-key", hex(RKEY),
                        "--ack-timeout", str(ack_timeout),
                        "--max-wimm-inflight", "1", "--timeout-ms", "3000"]
            if retry_timer is not None:
                command += ["--linear-retries", "0", "--exp-retries", "0" if responder.exhausted else "4"]
            if fast_loss == "base_timer":
                command += ["--linear-retries", "0", "--exp-retries", "3"]
            if late_nack:
                command += ["--linear-retries", "2", "--exp-retries", "0"]
            if nack_reason == NO_PKT_BUFFER:
                responder.data_dscp, responder.retry_dscp = 26, 34
                command += ["--data-dscp", "26", "--retry-dscp", "34"]
            elif nack_reason == TRIM_LAST_HOP:
                responder.data_dscp = responder.retry_dscp = 18
                command += ["--data-dscp", "18"]
            if responder.srv6:
                command += ["--srv6-local", str(SIDS["tx"]), "--srv6-peer", str(SIDS["rx"])]
                if responder.srh:
                    command += ["--srv6-srh", "1"]
            requester = start(command, directory)
            deadline = time.monotonic() + 4
            while not responder.finished:
                now = time.monotonic()
                require(now < deadline, "Independent peer did not complete the exchange")
                responder.tick(now)
                try:
                    message = wire.recv(2048)
                except TimeoutError:
                    if requester.poll() is not None:
                        if retry_timer == "exhausted":
                            exited_at = time.monotonic()
                            diagnostics = collect(requester, "Retry limit exhausted")
                            require(f"NACK-RX={1 << NO_PKT_BUFFER:#x}" in diagnostics and
                                    "state=ERROR work-errors=1 first-status=retry-exceeded" in diagnostics,
                                    f"NACK did not fail the outstanding work correctly:\n{diagnostics}")
                            counters = responder.check_exhaustion(exited_at)
                            stop_relay(wire, relay)
                            return counters
                        raise RuntimeError(f"Requester exited before independent completion: {collect(requester)}")
                    if relay.poll() is not None:
                        _, error = relay.communicate()
                        raise RuntimeError(f"memif relay exited during exchange\n{error}")
                    responder.idle()
                    continue
                responder.receive(message)

            counters = collect(requester)
            total = sum(responder.write_lengths)
            work_requests = len(responder.write_lengths)
            require(responder.memory == payload(0, total), "Incomplete peer memory placement")
            require(counters["bytes"] == total and counters["packets"] == len(responder.opcodes) and
                    (flow_control or counters["probe_replies"] > 0) and
                    counters["transport_acks"] == counters["write_completions"] == work_requests,
                    f"Unexpected requester counters: {counters}")
            if nack_reason is not None:
                require(responder.retry_rail is not None and responder.first_write_acked,
                        "Fixture did not exercise both retransmission and delayed semantic completion")
                require(counters["nack_received_mask"] == 1 << nack_reason and
                        counters["trim_nacks"] == (nack_reason in TRIM_NACKS) and
                        counters["ev_skipped"] == (nack_reason == TRIM),
                        f"Unexpected NACK counters: {counters}")
            elif retry_timer is not None:
                require(sum(counters["retries"]) == 4 and counters["nack_received_mask"] == 1 << NO_PKT_BUFFER,
                        f"Unexpected retry timer counters: {counters}")
                counters.update(responder.check_intervals())
            elif late_nack:
                require(len(responder.attempts) == 3 and sum(counters["retries"]) == 2
                        and counters["probe_replies"] == 2
                        and counters["nack_received_mask"] == 1 << NO_PKT_BUFFER,
                        f"Late NACK interfered with the current attempt: {counters}")
            elif fast_loss is not None:
                responder.check_counters(counters)
            elif port_status is not None:
                require(counters["port_status_updates"] == 2 and counters["port_status_replies"] == 1,
                        f"Unexpected Port Status counters: {counters}")
                counters["status_report_interval_ms"] = round(1000 * responder.report_interval)
                counters["status_resume_delay_ms"] = round(1000 * responder.resume_delay)
            elif flow_control:
                require(responder.restricted_stages == 2 and responder.restored_full_packets == 2
                        and not any(counters["retries"]) and counters["nack_received_mask"] == 0,
                        f"Unexpected receiver flow-control observations: {counters}")
                counters["single_packet_stages"] = responder.restricted_stages
                counters["restored_full_packets_before_feedback"] = responder.restored_full_packets
            else:
                require(responder.uncorrelated_replies_sent == 3 and responder.lost_reply_retried
                        and counters["probe_replies"] == 2
                        and counters["sacks"] == responder.sacks_sent - responder.uncorrelated_replies_sent,
                        f"Unexpected probe correlation observations: {counters}")
                counters["uncorrelated_probe_sacks_ignored"] = responder.uncorrelated_replies_sent
            stop_relay(wire, relay)
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
    focused = parser.add_mutually_exclusive_group()
    focused.add_argument("--port-status-only", action="store_true", help="Run only the Port Status cases")
    focused.add_argument("--flow-control-only", action="store_true", help="Run only the responder flow-control case")
    focused.add_argument("--probe-correlation-only", action="store_true", help="Run only the probe correlation case")
    focused.add_argument("--retry-timer-only", action="store_true", help="Run only the NACK and timeout retry cases")
    focused.add_argument("--late-nack-only", action="store_true", help="Run only cross-rail late NACK cases")
    focused.add_argument("--fast-loss-only", action="store_true", help="Run only SACK fast-loss cases")
    args = parser.parse_args()
    require(args.binary.is_file() and args.relay.is_file(), "Build mrc_write and tests/mrc_memif_relay first")
    binary, relay = args.binary.resolve(), args.relay.resolve()
    normal_suite = not (args.port_status_only or args.flow_control_only or args.probe_correlation_only
                        or args.retry_timer_only or args.late_nack_only or args.fast_loss_only)
    if normal_suite:
        for mode in args.mode or MODES:
            for reason, label in ((NO_PKT_BUFFER, "no_packet_buffer"), (TRIM, "trim"),
                                  (TRIM_LAST_HOP, "trim_last_hop")):
                counters = run_requester(binary, relay, mode, reason)
                print(json.dumps({"mode": mode, "injected_nack": label, **counters}))
    if normal_suite or args.late_nack_only:
        for mode in args.mode or MODES:
            counters = run_requester(binary, relay, mode, late_nack=True)
            print(json.dumps({"mode": mode, "late_nack": True, **counters}))
    mode = (args.mode or ("ipv6",))[0]
    if normal_suite or args.port_status_only:
        for case in ("replace", "expire"):
            counters = run_requester(binary, relay, mode, port_status=case)
            print(json.dumps({"mode": mode, "port_status": case, **counters}))
    if normal_suite or args.flow_control_only:
        counters = run_requester(binary, relay, mode, flow_control=True)
        print(json.dumps({"mode": mode, "flow_control": "penalty_and_restore", **counters}))
    if normal_suite or args.probe_correlation_only:
        counters = run_requester(binary, relay, mode, probe_correlation=True)
        print(json.dumps({"mode": mode, "probe_correlation": True, **counters}))
    if normal_suite or args.retry_timer_only:
        for case in ("backoff", "exhausted"):
            counters = run_requester(binary, relay, mode, retry_timer=case)
            print(json.dumps({"mode": mode, "retry_timer": case, **counters}))
    if normal_suite or args.fast_loss_only:
        for case in ("sparse", "nack", "loss", "base_timer"):
            counters = run_requester(binary, relay, mode, fast_loss=case)
            print(json.dumps({"mode": mode, "fast_loss": case, **counters}))


if __name__ == "__main__":
    main()
