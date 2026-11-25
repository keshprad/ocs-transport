#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf '%s\n' "$*" >&2
    exit 1
}

check_host() {
    local machine
    machine=$(hostname -s) || fail "$label: could not identify the host"
    case "$label:$machine" in
        spark-02:spark-02|spark-02:spark-4ed9|spark-03:spark-03|spark-03:spark-016e|spark-04:spark-04|spark-04:spark-7e64) ;;
        *) fail "$label: unexpected hostname $machine" ;;
    esac
}

git_read() {
    git --no-optional-locks -C "$repo" "$@"
}

check_checkout() {
    local root origin changes
    root=$(git_read rev-parse --show-toplevel) || fail "$label: no Git worktree at $repo"
    [[ "$root" == "$(cd -- "$repo" && pwd -P)" ]] || fail "$label: unexpected Git worktree $root"

    origin=$(git_read config --get remote.origin.url) || fail "$label: missing origin"
    case "$origin" in
        git@github.com:keshprad/ocs-transport.git|https://github.com/keshprad/ocs-transport.git|ssh://git@github.com/keshprad/ocs-transport.git) ;;
        *) fail "$label: origin is not the expected GitHub repository" ;;
    esac

    changes=$(git_read status --porcelain=v1 --untracked-files=all --ignore-submodules=none) ||
        fail "$label: could not check worktree status"
    [[ -z "$changes" ]] || fail "$label: worktree is dirty at $repo"$'\n'"$changes"
}

verify_revision() {
    local actual
    actual=$(git_read rev-parse --verify 'HEAD^{commit}') || fail "$label: could not read HEAD"
    [[ "$actual" == "$expected" ]] || fail "$label: expected $expected, found $actual"
    printf '%s %s\n' "$label" "$actual"
}

mode=verify
if [[ "${1:-}" == --sync ]]; then
    mode=sync
    shift
fi
if [[ $# != 1 || ! "$1" =~ ^[0-9a-f]{40}$ ]]; then
    printf 'Usage: %s [--sync] FULL_COMMIT_HASH\n' "${0##*/}" >&2
    exit 2
fi
expected=$1
branch_ref=refs/heads/keshprad/codex/dpdk-mrc
repo=$(git -C "$(dirname -- "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
label=spark-04
check_host
check_checkout
verify_revision

check_peer() {
    local host=$1 peer_mode=$2
    {
        declare -f fail check_host git_read check_checkout verify_revision
        cat <<'REMOTE'
set -euo pipefail
expected=$1
branch_ref=$2
label=$3
mode=$4
primary="$HOME/Documents/Workspace/ocs-transport"
repo="$HOME/Documents/Workspace/ocs-transport-mrc-dpdk"

check_host
check_checkout
[[ -f "$repo/.git" ]] || fail "$label: expected a linked worktree at $repo"
primary_common=$(git -C "$primary" rev-parse --path-format=absolute --git-common-dir) ||
    fail "$label: could not identify the primary repository"
peer_common=$(git_read rev-parse --path-format=absolute --git-common-dir) ||
    fail "$label: could not identify the peer repository"
[[ "$primary_common" == "$peer_common" ]] || fail "$label: peer worktree belongs to another repository"

if git_read symbolic-ref -q HEAD >/dev/null; then
    fail "$label: expected a detached HEAD in $repo"
else
    [[ $? == 1 ]] || fail "$label: could not check detached HEAD"
fi
if [[ "$mode" == verify ]]; then
    verify_revision
    exit
fi

git -C "$repo" fetch --quiet --no-tags --no-recurse-submodules --refmap= origin "$branch_ref" ||
    fail "$label: could not fetch $branch_ref from origin"
published=$(git_read rev-parse --verify 'FETCH_HEAD^{commit}') ||
    fail "$label: could not resolve the published branch tip"
[[ "$published" == "$expected" ]] ||
    fail "$label: published branch tip is $published, expected $expected"
git_read merge-base --is-ancestor HEAD "$expected" ||
    fail "$label: refusing to move the peer worktree backward or across histories"

if [[ "$mode" == sync ]]; then
    check_checkout
    git -C "$repo" switch --quiet --detach "$expected" ||
        fail "$label: could not switch to $expected"
    check_checkout
    verify_revision
fi
REMOTE
    } | ssh -o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=5 -o ServerAliveCountMax=2 \
        "$host" bash -s -- "$expected" "$branch_ref" "$host" "$peer_mode" ||
        fail "$host: revision gate failed"
}

peers=(spark-02 spark-03)
if [[ "$mode" == sync ]]; then
    for host in "${peers[@]}"; do check_peer "$host" prepare; done
    for host in "${peers[@]}"; do check_peer "$host" sync; done
fi
for host in "${peers[@]}"; do check_peer "$host" verify; done

check_checkout
verify_revision >/dev/null
