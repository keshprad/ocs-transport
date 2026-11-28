#!/bin/bash
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
umask 077
[[ $# == 1 && $EUID == 0 && ${INVOCATION_ID:-} =~ ^[a-f0-9]{32}$ ]]
case "$(hostname -s)" in
    spark-7e64)
        interfaces=(enp1s0f0np0 enP2p1s0f1np1)
        devices=(0000:01:00.0 0002:01:00.1)
        rails=(1 2)
        ;;
    spark-4ed9)
        interfaces=(enp1s0f1np1)
        devices=(0000:01:00.1)
        rails=(1)
        ;;
    spark-28a3)
        interfaces=(enP2p1s0f0np0)
        devices=(0002:01:00.0)
        rails=(2)
        ;;
    *) exit 1 ;;
esac
pages=$((64 * ${#rails[@]}))
pool=/sys/kernel/mm/hugepages/hugepages-2048kB
state=/run/ocs-roce-app-bw-hugepages
backing=/dev/hugepages/ocs-roce-two-qsfp

check_interfaces() {
    local index device param
    for index in "${!interfaces[@]}"; do
        device=${devices[index]}
        [[ $(basename "$(readlink -f "/sys/class/net/${interfaces[index]}/device")") == "$device" ]]
        param=$(/usr/sbin/devlink dev param show "pci/$device" name enable_roce)
        [[ $param == *'cmode driverinit value false'* ]]
    done
}

case "$1" in
    prepare)
        check_interfaces
        [[ $(findmnt -n --target /dev/hugepages -o FSTYPE) == hugetlbfs ]]
        [[ $(stat -f -c %S /dev/hugepages) == 2097152 ]]
        [[ ! -e $backing && ! -L $backing ]]
        for interface in "${interfaces[@]}"; do
            [[ $(<"/sys/class/net/$interface/mtu") == 1500 ]]
        done
        [[ $(<"$pool/nr_hugepages") == 0 && $(<"$pool/free_hugepages") == 0 &&
           $(<"$pool/resv_hugepages") == 0 && $(<"$pool/surplus_hugepages") == 0 ]]
        mkdir -m 0700 -- "$state"
        if ! printf '%s\n' "$INVOCATION_ID" >"$state/owner"; then
            rm -f -- "$state/owner"
            rmdir -- "$state"
            exit 1
        fi
        printf '%s\n' "$pages" >"$pool/nr_hugepages"
        [[ $(<"$pool/nr_hugepages") == "$pages" && $(<"$pool/free_hugepages") == "$pages" ]]
        mkdir -m 0711 -- "$backing"
        for rail in "${rails[@]}"; do
            install -d -o ocs-roce-native -g ocs-roce-native -m 0700 -- "$backing/$rail"
        done
        for interface in "${interfaces[@]}"; do
            /usr/sbin/ip link set dev "$interface" mtu 4200
            [[ $(<"/sys/class/net/$interface/mtu") == 4200 ]]
        done
        printf 'DPDK_HUGEPAGES reserved=%s page_kib=2048\n' "$pages"
        ;;
    cleanup)
        [[ -f $state/owner && $(<"$state/owner") == "$INVOCATION_ID" ]] || exit 0
        status=0
        for index in "${!interfaces[@]}"; do
            interface=${interfaces[index]}
            sysfs="/sys/class/net/$interface"
            if [[ $(basename "$(readlink -f "$sysfs/device")") != "${devices[index]}" ]]; then
                status=1
                continue
            fi
            if [[ $(<"$sysfs/mtu") == 4200 ]]; then
                /usr/sbin/ip link set dev "$interface" mtu 1500 || status=1
                [[ $(<"$sysfs/mtu") == 1500 ]] || status=1
            fi
        done
        current=$(<"$pool/nr_hugepages")
        if (( current > pages )) || [[ $(<"$pool/free_hugepages") != "$current" ||
              $(<"$pool/resv_hugepages") != 0 || $(<"$pool/surplus_hugepages") != 0 ]]; then
            printf 'The 2 MiB hugepage pool changed or is still in use.\n' >&2
            exit 1
        fi
        printf '0\n' >"$pool/nr_hugepages"
        [[ $(<"$pool/nr_hugepages") == 0 ]]
        for rail in "${rails[@]}"; do
            if [[ -d $backing/$rail ]]; then rmdir -- "$backing/$rail" || status=1; fi
        done
        if [[ -d $backing ]]; then rmdir -- "$backing" || status=1; fi
        rm -- "$state/owner"
        rmdir -- "$state"
        (( status == 0 ))
        printf 'DPDK_HUGEPAGES restored=0 page_kib=2048\n'
        ;;
    *) exit 2 ;;
esac
