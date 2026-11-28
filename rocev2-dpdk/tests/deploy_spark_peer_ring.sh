#!/bin/bash
# Install or remove the per-peer receiver overrides on the three ring hosts.
set -euo pipefail
action=${1:-install}
[[ $# -le 1 && ( $action == install || $action == uninstall || $action == check ) ]] || {
    echo "Usage: $0 [check|install|uninstall]" >&2
    exit 2
}
here=$(cd -- "$(dirname -- "$0")" && pwd -P)
hosts=(spark-04 spark-02 spark-01)
declare -A copies=()
cleanup() {
    for host in "${!copies[@]}"; do
        ssh -T -o BatchMode=yes "$host" "rm -f -- '${copies[$host]}'" || true
    done
}
trap cleanup EXIT
for host in "${hosts[@]}"; do
    path=$(ssh -T -o BatchMode=yes "$host" 'mktemp /tmp/ocs-peer-vf.XXXXXXXXXX')
    [[ $path =~ ^/tmp/ocs-peer-vf\.[[:alnum:]]{10}$ ]]
    copies[$host]=$path
    scp -q -o BatchMode=yes "$here/peer_vf.py" "$host:$path"
    ssh -T -o BatchMode=yes "$host" "/usr/bin/python3 -I '$path' check"
done
if [[ $action != check ]]; then
    for host in "${hosts[@]}"; do
        echo "$host: $action per-peer DPDK profile"
        if [[ -t 0 ]]; then
            ssh -t -o BatchMode=yes "$host" "sudo /usr/bin/python3 -I '${copies[$host]}' $action"
        else
            ssh -T -o BatchMode=yes "$host" "sudo -n /usr/bin/python3 -I '${copies[$host]}' $action"
        fi
    done
fi
