#!/usr/bin/env python3
"""Check production SRv6 encoding against Linux NEXT-CSID forwarding.

RFC 9800 sections 4.1, 4.1.1, 6.2 and 6.3:
https://datatracker.ietf.org/doc/html/rfc9800
Run in an empty user/network namespace (unshare -Urn). No physical links
are touched. Each captured packet is reinjected for the next hop so every
DA shift and SRH promotion can be checked against an independent oracle.
"""
import argparse
import ipaddress
import json
import socket
import struct
import subprocess
import time
from contextlib import ExitStack
from pathlib import Path

from check_mrc_srv6_fabric import open_namespace, namespace_run, run
from memif_pair import require
from mrc_wire_reference import Bth, IPPeer, encode_ipv4, encode_ipv6

MAC_SEND = bytes.fromhex('020000000001')
MAC_IN = bytes.fromhex('020000000002')
MAC_OUT = bytes.fromhex('020000000003')
MAC_RECV = bytes.fromhex('020000000004')


def mac_text(value):
    return ':'.join(f'{x:02x}' for x in value)


def container(block, lblen, csids, nflen=16):
    require(lblen + len(csids) * nflen <= 128, 'CSID vector exceeds container')
    value = int(ipaddress.IPv6Address(block)) >> (128 - lblen)
    for csid in csids:
        value = (value << nflen) | csid
    value <<= 128 - lblen - len(csids) * nflen
    return ipaddress.IPv6Address(value).packed


def inner_frame(ipv6):
    source = IPPeer(MAC_SEND, ipaddress.ip_address('2001:db8:1::1' if ipv6 else '192.0.2.1').packed)
    target = IPPeer(MAC_IN, ipaddress.ip_address('2001:db8:1::2' if ipv6 else '192.0.2.2').packed)
    # Valid WRITE_ONLY_IMM with a three-byte payload and one pad byte.
    data = struct.pack('!HHQIII', 1, 1, 0x123400000, 0xdeadbeef, 3, 0x12345678) + b'MRC'
    return (encode_ipv6 if ipv6 else encode_ipv4)(
        source, target, 0xc123, 4971, Bth(0xcb, 0xffff, 0x12345, 0xfffffe), data)


def encoded(binary, inner, initial, segments, left):
    args = [str(binary), str(ipaddress.IPv6Address(initial)), str(left)]
    args += [str(ipaddress.IPv6Address(segment)) for segment in segments]
    result = subprocess.run(args, input=inner, capture_output=True, timeout=3)
    require(result.returncode == 0, f'Production encoder failed: {result.stderr!r}')
    frame = result.stdout
    next_header = 41 if inner[14] >> 4 == 6 else 4
    srh = (struct.pack('!BBBBBBH', next_header, 2 * len(segments), 4, left,
                       len(segments) - 1, 0, 0) + b''.join(segments)) if segments else b''
    outer = struct.pack('!IHBB', 6 << 28 | 42 << 20 | 0x12345,
                        len(srh) + len(inner) - 14, 43 if segments else next_header, 64)
    outer += ipaddress.IPv6Address('2001:db8::feed').packed + initial
    expected = inner[:12] + b'\x86\xdd' + outer + srh + inner[14:]
    require(frame == expected, 'Production SRv6 header differs from independent RFC encoding')
    return frame


def next_hop(frame, lblen, nflen):
    data = bytearray(frame)
    da = int.from_bytes(data[38:54], 'big')
    argument_bits = 128 - lblen - nflen
    argument = da & ((1 << argument_bits) - 1)
    if argument:
        prefix = da >> (128 - lblen) << (128 - lblen)
        new_da = prefix | argument << nflen
        action = 'shift'
    elif data[20] == 43 and data[57]:
        data[57] -= 1
        new_da = int.from_bytes(data[62 + 16 * data[57]:78 + 16 * data[57]], 'big')
        action = 'promote'
    else:
        return None
    data[38:54] = new_da.to_bytes(16, 'big')
    data[21] -= 1
    return bytes(data), action


def kernel_hop(router, sender, receiver, frame, expected, lblen, nflen):
    active = ipaddress.IPv6Network((ipaddress.IPv6Address(frame[38:54]), lblen + nflen), strict=False)
    next_da = str(ipaddress.IPv6Address(expected[38:54]))
    namespace_run(router, 'ip', '-6', 'route', 'add', str(active), 'encap', 'seg6local',
                  'action', 'End', 'flavors', 'next-csid', 'lblen', str(lblen), 'nflen', str(nflen),
                  'count', 'dev', 'ingress')
    namespace_run(router, 'ip', '-6', 'route', 'add', next_da + '/128', 'dev', 'egress')
    namespace_run(router, 'ip', '-6', 'neigh', 'replace', next_da, 'lladdr',
                  mac_text(MAC_RECV), 'nud', 'permanent', 'dev', 'egress')
    try:
        sender.send(MAC_IN + MAC_SEND + frame[12:])
        deadline = time.monotonic() + 2
        while True:
            require(time.monotonic() < deadline, f'Kernel did not forward {active} to {next_da}')
            captured = receiver.recv(4096)
            if captured[12:14] == b'\x86\xdd' and captured[22:38] == frame[22:38]:
                break
        require(captured[14:] == expected[14:],
                f'Kernel NEXT-CSID result differs from independent oracle for {active}: {captured.hex()}')
        routes = json.loads(namespace_run(router, 'ip', '-6', '-j', '-s', 'route', 'show', 'exact', str(active)))
        require(len(routes) == 1 and routes[0]['stats64']['packets'] == 1
                and routes[0]['stats64']['errors'] == 0, f'No successful NEXT-CSID processing: {routes}')
        return captured
    finally:
        namespace_run(router, 'ip', '-6', 'route', 'del', str(active))
        namespace_run(router, 'ip', '-6', 'route', 'del', next_da + '/128')
        namespace_run(router, 'ip', '-6', 'neigh', 'del', next_da, 'dev', 'egress')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--encoder', type=Path, default=Path(__file__).resolve().parent / 'mrc_srv6_encode')
    args = parser.parse_args()
    require([item['ifname'] for item in json.loads(run('ip', '-j', 'link', 'show'))] == ['lo'],
            'Run inside an empty network namespace with unshare -Urn')
    with ExitStack() as resources:
        router = open_namespace(resources)
        namespace_run(router, 'sysctl', '-qw', 'net.ipv6.conf.all.forwarding=1')
        for host, remote, local_mac, remote_mac in (
                ('send', 'ingress', MAC_SEND, MAC_IN), ('recv', 'egress', MAC_RECV, MAC_OUT)):
            run('ip', 'link', 'add', host, 'address', mac_text(local_mac), 'type', 'veth',
                'peer', 'name', remote, 'address', mac_text(remote_mac), 'netns', str(router.pid))
            resources.callback(run, 'ip', 'link', 'del', host)
            run('ip', 'link', 'set', host, 'up')
            namespace_run(router, 'ip', 'link', 'set', remote, 'up')
            namespace_run(router, 'sysctl', '-qw', f'net.ipv6.conf.{remote}.seg6_enabled=1')
        sender = resources.enter_context(socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x86dd)))
        receiver = resources.enter_context(socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x86dd)))
        sender.bind(('send', 0))
        receiver.bind(('recv', 0))
        receiver.settimeout(2)
        for lblen, nflen, block in ((32, 16, 'fcbb:bbbb::'), (48, 16, 'fcbb:bbbb:aaaa::'),
                                    (32, 32, 'fcbb:bbbb::')):
            capacity = (128 - lblen) // nflen
            first = container(block, lblen, list(range(1, capacity + 1)), nflen)
            second = container(block, lblen, [0x101, 0x102, 0x103], nflen)
            short = container(block, lblen, [1, 2], nflen)
            other = container('fcbb:cccc:dddd::', lblen, [0x201, 0x202], nflen)
            for name, initial, segments, left in (
                    ('no-srh', short, [], 0),
                    ('debug-copy', short, [short], 0),
                    ('reduced', first, [second], 1),
                    ('full', first, [second, first], 1),
                    ('reduced-other-block', short, [other], 1)):
                for ipv6 in (False, True):
                    inner = inner_frame(ipv6)
                    frame = encoded(args.encoder.resolve(), inner, initial, segments, left)
                    hops = {'shift': 0, 'promote': 0}
                    while (step := next_hop(frame, lblen, nflen)) is not None:
                        expected, action = step
                        frame = kernel_hop(router, sender, receiver, frame, expected, lblen, nflen)
                        hops[action] += 1
                    srh_bytes = 8 + 16 * len(segments) if segments else 0
                    require(frame[54 + srh_bytes:] == inner[14:], 'Forwarding changed inner datagram/iCRC')
                    result = subprocess.run([str(args.encoder.resolve()), 'decode'], input=frame,
                                            capture_output=True, timeout=3)
                    require(result.returncode == 0, f'Production terminal decode failed: {result.stderr!r}')
                    print(json.dumps({'case': name, 'lblen': lblen, 'nflen': nflen,
                                      'inner': 6 if ipv6 else 4, **hops}), flush=True)


if __name__ == '__main__':
    main()
