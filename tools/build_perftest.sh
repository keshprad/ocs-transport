#!/usr/bin/env bash
# Build the pinned latest stable perftest release privately, without sudo.
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
prefix="$repo_dir/.perftest"
release=26.07.8
asset=perftest-26.07.8-0.157.ga31f9ea.tar.gz
archive_sha=36cc9b0235612017d2baa5f74ed114c963b602ba8c4424d79f98ffc18948b222
version=6.30

for tool in curl sha256sum tar make cc install; do
    if ! command -v "$tool" >/dev/null; then
        echo "Missing $tool. See README.md: Build current perftest." >&2
        exit 1
    fi
done

mkdir -p "$prefix"
archive="$prefix/$asset"
if [[ ! -f "$archive" ]]; then
    echo "Downloading perftest $release source..."
    curl -fsSL --retry 2 --connect-timeout 15 --max-time 120 \
        "https://github.com/linux-rdma/perftest/releases/download/$release/$asset" -o "$archive.tmp"
    mv -- "$archive.tmp" "$archive"
fi
if ! printf '%s  %s\n' "$archive_sha" "$archive" | sha256sum --check --status; then
    echo "Source checksum mismatch: $archive" >&2
    exit 1
fi

build_dir=$(mktemp -d "$prefix/build.XXXXXX")
build_log="$prefix/build.log"
cleanup() {
    if [[ $? == 0 ]]; then
        rm -rf -- "$build_dir"
    else
        echo "Build failed; source retained in $build_dir. Log: $build_log" >&2
    fi
}
trap cleanup EXIT

tar -xzf "$archive" -C "$build_dir" --strip-components=1
cd -- "$build_dir"

run_logged() {
    echo "Running: $* (log: $build_log)"
    if ! "$@" >> "$build_log" 2>&1; then
        tail -n 40 "$build_log" >&2
        return 1
    fi
}
: > "$build_log"
# This baseline uses host memory; retain the default ibv_wr_* posting API.
run_logged env CUDA_H_PATH= MUSA_H_PATH= ./configure --prefix="$prefix" --disable-cudart
run_logged make -j "${PERFTEST_BUILD_JOBS:-2}" ib_write_bw ib_write_lat

# perftest returns 10 for --version, so verify its output rather than its status.
for tool in ib_write_bw ib_write_lat; do
    actual=$("./$tool" --version 2>&1 || true)
    if [[ "$actual" != "Version: $version" ]]; then
        echo "Unexpected $tool version: $actual" >&2
        exit 1
    fi
done
install -d "$prefix/bin"
install -m 755 ib_write_bw ib_write_lat "$prefix/bin/"
install -m 644 COPYING "$prefix/COPYING"
printf 'release=%s\narchive_sha256=%s\nversion=%s\n' "$release" \
    "$archive_sha" "$version" > "$prefix/build-info.txt"
echo "Built perftest $release (tool version $version) in $prefix/bin."
echo "rdma_bench.py selects it automatically. Build the same release on the peer."
