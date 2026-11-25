#!/usr/bin/env python3
"""Render the host-specific units for the Spark three-node double ring."""

import argparse
import re
from pathlib import Path
from typing import NamedTuple


class Endpoint(NamedTuple):
    ip: str
    macs: tuple[str, str]


class Spark(NamedTuple):
    hostname: str
    tx: Endpoint
    rx: Endpoint


SPARKS = {
    "02": Spark(
        "spark-4ed9",
        tx=Endpoint("10.61.0.10", ("4c:bb:47:82:4e:da", "4c:bb:47:82:4e:de")),
        rx=Endpoint("10.61.0.1", ("4c:bb:47:82:4e:db", "4c:bb:47:82:4e:df")),
    ),
    "03": Spark(
        "spark-016e",
        tx=Endpoint("10.61.0.6", ("4c:bb:47:2c:01:6f", "4c:bb:47:2c:01:73")),
        rx=Endpoint("10.61.0.9", ("4c:bb:47:2c:01:70", "4c:bb:47:2c:01:74")),
    ),
    "04": Spark(
        "spark-7e64",
        tx=Endpoint("10.61.0.2", ("4c:bb:47:2e:7e:65", "4c:bb:47:2e:7e:69")),
        rx=Endpoint("10.61.0.5", ("4c:bb:47:2e:7e:66", "4c:bb:47:2e:7e:6a")),
    ),
}
TX_PEER = {"04": "02", "02": "03", "03": "04"}
RX_PEER = {receiver: sender for sender, receiver in TX_PEER.items()}
BASE_PROFILES = ("smoke", "failover")
PROFILES = (*BASE_PROFILES, "negotiated")
TRANSFER_BYTES = 256 * 1024 * 1024
NEGOTIATED_REQUESTS = 8


def oob_port(receiver):
    return str(46000 + int(receiver))


def snapshot_path(value):
    if not re.fullmatch(r"/opt/ocs-mrc/[A-Za-z0-9][A-Za-z0-9._-]*", value):
        raise argparse.ArgumentTypeError("expected a snapshot directory under /opt/ocs-mrc")
    return Path(value)


def render_unit(host, snapshot, role, profile):
    spark = SPARKS[host]
    if role == "tx":
        local, peer = spark.tx, SPARKS[TX_PEER[host]].rx
        lcore = "3"
        first_pci, second_pci = "0000:01:00.0", "0002:01:00.0"
    else:
        local, peer = spark.rx, SPARKS[RX_PEER[host]].tx
        lcore = "2"
        first_pci, second_pci = "0000:01:00.1", "0002:01:00.1"

    runtime_dir = f"ocs-mrc-cycle-{role}"
    bytes_per_request = TRANSFER_BYTES // NEGOTIATED_REQUESTS if profile == "negotiated" else TRANSFER_BYTES
    first_mac, second_mac = peer.macs
    library_dir = snapshot / "lib"
    command = [
        # Pair tests hold the common lock exclusively. The two cycle roles
        # can share it while each holds its own exclusive profile lock.
        "/usr/bin/flock", "--shared", "--nonblock", "/run/ocs-mrc/runner.lock",
        "/usr/bin/flock", "--exclusive", "--nonblock", f"/run/ocs-mrc/cycle-{role}.lock",
        str(snapshot / "mrc_write"),
        "-l", lcore, "--iova-mode=va", "--no-huge", f"--file-prefix={runtime_dir}",
        "-d", str(library_dir / "librte_mempool_ring.so.24"),
        "-d", str(library_dir / "librte_net_mlx5.so.24"),
        "-a", first_pci, "-a", second_pci,
        "--", role, str(bytes_per_request), local.ip, peer.ip,
        first_pci, first_mac, second_pci, second_mac,
        "--udp-port", "4971", "--timeout-ms", "120000", "--port-stats", "1",
    ]
    if profile == "failover":
        command += ["--ack-timeout", "13"]
        if host == "04" and role == "tx":
            command += ["--drop-path-after", "512", "--restore-path-after", "2048"]
    elif profile == "negotiated":
        receiver = host if role == "rx" else TX_PEER[host]
        command += ["--oob-host", "127.0.0.1", "--oob-port", oob_port(receiver),
                    "--work-requests", str(NEGOTIATED_REQUESTS), "--immediate", "99",
                    "--max-wimm-inflight", "2", "--mpr", "8", "--dynamic-mpr", "1"]
        if role == "rx":
            command += ["--responder-qpn", str(0x250000 + int(host)), "--initial-psn", "16777212",
                        "--mpr-change-at", "65", "--mpr-change-to", "1"]
        else:
            command += ["--requestor-qpn", str(0x140000 + int(host))]

    return f"""[Unit]
Description=OCS MRC Spark-{host} cycle {profile} ({role})
ConditionHost={spark.hostname}

[Service]
Type=exec
User=ocs-mrc
DynamicUser=yes
WorkingDirectory={snapshot}
Environment=LD_LIBRARY_PATH={library_dir}
Environment=XDG_RUNTIME_DIR=/run/{runtime_dir}
# systemd joins RuntimeDirectory paths with a colon. DPDK treats it as one path.
UnsetEnvironment=RUNTIME_DIRECTORY
RuntimeDirectory=ocs-mrc {runtime_dir}
RuntimeDirectoryPreserve=yes
NoNewPrivileges=yes
CapabilityBoundingSet=CAP_NET_RAW CAP_NET_ADMIN
AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN
LimitMEMLOCK=1G
ProtectHome=yes
ProtectSystem=strict
PrivateTmp=yes
RuntimeMaxSec=150
ExecStart={' '.join(command)}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", choices=SPARKS, required=True)
    parser.add_argument("--snapshot", type=snapshot_path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--include-negotiated", action="store_true",
                        help="also render the negotiated WRITEIMM and Dynamic MPR units")
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    profiles = PROFILES if args.include_negotiated else BASE_PROFILES
    for role in ("rx", "tx"):
        for profile in profiles:
            filename = f"ocs-mrc-cycle-{role}-{profile}.service"
            unit = render_unit(args.host, args.snapshot, role, profile)
            (args.output_dir / filename).write_text(unit, encoding="utf-8")


if __name__ == "__main__":
    main()
