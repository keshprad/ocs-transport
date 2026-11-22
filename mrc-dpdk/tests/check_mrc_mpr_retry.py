#!/usr/bin/env python3
"""Independent peer: a Dynamic MPR shrink must gate an already NACKed retry."""
import argparse
import json
import os
import socket
import struct
import tempfile
import time
from pathlib import Path

from check_mrc_requester import (
    ADDRESS, INITIAL_PSN, MACS, MODES, NO_PKT_BUFFER, PDC_IDS, PKEY, PSN_MASK,
    REQUESTOR, RESPONDER, RKEY, RTX, SIDS, ReferenceResponder, payload, psn,
    stop_relay, wait_relay_quiesced,
)
from memif_pair import collect, eal_args, require, start


CASES = ("shrink", "static", "equal-cack", "stale-cack", "zero", "oversized")


class MprResponder(ReferenceResponder):
    # A 256-packet negotiated window permits this packet, a 128-packet
    # window anchored at index zero does not. Initial PSN crosses wrap.
    TARGET = 130
    write_lengths = ((TARGET + 1) * 1024,)
    opcodes = (0xC6,) + (0xC7,) * (TARGET - 1) + (0xC9,)

    def __init__(self, wire, mode, case):
        super().__init__(wire, mode, None)
        self.case = case
        self.held = {}
        self.shrunk_at = None
        self.reopened_at = None
        self.retry_at = None
        self.target_rail = None
        self.rejected = None

    def feedback(self, rail, packet, index, cack, units):
        # Only the trigger is selectively acknowledged. In particular no
        # bitmap accidentally delivers the deliberately rejected target.
        body = struct.pack('!BBHIHH', 0, 0, (index - cack) & 0xffff,
                           packet.entropy, *PDC_IDS)
        body += struct.pack('!IBBhQ', psn(cack), 0, units, 1, 0)
        body += struct.pack('!HHI', 0, 0, (self.received_bytes + 255) // 256)
        self.reply(rail, 0xDC, psn(cack), body, packet.bth.flags & RTX)
        self.sacks_sent += 1

    def acknowledge_probe(self, rail, packet, token, timestamp):
        # Suppress probe feedback during the experiment: it must not advance
        # CACK, return extra credits, or change the advertised window.
        if not self.held:
            super().acknowledge_probe(rail, packet, token, timestamp)

    def acknowledge_write(self, rail, packet, index):
        self.held[index] = (rail, packet)
        if index == self.TARGET:
            self.feedback(rail, packet, index, index, 1 if self.case == "shrink" else 2)
            self.finish_write(rail)
        elif index > 1 and self.shrunk_at is None:
            self.feedback(rail, packet, index, -1, 2)

    def write(self, rail, packet):
        index = (packet.bth.psn - INITIAL_PSN) & PSN_MASK
        if index == self.TARGET:
            if self.shrunk_at is None:
                require(not packet.bth.flags & RTX, 'Target was not an initial transmission')
                require(self.rejected is None, 'Repeated target before shrink setup')
                self.target_rail = rail
                self.rejected = (rail, packet)
                return
            require(self.reopened_at is not None,
                    'Requester retransmitted a NACKed PSN outside the shrunken MPR')
            require(packet.bth.flags & RTX, 'Reopened target did not carry RTX')
            require(rail != self.target_rail, 'NACK retry did not switch rails')
            self.retry_at = time.monotonic()
        super().write(rail, packet)

    def tick(self, now):
        if self.rejected is not None and self.shrunk_at is None and self.rails_observed == 3 and all(
                i in self.received for i in range(self.TARGET)):
            rail, packet = self.rejected
            self.shrunk_at = now
            # Ordered on one rail: first advance CACK to zero and shrink,
            # then NACK the already transmitted out-of-window target.
            if self.case == "stale-cack":
                self.feedback(rail, self.held[0][1], 0, 0, 2)
            cack = -1 if self.case in ("equal-cack", "stale-cack") else 0
            units = {"zero": 0, "oversized": 3}.get(self.case, 1)
            self.feedback(rail, self.held[0][1], 0, cack, units)
            if self.case != "shrink":
                self.reopened_at = now  # Invalid/static feedback must not close admission.
            self.nack(rail, packet, NO_PKT_BUFFER)
        if self.case != "shrink" and self.shrunk_at is not None:
            require(self.retry_at is not None or now - self.shrunk_at < 0.2,
                    f"{self.case} MPR feedback incorrectly withheld an eligible NACK retry")
        if self.shrunk_at is not None and self.reopened_at is None and now - self.shrunk_at >= 0.1:
            # Advance CACK so the rejected PSN is now inside the SAME small
            # window; no growth or semantic ACK may release it instead.
            rail, packet = self.held[self.TARGET - 1]
            self.reopened_at = now
            self.feedback(rail, packet, self.TARGET - 1, self.TARGET - 1, 1)


def exchange(listener, total, dynamic):
    connection, _ = listener.accept()
    with connection:
        connection.settimeout(3)
        offer = bytearray()
        while len(offer) < 64:
            chunk = connection.recv(64 - len(offer))
            require(chunk, 'OOB peer closed before its offer')
            offer += chunk
        require(offer[:4] == b'MRCQ' and bool(struct.unpack_from('!I', offer, 12)[0] & 2) == dynamic,
                'Requester did not offer Dynamic MPR')
        # Independent lab OOB record, not the implementation codec.
        reply = bytearray(64)
        struct.pack_into('!4sHHIIIIIHBBHHHHQQI', reply, 0,
                         b'MRCQ', 1, 64, 7, 2 if dynamic else 0, RESPONDER, INITIAL_PSN, 1, 1,
                         2, 1, 1, 1024, PKEY, 0, ADDRESS, total, RKEY)
        connection.sendall(reply)


def run(binary, relay_binary, mode, case="shrink"):
    cpus = sorted(os.sched_getaffinity(0))
    require(len(cpus) >= 2, 'Two polling cores are required')
    with tempfile.TemporaryDirectory(prefix='mrc-mpr-') as temporary, \
            socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as wire, \
            socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        directory = Path(temporary)
        wire.bind(str(directory / 'fixture.sock'))
        wire.settimeout(0.005)
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        listener.settimeout(4)
        relay = start(eal_args(relay_binary, 'rx', cpus[0], directory) +
                      ['--', str(directory / 'relay.sock'), str(directory / 'fixture.sock')], directory)
        requester = None
        try:
            deadline = time.monotonic() + 4
            while not (directory / 'relay.sock').exists():
                require(relay.poll() is None and time.monotonic() < deadline, 'Relay startup failed')
                time.sleep(0.01)
            wire.connect(str(directory / 'relay.sock'))
            peer = MprResponder(wire, mode, case)
            total = sum(peer.write_lengths)
            command = eal_args(binary, 'tx', cpus[1], directory) + [
                '--', 'tx', f'{total}@0xffffffff', str(peer.addresses['tx']), str(peer.addresses['rx']),
                '0', MACS['rx'][0], '1', MACS['rx'][1], '--initial-psn', str(INITIAL_PSN),
                '--requestor-qpn', str(REQUESTOR), '--responder-qpn', str(RESPONDER),
                '--p-key', str(PKEY), '--mpr', '2', '--dynamic-mpr', '0' if case == 'static' else '1',
                '--oob-host', '127.0.0.1', '--oob-port', str(listener.getsockname()[1]),
                '--ack-timeout', '20', '--timeout-ms', '3000', '--max-wimm-inflight', '1']
            if peer.srv6:
                command += ['--srv6-local', str(SIDS['tx']), '--srv6-peer', str(SIDS['rx'])]
                if peer.srh:
                    command += ['--srv6-srh', '1']
            requester = start(command, directory)
            exchange(listener, total, case != "static")
            deadline = time.monotonic() + 4
            while not peer.finished:
                require(time.monotonic() < deadline, 'MPR exchange did not complete')
                peer.tick(time.monotonic())
                try:
                    peer.receive(wire.recv(4096))
                except TimeoutError:
                    require(requester.poll() is None, 'Requester exited before MPR completion')
            wait_relay_quiesced(wire, relay, peer)
            counters = collect(requester)
            require(peer.memory == payload(0, total), 'MPR retry corrupted placement')
            require(counters['mpr_updates'] == (case == 'shrink') and counters['nack_received_mask'] == 1 << NO_PKT_BUFFER
                    and sum(counters['retries']) == 1 and counters['write_completions'] == 1,
                    f'Unexpected MPR counters: {counters}')
            require(peer.retry_at is not None and peer.retry_at >= peer.reopened_at,
                    'No retry after the CACK reopened the window')
            stop_relay(wire, relay)
            return {'mode': mode, 'case': case, 'withheld_ms': round(1000 * (peer.reopened_at - peer.shrunk_at)),
                    'resume_ms': round(1000 * (peer.retry_at - peer.reopened_at)), **counters}
        except Exception as error:
            diagnostics = []
            for name, process in (("requester", requester), ("relay", relay)):
                if process is not None:
                    if process.poll() is None:
                        process.kill()
                    stdout, stderr = process.communicate()
                    diagnostics.append(f"{name}: {process.returncode}\n{stdout}\n{stderr}")
            raise RuntimeError(f"{error}\n" + "\n".join(diagnostics)) from error
        finally:
            for process in (requester, relay):
                if process is not None:
                    if process.poll() is None:
                        process.kill()
                    process.communicate()


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=root / 'mrc_write')
    parser.add_argument('--relay', type=Path, default=root / 'tests/mrc_memif_relay')
    parser.add_argument('--mode', action='append', choices=MODES)
    parser.add_argument('--case', action='append', choices=CASES)
    args = parser.parse_args()
    for mode in args.mode or MODES:
        for case in args.case or CASES:
            print(json.dumps(run(args.binary.resolve(), args.relay.resolve(), mode, case)), flush=True)


if __name__ == '__main__':
    main()
