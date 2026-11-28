#!/usr/bin/env python3
"""Run the full-duplex scatter/gather baseline with one physical PCIe path per peer."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
from dataclasses import dataclass, replace
from pathlib import Path
import hashlib
import socket
import statistics
import sys
import tempfile
import time

import check_spark_bandwidth as bw
import check_spark_ring_bandwidth as ring
import peer_vf

RELEASE = Path("/opt/ocs-roce-app-bw/8c5058a-aggregate")
LAUNCHER = "/usr/local/libexec/ocs-roce-ring-gather-app"
POOL = "ocs-roce-ring-two-way-pool.service"
FORWARD_PF = peer_vf.PARENTS["reverse-rx"]
REVERSE_PF = peer_vf.PARENTS["forward-rx"]
INTERFACES = {FORWARD_PF: "enp1s0f0np0", REVERSE_PF: "enP2p1s0f1np1"}
APP_CPUS = {"forward-tx": 9, "forward-rx": 19, "reverse-tx": 4, "reverse-rx": 14}
PAGE_DIR = Path("/sys/kernel/mm/hugepages/hugepages-2048kB")


@dataclass(frozen=True)
class Endpoint(ring.Endpoint):
    reverse: bool = False
    shared_hugepages = True

    @property
    def name(self):
        return ("reverse-" if self.reverse else "forward-") + self.role

    @property
    def pci(self):
        # The physical function identifies the peer cable and PCIe uplink.
        if self.role == "rx":
            return peer_vf.PARENTS[self.name]
        return REVERSE_PF if self.reverse else FORWARD_PF

    @property
    def interface(self):
        return INTERFACES[self.pci]

    @property
    def unit(self):
        return f"{peer_vf.UNIT}{self.name}.service"

    @property
    def runtime_dir(self):
        return Path("/run") / self.unit.removesuffix(".service")

    @property
    def cpus(self):
        if self.reverse:
            return (0, 1, 2, 3) if self.role == "tx" else (10, 11, 12, 13)
        return super().cpus

    @property
    def settings(self):
        return replace(super().settings, pmtu=4096, app_mode="memfd-gather",
                       prefault_app=True, hugepages=True)

    @property
    def profile(self):
        return "app-ring-gather-" + self.name


@dataclass(frozen=True)
class Pool:
    host: str
    unit: str = POOL
    label = POOL


def edges():
    forward = [(Endpoint(tx.host, tx.role, tx.port, tx.data_ip),
                Endpoint(rx.host, rx.role, rx.port, rx.data_ip)) for tx, rx in ring.EDGES]
    # Control and wire IPs match the reverse-direction units already on the Sparks.
    reverse_ports = (29133, 29132, 29131)
    reverse_ips = ((13, 14), (21, 22), (17, 18))
    reverse = [(Endpoint(rx.host, "tx", port, f"10.61.0.{ips[0]}", True),
                Endpoint(tx.host, "rx", port, f"10.61.0.{ips[1]}", True))
               for (tx, rx), port, ips in zip(forward, reverse_ports, reverse_ips, strict=True)]
    for (tx, rx), (back_tx, back_rx) in zip(forward, reverse, strict=True):
        bw.require(tx.pci == back_rx.pci and rx.pci == back_tx.pci, "Asymmetric per-peer PCIe path")
    return forward + reverse


def unit_argv(endpoint, destination, runtime):
    device = peer_vf.PLACEHOLDER if endpoint.role == "rx" else endpoint.pci
    argv = ring.unit_argv(endpoint, destination, runtime)
    argv[argv.index(str(destination / "roce_write"))] = LAUNCHER
    idx = argv.index("--no-huge")
    argv[idx:idx + 1] = [f"--huge-dir=/dev/hugepages/ocs-roce-ring-two-way/{endpoint.name}",
                       "--socket-mem=128", "--huge-unlink=always"]
    argv[argv.index(f"--file-prefix=ocs-roce-ring-{endpoint.role}")] = "--file-prefix=" + endpoint.runtime_dir.name
    argv[argv.index("-a") + 1] = device + ",max_dump_files_num=1"
    idx = argv.index("--")
    argv[idx + 6:idx + 8] = ["4096", device]
    argv.append("--prefault-app")
    if endpoint.role == "tx":
        argv += ["--app-post-batch", "32", "--scatter-gather"]
    return argv


def page_counts(commands, host):
    return [int(value) for value in commands.run(host, "cat", *(PAGE_DIR / name for name in
            ("nr_hugepages", "free_hugepages", "resv_hugepages", "surplus_hugepages"))).split()]


def verify_idle(commands, host):
    bw.require(page_counts(commands, host) == [0] * 4, f"{host}: hugepages are in use")
    for pci, interface in INTERFACES.items():
        state = commands.run(host, "cat", peer_vf.PCI / pci / "sriov_numvfs",
                             Path("/sys/class/net") / interface / "mtu").split()
        bw.require(state == ["0", "1500"], f"{host} {pci}: expected idle VF count and MTU")


def preflight(commands, paths):
    for host in ring.HOSTS:
        bw.require_idle(commands, host)
        verify_idle(commands, host)
        bw.verify_service_access(commands, Pool(host))
        bw.verify_deployed_file(commands, host, peer_vf.HELPER)
        bw.require(commands.run(host, "sha256sum", peer_vf.HELPER).split()[0] ==
                   hashlib.sha256(Path(peer_vf.__file__).read_bytes()).hexdigest(),
                   f"{host}: installed VF helper differs from this controller")
        bw.require(commands.run(host, "sha256sum", RELEASE / "roce_write").split()[0] ==
                   "bcb390549d7d69a0a04f829eb3a28b74d6f228963601f50db7b40aa898e42c5b",
                   f"{host}: unexpected DPDK worker build")
    for pair in paths:
        for endpoint in pair:
            ring.verify_unit(commands, endpoint, RELEASE, build_argv=unit_argv)
            props = commands.properties(endpoint, "Environment", "BindsTo", "After")
            bw.require(f"ROCE_RING_APP_CPU={APP_CPUS[endpoint.name]}" in props["Environment"].split()
                       and POOL in props["BindsTo"].split() and POOL in props["After"].split(),
                       f"{endpoint.label}: incorrect pool dependency or application pinning")
            ring.verify_link(commands, endpoint)


def run_sample(commands, output, sample, paths):
    senders, receivers = zip(*paths)
    invocations = {}
    devices = {}
    try:
        ring.start_group(commands, receivers, invocations, timeout=75)
        deadline = time.monotonic() + 90

        def verify_receiver(rx):
            messages = bw.wait_endpoint(commands, rx, invocations[rx], deadline, ready=True)
            vf = Path(commands.run(rx.host, "realpath", "-e", peer_vf.PCI / rx.pci / "virtfn0").strip()).name
            parent = Path(commands.run(rx.host, "realpath", "-e", peer_vf.PCI / vf / "physfn").strip()).name
            bw.require(parent == rx.pci and f"DPDK port 0: {vf}" in messages,
                       f"{rx.label}: receiver did not open its peer's VF")
            bw.verify_loopback_listener(commands, rx.host, rx.port)
            return rx, vf

        with ThreadPoolExecutor(max_workers=len(receivers)) as pool:
            devices.update(pool.map(verify_receiver, receivers))
        timing = ring.release_ring(commands, output, sample, invocations, senders=senders, timeout=90)
        results = {}
        deadline = time.monotonic() + 120
        for endpoint in (*senders, *receivers):
            messages = bw.wait_endpoint(commands, endpoint, invocations[endpoint], deadline)
            ring.log_path(output, sample, endpoint).write_text("\n".join(messages) + "\n")
            device = devices.get(endpoint, endpoint.pci)
            bw.require(f"DPDK port 0: {device}" in messages, f"{endpoint.label}: unexpected DPDK port")
            results[endpoint] = dict(bw.parse_result(endpoint, messages), device=device,
                                     physical_function=endpoint.pci, cpu_ids=endpoint.cpus)
        trial = ring.summarize(timing, results, paths)
        bw.require(trial["overlap_lower_bound_ns"] > 0, "The six transfers did not overlap")
        # Translate local starts to controller-time bounds using each gate exchange.
        starts, ends, durations = [], [], []
        for tx in senders:
            gate = timing[tx.host]
            delay = int(results[tx]["start_ns"]) - gate["sender_gate_ns"] + ring.GATE_LEAD_NS
            interval = [gate[key] + delay for key in ("controller_sent_ns", "controller_received_ns")]
            duration = int(results[tx]["elapsed_ns"])
            starts.append(interval)
            ends.append([value + duration for value in interval])
            durations.append(duration)
        shortest = max(max(durations), max(a for a, _ in ends) - min(b for _, b in starts))
        longest = max(b for _, b in ends) - min(a for a, _ in starts)
        trial["aggregate_payload_gbps_bounds"] = [8 * trial["verified_payload_bytes"] / ns
                                                 for ns in (longest, shortest)]
        return trial
    finally:
        ring.cleanup_invocations(commands, output, sample, invocations)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--check", action="store_true", help="verify installed profile without starting services")
    args = parser.parse_args()
    if args.samples <= 0 or socket.gethostname().split(".")[0] != "spark-7e64":
        parser.error("Use a positive sample count and launch from Spark-04")
    paths = edges()
    with tempfile.TemporaryDirectory(prefix="roce-peer-control-") as sockets:
        commands = bw.Commands(sockets)
        preflight(commands, paths)
        if args.check:
            print("Verified all twelve peer-ring worker services.")
            return
        output = Path(tempfile.mkdtemp(prefix="roce-peer-ring-results-"))
        print(f"Peer ring output {output}", flush=True)
        report = {"placement": "per-peer", "ethernet_mtu": 4200, "roce_pmtu": 4096,
                  "packet_page_bytes": 2 << 20, "tx_mode": "scatter-gather", "trials": []}
        with ExitStack() as cleanup:
            cleanup.enter_context(ring.control_tunnels(commands, sockets, paths))
            for host in ring.HOSTS:
                pool = Pool(host)
                invocation = bw.start(commands, pool)
                cleanup.callback(verify_idle, commands, host)
                cleanup.callback(bw.stop_owned, commands, pool, invocation)
                bw.require(page_counts(commands, host) == [256, 256, 0, 0], f"{host}: incorrect page pool")
            for pair in paths:
                for endpoint in pair:
                    bw.require(ring.verify_link(commands, endpoint)["ethernet_mtu"] == 4200,
                               f"{endpoint.label}: wrong Ethernet MTU")
            for sample in range(1, args.samples + 1):
                trial = run_sample(commands, output, sample, paths)
                report["trials"].append(trial)
                bounds = trial["aggregate_payload_gbps_bounds"]
                for index, label in enumerate(("lower", "upper")):
                    report[f"median_ring_payload_gbps_{label}"] = statistics.median(
                        t["aggregate_payload_gbps_bounds"][index] for t in report["trials"])
                bw.save_report(output, report)
                print(f"Trial {sample}: {bounds[0]:.2f}–{bounds[1]:.2f} Gb/s; verified 96 GiB", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, bw.CycleError) as exc:
        sys.exit(str(exc))
