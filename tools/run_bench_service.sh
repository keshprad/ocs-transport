#!/usr/bin/env bash
# Run one benchmark controller/server independently of its terminal.
set -euo pipefail

if [[ $# == 0 || ( "$1" != server && "$1" != client ) ]]; then
    echo "Usage: bash tools/run_bench_service.sh server [OPTIONS...]" >&2
    echo "       bash tools/run_bench_service.sh client SERVER_IP [OPTIONS...]" >&2
    exit 2
fi
role=$1
shift
if [[ "$role" == client && ( $# == 0 || "$1" == -* ) ]]; then
    echo "Client requires SERVER_IP before its options" >&2
    exit 2
fi

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
python_bin=$(command -v python3)
unit="ocs-rdma-$role"
log_file="$repo_dir/results/service-$role.log"
mkdir -p "$repo_dir/results"

systemd-run --user --unit="$unit" --collect --service-type=exec \
    --working-directory="$repo_dir" \
    --property="StandardOutput=append:$log_file" \
    --property="StandardError=append:$log_file" \
    "$python_bin" "$repo_dir/rdma_bench.py" "--$role" "$@"
printf 'Log: %s\nStop: systemctl --user stop %s\n' "$log_file" "$unit"
