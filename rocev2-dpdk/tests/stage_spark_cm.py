#!/usr/bin/env python3
"""Stage the fixed Spark-04 DPDK services and Spark-02 stock CM peer."""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import tempfile


RUNTIME = Path("/opt/ocs-mrc/32d67ee.sYuPGvAf/lib")
INSTALL_ROOT = Path("/opt/ocs-roce-cm")
PROFILES = (("tx", 1500, 1024, 28886), ("rx", 1500, 1024, 28887),
            ("tx", 4200, 4096, 28888), ("rx", 4200, 4096, 28889))

PEER_INSTALLER = """#!/bin/bash
set -euo pipefail
[[ $EUID == 0 && $(hostname -s) == spark-4ed9 ]] || {
    echo 'Run as root on Spark-02 (spark-4ed9).' >&2
    exit 1
}
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
sha256sum --check --strict SHA256SUMS
/usr/sbin/visudo -cf ocs-roce-cm-peer.sudoers
/usr/bin/install -o root -g root -m 0440 ocs-roce-cm-peer.sudoers /etc/sudoers.d/ocs-roce-cm-peer
echo 'Spark-02 RoCEv2 CM test port permissions installed.'
"""

APP_LAUNCHER = """#!/bin/bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
: "${XDG_RUNTIME_DIR:?missing service runtime directory}"
socket="$XDG_RUNTIME_DIR/application.sock"
stdout="$XDG_RUNTIME_DIR/worker.stdout"
worker=
application=
cleanup() {
    if [[ -n $application ]]; then kill "$application" 2>/dev/null || :; fi
    if [[ -n $worker ]]; then kill "$worker" 2>/dev/null || :; fi
}
trap cleanup EXIT
mkfifo -- "$stdout"
./roce_write "$@" --app-socket "$socket" > "$stdout" &
worker=$!
while IFS= read -r line; do
    printf '%s\\n' "$line"
    if [[ $line == 'application listener ready' && -z $application ]]; then
        /usr/bin/setpriv --ambient-caps=-all ./roce_app_write "$socket" &
        application=$!
    fi
done < "$stdout"
wait "$worker"
worker=
[[ -n $application ]]
wait "$application"
application=
"""


def render_unit(destination, role, mtu, pmtu, port):
    gather = " --scatter-gather" if role == "tx" else ""
    return f"""[Unit]
Description=RoCEv2 ConnectX-7 CM interoperability ({role}, MTU {mtu})
ConditionHost=spark-7e64

[Service]
Type=exec
User=ocs-roce-cm
DynamicUser=yes
WorkingDirectory={destination}
Environment=LD_LIBRARY_PATH={RUNTIME}
Environment=XDG_RUNTIME_DIR=/run/ocs-roce-cm
UnsetEnvironment=RUNTIME_DIRECTORY
RuntimeDirectory=ocs-roce-cm
RuntimeDirectoryMode=0750
NoNewPrivileges=yes
CapabilityBoundingSet=CAP_NET_RAW CAP_NET_ADMIN
AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN
LimitMEMLOCK=1G
ProtectHome=yes
ProtectSystem=strict
PrivateTmp=yes
RuntimeMaxSec=90
ExecStart=/usr/bin/flock --exclusive --nonblock /run/ocs-roce-cm/function.lock {destination}/run_app -l 5 --iova-mode=va --no-huge --no-telemetry --file-prefix=ocs-roce-cm-{role}-{mtu} -d {RUNTIME}/librte_mempool_ring.so.24 -d {RUNTIME}/librte_net_mlx5.so.24 -a 0000:01:00.0 -- {role} 8195 127.0.0.1 {port} 10.61.0.2 {pmtu} 0000:01:00.0 --report-bw --cm-peer 10.61.0.1 4c:bb:47:82:4e:db{gather}
"""


def render_install(destination, units):
    names = " ".join(units)
    return f"""#!/bin/bash
set -euo pipefail
[[ $EUID == 0 && $(hostname -s) == spark-7e64 ]] || {{
    echo 'Run as root on Spark-04 (spark-7e64).' >&2
    exit 1
}}
cd -- "$(dirname -- "${{BASH_SOURCE[0]}}")"
sha256sum --check --strict SHA256SUMS
for path in {RUNTIME} {RUNTIME}/librte_eal.so.24 {RUNTIME}/librte_mempool_ring.so.24 {RUNTIME}/librte_net_mlx5.so.24; do
    read -r uid mode < <(stat -L -c '%u %a' -- "$path")
    [[ $uid == 0 ]] && (( (8#$mode & 8#022) == 0 )) || {{
        printf 'Unsafe DPDK runtime permissions on %s\\n' "$path" >&2
        exit 1
    }}
done
units=({names})
for unit in "${{units[@]}}"; do
    if /usr/bin/systemctl is-active --quiet "$unit"; then
        printf 'Service still active %s\\n' "$unit" >&2
        exit 1
    fi
done
/usr/sbin/visudo -cf ocs-roce-cm.sudoers
/usr/bin/install -d -o root -g root -m 0755 {destination}
for file in roce_write roce_app_write run_app; do
    /usr/bin/install -o root -g root -m 0755 "$file" "{destination}/$file"
done
/usr/bin/systemd-analyze verify --man=no --generators=no --recursive-errors=no "${{units[@]}}"
for unit in "${{units[@]}}"; do
    /usr/bin/install -o root -g root -m 0644 "$unit" "/etc/systemd/system/$unit"
done
/usr/bin/systemctl daemon-reload
/usr/bin/install -o root -g root -m 0440 ocs-roce-cm.sudoers /etc/sudoers.d/ocs-roce-cm
echo 'Spark-04 RoCEv2 CM test services installed.'
"""


def write_manifest(directory, files):
    (directory / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256((directory / name).read_bytes()).hexdigest()}  {name}\n" for name in files
    ))


def stage_peer_access(bundle):
    peer = bundle / "spark-02"
    peer.mkdir(mode=0o700)
    pci = r"pci/0000\:01\:00.1"
    rules = [f"/usr/sbin/devlink dev param set {pci} name enable_roce value {mode} cmode driverinit"
             for mode in ("true", "false")]
    rules.append(f"/usr/sbin/devlink dev reload {pci} action driver_reinit")
    (peer / "ocs-roce-cm-peer.sudoers").write_text(f"rkp64 ALL=(root) NOPASSWD: {', '.join(rules)}\n")
    (peer / "install.sh").write_text(PEER_INSTALLER)
    (peer / "install.sh").chmod(0o700)
    write_manifest(peer, ("ocs-roce-cm-peer.sudoers",))


def stage(revision):
    build = Path(__file__).resolve().parents[1]
    binaries = ("roce_write", "roce_app_write", "roce_verbs_write")
    for binary in binaries:
        if not (build / binary).is_file() or not os.access(build / binary, os.X_OK):
            raise ValueError(f"Build {build / binary} before staging the bundle")

    destination = INSTALL_ROOT / revision
    bundle = Path(tempfile.mkdtemp(prefix="ocs-roce-cm-"))
    for binary in binaries:
        shutil.copy2(build / binary, bundle / binary)
    (bundle / "run_app").write_text(APP_LAUNCHER)
    (bundle / "run_app").chmod(0o700)
    units = []
    rules = []
    for role, mtu, pmtu, port in PROFILES:
        unit = f"ocs-roce-cm-{role}-{mtu}.service"
        (bundle / unit).write_text(render_unit(destination, role, mtu, pmtu, port))
        units.append(unit)
        rules.append(f"rkp64 ALL=(root) NOPASSWD: /usr/bin/systemctl start {unit}, /usr/bin/systemctl stop {unit}")
        rules.append(r"rkp64 ALL=(root) NOPASSWD: /usr/bin/journalctl --quiet --no-pager --output\=json "
                     r"--output-fields\=MESSAGE\,_SYSTEMD_INVOCATION_ID\,__REALTIME_TIMESTAMP "
                     rf"--unit {unit} --lines\=200")
    (bundle / "ocs-roce-cm.sudoers").write_text("\n".join(rules) + "\n")
    (bundle / "install.sh").write_text(render_install(destination, units))
    (bundle / "install.sh").chmod(0o700)
    files = (*binaries, "run_app", *units, "ocs-roce-cm.sudoers")
    write_manifest(bundle, files)
    stage_peer_access(bundle)
    return bundle


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("revision", help="revision labeling the three built executables")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{7,40}", args.revision):
        parser.error("revision must be a 7- to 40-character Git commit ID")
    print(stage(args.revision))


if __name__ == "__main__":
    main()
