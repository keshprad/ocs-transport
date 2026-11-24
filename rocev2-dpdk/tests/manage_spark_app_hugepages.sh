#!/bin/bash
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
umask 077
[[ $# == 1 && $EUID == 0 && ${INVOCATION_ID:-} =~ ^[a-f0-9]{32}$ ]]
case "$(hostname -s)" in
    spark-7e64|spark-4ed9) ;;
    *) exit 1 ;;
esac

pool=/sys/kernel/mm/hugepages/hugepages-2048kB
pages=64
runtime=/run/ocs-roce-app-bw
state=/run/ocs-roce-app-bw-hugepages
backing=/dev/hugepages/ocs-roce-app-bw

case "$1" in
    prepare)
        [[ $(findmnt --noheadings --target /dev/hugepages --output FSTYPE) == hugetlbfs &&
           $(stat -f -c %S -- /dev/hugepages) == 2097152 ]]
        if [[ $(<"$pool/nr_hugepages") != 0 || $(<"$pool/free_hugepages") != 0 ||
              $(<"$pool/resv_hugepages") != 0 || $(<"$pool/surplus_hugepages") != 0 ]]; then
            printf 'The temporary DPDK reservation requires an empty 2 MiB page pool.\n' >&2
            exit 1
        fi
        [[ ! -e $backing && ! -L $backing && -d $runtime && ! -L $runtime ]]
        uid=$(stat -c %u -- "$runtime")
        gid=$(stat -c %g -- "$runtime")
        [[ $uid == "$(id -u ocs-roce-native)" && $gid == "$(id -g ocs-roce-native)" && $uid != 0 && $gid != 0 ]]
        mkdir -m 0700 -- "$state"
        if ! printf '%s\n' "$INVOCATION_ID" >"$state/owner"; then
            rm -f -- "$state/owner"
            rmdir -- "$state"
            exit 1
        fi
        printf '%s\n' "$pages" >"$pool/nr_hugepages"
        [[ $(<"$pool/nr_hugepages") == "$pages" && $(<"$pool/free_hugepages") == "$pages" ]]
        install -d -o "$uid" -g "$gid" -m 0700 -- "$backing"
        printf 'DPDK_HUGEPAGES reserved=%s page_kib=2048\n' "$pages"
        ;;
    cleanup)
        if [[ ! -f $state/owner || $(<"$state/owner") != "$INVOCATION_ID" ]]; then
            exit 0
        fi
        current=$(<"$pool/nr_hugepages")
        if [[ $current -gt $pages || $(<"$pool/free_hugepages") != "$current" ||
              $(<"$pool/resv_hugepages") != 0 || $(<"$pool/surplus_hugepages") != 0 ]]; then
            printf 'The 2 MiB page pool changed or is still in use.\n' >&2
            exit 1
        fi
        printf '0\n' >"$pool/nr_hugepages"
        [[ $(<"$pool/nr_hugepages") == 0 ]]
        if [[ -d $backing ]]; then rmdir -- "$backing"; fi
        rm -- "$state/owner"
        rmdir -- "$state"
        printf 'DPDK_HUGEPAGES restored=0 page_kib=2048\n'
        ;;
    *) exit 2 ;;
esac
