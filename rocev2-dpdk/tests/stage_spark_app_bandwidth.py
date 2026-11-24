#!/usr/bin/env python3
"""Stage paired Spark application-bandwidth services from pinned userspace inputs."""

import argparse
import hashlib
from pathlib import Path
import re
import shlex
import shutil
import tempfile

from check_spark_bandwidth import (
    APP_DESTINATION_ROOT, APP_FILES, APP_HUGEPAGE_HELPER, APP_PROFILES, HUGEPAGE_PROFILES,
    PREFAULT_PROFILES, Endpoint, unit_argv, unit_environment,
)
from stage_spark_cm import APP_LAUNCHER


HOSTS = {"spark-04": "spark-7e64", "spark-02": "spark-4ed9"}
ACCOUNT = "ocs-roce-native"
RULES = "99-ocs-roce-native.rules"
SUDOERS = "ocs-roce-app-bw.sudoers"
INPUT_APPS = {
    "roce_write": "bin/roce_write",
    "roce_app_write": "bin/roce_app_write",
    "provider/libroce_dpdk-rdmav34.so": "provider/libroce_dpdk-rdmav34.so",
}
REQUIRED_LIBS = {f"lib/librte_{name}.so.24" for name in ("eal", "mempool_ring", "common_mlx5", "net_mlx5")}


def digest(path):
    with path.open("rb") as data:
        return hashlib.file_digest(data, "sha256").hexdigest()


def manifest_text(directory, files):
    return "".join(f"{digest(directory / file)}  {file}\n" for file in files)


def read_manifest(directory):
    path = directory / "SHA256SUMS"
    hashes = {}
    for line in path.read_text(encoding="ascii").splitlines():
        entry = re.fullmatch(r"([0-9a-f]{64}) [ *](?:\./)?([A-Za-z0-9_./-]+)", line)
        if not entry:
            raise ValueError(f"Invalid checksum entry in {path}")
        checksum, name = entry.groups()
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts or str(relative) != name or name in hashes:
            raise ValueError(f"Unsafe or duplicate input path in {path}: {name}")
        hashes[name] = checksum
    return hashes


def stage_inputs(source, target, revision):
    source = source.resolve()
    hashes = read_manifest(source)
    if "REVISION" in hashes:
        marker = source / "REVISION"
        recorded = marker.read_text(encoding="ascii").strip()
        if (digest(marker) != hashes["REVISION"] or not re.fullmatch(r"[0-9a-f]{7,40}", recorded) or
                not recorded.startswith(revision)):
            raise ValueError(f"{source}: source revision does not match {revision}")
    libraries = sorted(name for name in hashes if re.fullmatch(r"lib/[A-Za-z0-9_]+\.so(?:\.\d+)*", name))
    if not REQUIRED_LIBS.issubset(libraries):
        raise ValueError(f"{source}: pinned input lacks required DPDK libraries")
    assets = INPUT_APPS | {name: name for name in libraries}
    for target_name, input_name in assets.items():
        original = (source / input_name).resolve(strict=True)
        if not original.is_relative_to(source) or not original.is_file() or input_name not in hashes:
            raise ValueError(f"{source}: expected a pinned file at {input_name}")
        copied = target / target_name
        copied.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        shutil.copyfile(original, copied)
        if digest(copied) != hashes[input_name]:
            raise ValueError(f"{source}: checksum mismatch for {input_name}")
    return libraries


def render_launcher():
    launch = "        /usr/bin/setpriv --ambient-caps=-all ./roce_app_write \"$socket\" &\n        application=$!"
    worker = './roce_write "$@" --app-socket "$socket" > "$stdout" &'
    cleanup = '    if [[ -n $worker ]]; then kill "$worker" 2>/dev/null || :; fi'
    if any(APP_LAUNCHER.count(block) != 1 for block in (launch, worker, cleanup)):
        raise ValueError("The shared application launcher no longer has its expected process blocks")
    application = '''        ROCE_DPDK_APP_BW=1 /usr/bin/setpriv --ambient-caps=-all --inh-caps=-all \\
            /usr/bin/taskset --cpu-list 10-13 ./roce_app_write "$socket" &
        application=$!
        printf 'APP_PROCESSES worker_pid=%s app_pid=%s\\n' "$worker" "$application"'''
    # mlx5 falls back to the current directory if /var/log is read-only.
    run_worker = '''release=$PWD
( cd -- "$XDG_RUNTIME_DIR"; exec "$release/roce_write" "$@" --app-socket "$socket" ) > "$stdout" &'''
    report = r'''
    for dump in "$XDG_RUNTIME_DIR"/dpdk_mlx5_port_*_rxq_*; do
        [[ -f $dump ]] || continue
        IFS= read -r syndrome < "$dump" || continue
        if [[ $syndrome == 'Unexpected CQE error syndrome '* ]]; then
            printf 'MLX5_RX_ERROR %s %s\n' "$dump" "$syndrome"
        fi
    done'''
    return APP_LAUNCHER.replace(launch, application).replace(worker, run_worker).replace(cleanup, cleanup + report)


def render_unit(endpoint, destination):
    runtime = destination / "lib"
    environment = "\n".join(f"Environment={shlex.quote(f'{key}={value}')}"
                            for key, value in unit_environment(endpoint, destination, runtime).items())
    hugepage_hooks = ""
    if endpoint.settings.hugepages:
        helper = destination / APP_HUGEPAGE_HELPER
        hugepage_hooks = f"ExecStartPre=+/bin/bash {helper} prepare\nExecStopPost=+/bin/bash {helper} cleanup\n"
    return f"""[Unit]
Description=RoCEv2 Spark application bandwidth ({endpoint.profile}, {endpoint.role})
ConditionHost={HOSTS[endpoint.host]}

[Service]
Type=exec
User={ACCOUNT}
Group={ACCOUNT}
WorkingDirectory={destination}
{environment}
UnsetEnvironment=RUNTIME_DIRECTORY
RuntimeDirectory={endpoint.settings.runtime_dir.name}
RuntimeDirectoryMode=0750
NoNewPrivileges=yes
CapabilityBoundingSet=CAP_NET_RAW CAP_NET_ADMIN
AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN
LimitMEMLOCK=20G
ProtectHome=yes
ProtectSystem=strict
PrivateTmp=yes
RuntimeMaxSec=120
{hugepage_hooks}ExecStart={shlex.join(unit_argv(endpoint, destination, runtime))}
"""


def render_permissions(units):
    rules = []
    for unit in units:
        rules.append(f"rkp64 ALL=(root) NOPASSWD: /usr/bin/systemctl start {unit}, /usr/bin/systemctl stop {unit}")
        rules.append(r"rkp64 ALL=(root) NOPASSWD: /usr/bin/journalctl --quiet --no-pager --output\=json "
                     r"--output-fields\=MESSAGE\,_SYSTEMD_INVOCATION_ID\,__REALTIME_TIMESTAMP "
                     rf"--unit {unit} --lines\=200")
    return "\n".join(rules) + "\n"


def render_install(host, destination, units, payload, manifest_hash):
    managed = (*units, RULES, SUDOERS)
    return f"""#!/bin/bash
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
[[ $EUID == 0 && $(hostname -s) == {HOSTS[host]} ]] || {{
    echo 'Run as root on {host} ({HOSTS[host]}).' >&2
    exit 1
}}
cd -- "$(dirname -- "${{BASH_SOURCE[0]}}")"
fail() {{ echo "$*" >&2; exit 1; }}
safe_directory() {{
    local uid mode
    [[ -d $1 && ! -L $1 ]] || fail "Unsafe installation directory $1"
    read -r uid mode < <(stat -c '%u %a' -- "$1")
    [[ $uid == 0 ]] && (( (8#$mode & 8#022) == 0 )) || fail "Unsafe installation directory $1"
}}
active=$(systemctl list-units --state=running,activating,deactivating --no-legend --plain --no-pager 'ocs-roce-bw-app-*.service')
[[ -z $active ]] || fail 'A RoCEv2 application bandwidth service is running.'
safe_directory {APP_DESTINATION_ROOT.parent}
if [[ ! -e {APP_DESTINATION_ROOT} && ! -L {APP_DESTINATION_ROOT} ]]; then
    install -d -o root -g root -m 0755 {APP_DESTINATION_ROOT}
fi
safe_directory {APP_DESTINATION_ROOT}

stage=$(mktemp -d -- {APP_DESTINATION_ROOT}/.install.XXXXXX)
trap 'rm -rf -- "$stage"' EXIT
files=({shlex.join((*payload, *managed))})
for file in "${{files[@]}}" SHA256SUMS; do
    install -D -o root -g root -m 0644 -- "$file" "$stage/$file"
done
printf '%s  %s\\n' '{manifest_hash}' "$stage/SHA256SUMS" | sha256sum --check --strict --status
(cd -- "$stage" && sha256sum --check --strict SHA256SUMS)
visudo -cf "$stage/{SUDOERS}"
udevadm verify --resolve-names=never "$stage/{RULES}"
units=({shlex.join(units)})
unit_paths=("${{units[@]/#/$stage/}}")
systemd-analyze verify --man=no --generators=no --recursive-errors=no "${{unit_paths[@]}}"

if ! getent group {ACCOUNT} >/dev/null; then groupadd --system {ACCOUNT}; fi
group_entry=$(getent -s files group {ACCOUNT})
IFS=: read -r _ _ gid members <<< "$group_entry"
[[ $gid != 0 && -n $gid && -z $members ]] || fail 'Unsafe service group {ACCOUNT}.'
if ! getent passwd {ACCOUNT} >/dev/null; then
    useradd --system --gid {ACCOUNT} --home-dir /nonexistent --no-create-home --shell /usr/sbin/nologin {ACCOUNT}
fi
account_entry=$(getent -s files passwd {ACCOUNT})
IFS=: read -r _ _ uid primary_gid _ home shell <<< "$account_entry"
read -r _ password_state _ <<< "$(passwd -S {ACCOUNT})"
[[ $uid != 0 && -n $uid && $gid == "$primary_gid" && $(id -G {ACCOUNT}) == "$gid" &&
   $home == /nonexistent && $shell == /usr/sbin/nologin && $password_state == L ]] ||
    fail 'Unsafe service account {ACCOUNT}.'

release="$stage/release"
install -d -o root -g root -m 0755 "$release"
runtime=({shlex.join(payload)})
for file in "${{runtime[@]}}"; do
    install -D -o root -g root -m 0755 -- "$stage/$file" "$release/$file"
done
if [[ -e {destination} || -L {destination} ]]; then
    safe_directory {destination}
    unsafe=$(find {destination} \\( -type l -o ! -user root -o -perm /022 \\) -print -quit)
    [[ -z $unsafe ]] || fail "Unsafe release path $unsafe"
    diff -qr --no-dereference -- "$release" {destination} || fail 'The installed revision differs from the bundle.'
else
    mv -T -- "$release" {destination}
fi

for unit in "${{units[@]}}"; do
    install -o root -g root -m 0644 -- "$stage/$unit" "/etc/systemd/system/$unit"
done
install -o root -g root -m 0644 -- "$stage/{RULES}" /etc/udev/rules.d/{RULES}
udevadm control --reload-rules
udevadm trigger --action=change --settle --subsystem-match=misc --sysname-match=roce_mr
udevadm trigger --action=change --settle --subsystem-match=infiniband_verbs --attr-match=ibdev=roce_dpdk0
systemctl daemon-reload
install -o root -g root -m 0440 -- "$stage/{SUDOERS}" /etc/sudoers.d/ocs-roce-app-bw
echo '{host} RoCEv2 application bandwidth services installed.'
"""


def stage_host(bundle, inputs, destination, endpoints):
    host = endpoints[0].host
    directory = bundle / host
    directory.mkdir(mode=0o700)
    libraries = stage_inputs(inputs / host, directory, destination.name)
    launcher = directory / "run_app"
    launcher.write_text(render_launcher())
    launcher.chmod(0o700)
    units = [endpoint.unit for endpoint in endpoints]
    for endpoint in endpoints:
        (directory / endpoint.unit).write_text(render_unit(endpoint, destination))
    (directory / SUDOERS).write_text(render_permissions(units))
    (directory / RULES).write_text(
        f'SUBSYSTEM=="misc", KERNEL=="roce_mr", OWNER:="root", GROUP:="{ACCOUNT}", MODE:="0660"\n'
        f'SUBSYSTEM=="infiniband_verbs", KERNEL=="uverbs[0-9]*", ATTR{{ibdev}}=="roce_dpdk0", '
        f'OWNER:="root", GROUP:="{ACCOUNT}", MODE:="0660"\n'
    )
    payload = (*APP_FILES, *libraries)
    if any(endpoint.settings.hugepages for endpoint in endpoints):
        source = Path(__file__).with_name("manage_spark_app_hugepages.sh")
        shutil.copyfile(source, directory / APP_HUGEPAGE_HELPER)
        payload += (APP_HUGEPAGE_HELPER,)
    manifest = directory / "SHA256SUMS"
    manifest.write_text(manifest_text(directory, (*payload, *units, RULES, SUDOERS)))
    installer = directory / "install.sh"
    installer.write_text(render_install(host, destination, units, payload, digest(manifest)))
    installer.chmod(0o700)
    return manifest_text(directory, payload)


def stage(revision, inputs, profiles):
    destination = APP_DESTINATION_ROOT / revision
    bundle = Path(tempfile.mkdtemp(prefix="ocs-roce-app-bw-"))
    try:
        tx, rx = (stage_host(bundle, inputs, destination, [Endpoint(role, profile) for profile in profiles])
                  for role in ("tx", "rx"))
        if tx != rx:
            raise ValueError("The pinned application artifacts differ between Spark hosts")
        app_files = APP_FILES
        if any(name in HUGEPAGE_PROFILES for name in profiles):
            app_files += (APP_HUGEPAGE_HELPER,)
        (bundle / "APP_SHA256SUMS").write_text(manifest_text(bundle / "spark-04", app_files))
    except Exception:
        shutil.rmtree(bundle)
        raise
    return bundle


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("revision", help="revision of the pinned RoCEv2 application and worker")
    parser.add_argument("--inputs", type=Path, required=True, help="directory with spark-02 and spark-04 inputs")
    parser.add_argument("--profile", action="append", choices=APP_PROFILES | PREFAULT_PROFILES,
                        help="profile to stage (default: all profiles without prefaulting)")
    parser.add_argument("--include-prefault", action="store_true",
                        help="also stage prefault variants of the selected profiles")
    parser.add_argument("--include-hugepages", action="store_true",
                        help="also stage the 2 MiB packet-pool variants of selected MTU 4200 profiles")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{7,40}", args.revision):
        parser.error("revision must be a 7- to 40-character Git commit ID")
    profiles = tuple(args.profile or APP_PROFILES)
    if args.include_prefault:
        profiles += tuple(f"{name}-prefault" for name in profiles if name in APP_PROFILES)
    if args.include_hugepages:
        profiles += tuple(f"{name}-hugepages" for name in profiles if f"{name}-hugepages" in HUGEPAGE_PROFILES)
    try:
        print(stage(args.revision, args.inputs, tuple(dict.fromkeys(profiles))))
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
