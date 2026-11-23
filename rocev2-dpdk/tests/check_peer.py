#!/usr/bin/env python3
"""Exercise the production DPDK endpoint against an independent RC wire peer."""

import os
from contextlib import contextmanager
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
MRC_TESTS = ROOT / "mrc-dpdk" / "tests"
sys.path.insert(0, str(MRC_TESTS))
from memif_pair import eal_args
from mrc_wire_reference import Bth, IPPeer, decode_ipv4, encode_ipv4

BINARY = ROOT / "rocev2-dpdk" / "roce_write"
OFFER = struct.Struct("!10I6s")
SIZE = 515
PMTU = 256
PKEY = 0xFFFF
PSN_MASK = 0xFFFFFF
PEER_PSN = PSN_MASK - 1
PEER_QPN = 0x012345
ADDRESS = 0x1020304050607080
RKEY = 0x8192A3B4
PEER = IPPeer(bytes.fromhex("0200000000c1"), socket.inet_aton("192.0.2.2"))
DATA = bytes(((i * 2654435761) >> 24) & 0xFF for i in range(SIZE))
OPCODES = (0x06, 0x07, 0x08)


@contextmanager
def process(args, directory):
    env = {**os.environ, "XDG_RUNTIME_DIR": str(directory)}
    child = subprocess.Popen(args, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        yield child
    except Exception as error:
        if child.poll() is None:
            child.terminate()
        out, err = child.communicate(timeout=5)
        raise RuntimeError(f"{Path(args[0]).name} failed\n{out}{err}") from error
    finally:
        if child.poll() is None:
            child.kill()
            child.communicate()


class Peer:
    def __init__(self, wire, control, sender):
        self.wire = wire
        self.control = control
        self.source_port = None
        offer = OFFER.pack(
            0x52435731, SIZE, int.from_bytes(PEER.ip, "big"), PEER_QPN, PEER_PSN, PMTU,
            RKEY, ADDRESS >> 32, ADDRESS & 0xFFFFFFFF, int(sender), PEER.mac,
        )
        control.sendall(offer)
        fields = OFFER.unpack(self.read(OFFER.size))
        magic, size, ip, self.qpn, self.psn, mtu, key, high, low, remote_sender, mac = fields
        assert (magic, size, mtu, remote_sender) == (0x52435731, SIZE, PMTU, int(not sender))
        assert ip == int.from_bytes(socket.inet_aton("192.0.2.1"), "big")
        self.endpoint = IPPeer(mac, ip.to_bytes(4, "big"))
        self.remote_region = (high << 32 | low, key, size)
        control.sendall(b"R")
        assert self.read(1) == b"R"

    def read(self, length):
        chunks = bytearray()
        while len(chunks) < length:
            chunk = self.control.recv(length - len(chunks))
            if not chunk:
                raise RuntimeError("endpoint closed the setup connection")
            chunks.extend(chunk)
        return chunks

    def send(self, opcode, psn, body, *, ack=False, corrupt=False):
        bth = Bth(opcode, PKEY, self.qpn, psn & PSN_MASK, 0x80 if ack else 0)
        frame = encode_ipv4(PEER, self.endpoint, 0xC012, 4791, bth, body)
        if corrupt:
            frame = frame[:-1] + bytes([frame[-1] ^ 1])
        self.wire.send(b"\0" + frame)

    def receive(self):
        packet = self.wire.recv(2048)
        assert packet[:1] == b"\0", packet[:1]
        packet = decode_ipv4(packet[1:], self.endpoint, PEER, 4791)
        if self.source_port is None:
            self.source_port = packet.source_port
        assert packet.source_port == self.source_port
        assert packet.bth.qp == PEER_QPN and packet.bth.pkey == PKEY
        return packet

    def expect_write(self, index):
        packet = self.receive()
        assert packet.bth.opcode == OPCODES[index], packet
        assert packet.bth.psn == (self.psn + index) & PSN_MASK
        assert bool(packet.bth.flags & 0x80) == (index == 2)
        data = DATA[index * PMTU:(index + 1) * PMTU]
        if index == 0:
            data = struct.pack("!QII", ADDRESS, RKEY, SIZE) + data
        assert packet.body == data, f"incorrect WRITE packet {index}"

    def expect_ack(self, index, syndrome, msn):
        packet = self.receive()
        assert packet.bth.opcode == 0x11 and packet.bth.psn == (PEER_PSN + index) & PSN_MASK
        assert packet.body == struct.pack("!I", syndrome << 24 | msn), packet

    def check_requester(self):
        for index in range(3):
            self.expect_write(index)
        # Discard the second segment and send a NAK that also covers the first.
        self.send(0x11, self.psn + 1, struct.pack("!I", 0x60000000))
        self.expect_write(1)
        self.expect_write(2)
        # Model loss of the final ACK. The timer must retry from the remaining gap.
        self.expect_write(1)
        self.expect_write(2)
        self.send(0x11, self.psn + 2, struct.pack("!I", 0x1F000001))
        assert self.read(1) == b"D"
        self.control.sendall(b"V")

    def check_responder(self):
        reth = struct.pack("!QII", *self.remote_region)
        self.send(0x06, PEER_PSN, reth + DATA[:PMTU], ack=True, corrupt=True)
        self.send(0x06, PEER_PSN, reth + DATA[:PMTU])
        self.send(0x08, PEER_PSN + 2, DATA[2 * PMTU:], ack=True)
        self.expect_ack(1, 0x60, 0)
        self.send(0x07, PEER_PSN + 1, DATA[PMTU:2 * PMTU])
        self.send(0x08, PEER_PSN + 2, DATA[2 * PMTU:], ack=True)
        self.expect_ack(2, 0x1F, 1)
        # A duplicate with different bytes must leave the completed buffer intact.
        self.send(0x08, PEER_PSN + 2, bytes(3), ack=True)
        self.expect_ack(2, 0x1F, 1)
        self.control.sendall(b"D")
        assert self.read(1) == b"V"


def check(role, cpus):
    with tempfile.TemporaryDirectory(prefix="roce-peer-") as temporary:
        directory = Path(temporary)
        relay_path, fixture_path = directory / "relay", directory / "fixture"
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as wire, socket.socket() as listener:
            wire.bind(str(fixture_path))
            wire.settimeout(5)
            listener.bind(("127.0.0.1", 0))
            control_port = listener.getsockname()[1]
            if role == "tx":
                listener.listen(1)
                listener.settimeout(5)
            else:
                listener.close()
            relay_args = eal_args(MRC_TESTS / "mrc_memif_relay", "rx", cpus[0], directory)
            relay_args += ["--", str(relay_path), str(fixture_path)]
            with process(relay_args, directory) as relay:
                deadline = time.monotonic() + 5
                while not relay_path.exists():
                    assert relay.poll() is None and time.monotonic() < deadline, "relay startup failed"
                    time.sleep(0.01)
                wire.connect(str(relay_path))
                # Use one data port. The shared relay opens a second idle port.
                args = eal_args(BINARY, "tx", cpus[1], directory, rail_count=1)
                args += ["--", role, str(SIZE), "127.0.0.1", str(control_port),
                         "192.0.2.1", str(PMTU), "0"]
                with process(args, directory) as endpoint:
                    if role == "tx":
                        control, _ = listener.accept()
                    else:
                        assert select.select([endpoint.stdout], [], [], 5)[0], "listener timeout"
                        assert endpoint.stdout.readline() == "control listener ready\n"
                        control = socket.create_connection(("127.0.0.1", control_port), timeout=5)
                    with control:
                        control.settimeout(5)
                        peer = Peer(wire, control, sender=role == "rx")
                        if role == "tx":
                            peer.check_requester()
                        else:
                            peer.check_responder()
                    wire.send(b"")
                    out, err = endpoint.communicate(timeout=5)
                    assert endpoint.returncode == 0, out + err
                    marker = "WRITE complete" if role == "tx" else "WRITE verified"
                    assert f"{marker}: {SIZE} bytes" in out, out
                    if role == "tx":
                        assert "2 retries" in out, out
                out, err = relay.communicate(timeout=5)
                assert relay.returncode == 0, out + err
                print(f"independent RC peer verified {role} with {SIZE} bytes")


def main():
    cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) < 2:
        raise RuntimeError("independent peer test needs two available CPUs")
    check("tx", cpus[:2])
    check("rx", cpus[:2])


if __name__ == "__main__":
    main()
