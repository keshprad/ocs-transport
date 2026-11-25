#!/usr/bin/env python3
"""Check terminal relay enqueue, frame rejection and bounded stop timeout.

Uses the pcap PMD rather than a peer process so failures isolate the relay
control protocol. Select the same pcap-enabled DPDK runtime as the wire tests.
"""
import argparse
import json
import os
import socket
import tempfile
import time
from pathlib import Path

from check_mrc_pcap import read_capture, write_capture
from check_mrc_requester import RELAY_QUIESCED
from memif_pair import require, start


def check(binary, case):
    cpu = min(os.sched_getaffinity(0))
    frame = bytes.fromhex('02000000000102000000000286dd') + bytes(50)
    with tempfile.TemporaryDirectory(prefix='mrc-relay-protocol-') as tmp, \
            socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as wire:
        directory = Path(tmp)
        write_capture(directory / 'empty.pcap', [])
        wire.bind(str(directory / 'fixture.sock'))
        wire.settimeout(2)
        command = [str(binary), f'--lcores=0@{cpu}', '--no-huge', '--no-pci', '--no-telemetry',
                   f'--file-prefix={directory.name}', '-m', '64']
        for rail in range(2):
            command += [f'--vdev=net_pcap{rail},rx_pcap={directory / "empty.pcap"},'
                        f'tx_pcap={directory / f"out{rail}.pcap"}']
        command += ['--', str(directory / 'relay.sock'), str(directory / 'fixture.sock')]
        relay = start(command, directory)
        try:
            deadline = time.monotonic() + 3
            while not (directory / 'relay.sock').exists():
                if relay.poll() is not None:
                    out, err = relay.communicate()
                    raise RuntimeError(f'Relay startup failed: {out}\n{err}')
                require(time.monotonic() < deadline, 'Relay startup timed out')
                time.sleep(0.01)
            wire.connect(str(directory / 'relay.sock'))
            sent_at = time.monotonic()
            wire.send(b'\x80' + frame)
            require(wire.recv(2048) == RELAY_QUIESCED, 'Missing terminal enqueue acknowledgement')
            if case == 'stop':
                wire.send(b'')
            elif case == 'frame-after-terminal':
                wire.send(b'\x00' + frame)
            out, err = relay.communicate(timeout=7)
            if case == 'stop':
                require(relay.returncode == 0, f'Explicit stop failed: {out}\n{err}')
            else:
                expected = 'Protocol error' if case == 'frame-after-terminal' else 'quiesce timed out waiting for stop'
                require(relay.returncode != 0 and expected in err,
                        f'Missing bounded protocol rejection: {out}\n{err}')
            frames = list(read_capture(directory / 'out0.pcap'))
            require(frames == [frame] and list(read_capture(directory / 'out1.pcap')) == [],
                    'Terminal feedback was not enqueued exactly once before quiescence')
            elapsed = time.monotonic() - sent_at
            if case == 'missing-stop':
                require(4.8 <= elapsed < 6.5, f'Unexpected quiescence timeout: {elapsed}')
            print(json.dumps({'case': case, 'terminal_frames': len(frames),
                              'elapsed_ms': round(elapsed * 1000)}), flush=True)
        finally:
            if relay.poll() is None:
                relay.kill()
            relay.communicate()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--relay', type=Path, default=Path(__file__).resolve().parent / 'mrc_memif_relay')
    args = parser.parse_args()
    for case in ('stop', 'frame-after-terminal', 'missing-stop'):
        check(args.relay.resolve(), case)


if __name__ == '__main__':
    main()
