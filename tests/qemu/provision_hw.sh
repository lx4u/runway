#!/bin/sh
# tests/qemu/provision_hw.sh — facts about the machine and its disks, for the benchmark report.
# SPDX-License-Identifier: Apache-2.0
# Source it: hw_report BOOT_DISK TARGET_DISK... prints key=value lines in sections on standard output.
# It only reads /proc and /sys; a missing value is "-".

hw_val() {
    [ -r "$1" ] || { echo "-"; return; }
    v=$(tr -d '\n' <"$1" | sed 's/^ *//; s/ *$//')
    echo "${v:--}"
}

# The first file with this name in the sysfs directory of the disk or in one of its parents.
hw_up() {
    d=$(readlink -f "/sys/block/${1##*/}/device" 2>/dev/null) || { echo "-"; return; }
    while [ -n "$d" ] && [ "$d" != / ]; do
        if [ -r "$d/$2" ]; then hw_val "$d/$2"; return; fi
        d=${d%/*}
    done
    echo "-"
}

hw_system() {
    echo "[system]"
    echo "cpu_model=$(sed -n 's/^model name[[:space:]]*: *//p; s/^Model[[:space:]]*: *//p; s/^Hardware[[:space:]]*: *//p' /proc/cpuinfo | head -n 1)"
    echo "cpus=$(grep -c '^processor' /proc/cpuinfo)"
    if grep -E '^(flags|Features)' /proc/cpuinfo | grep -qw aes; then echo "cpu_aes=yes"; else echo "cpu_aes=no"; fi
    khz=$(hw_val /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq)
    case "$khz" in
    ''|-) echo "cpu_max_mhz=-" ;;
    *) echo "cpu_max_mhz=$((khz / 1000))" ;;
    esac
    cf=/sys/devices/system/cpu/cpu0/cpufreq
    echo "cpufreq_driver=$(hw_val $cf/scaling_driver)"
    echo "cpufreq_governor=$(hw_val $cf/scaling_governor)"
    echo "cpufreq_governors=$(hw_val $cf/scaling_available_governors)"
    for k in min max cur; do
        khz=$(hw_val $cf/scaling_${k}_freq)
        case "$khz" in
        ''|-) echo "cpufreq_${k}_mhz=-" ;;
        *) echo "cpufreq_${k}_mhz=$((khz / 1000))" ;;
        esac
    done
    echo "cpufreq_boost=$(hw_val /sys/devices/system/cpu/cpufreq/boost)"
    echo "intel_pstate_status=$(hw_val /sys/devices/system/cpu/intel_pstate/status)"
    echo "intel_pstate_no_turbo=$(hw_val /sys/devices/system/cpu/intel_pstate/no_turbo)"
    echo "mem_total_kib=$(sed -n 's/^MemTotal: *\([0-9]*\) kB/\1/p' /proc/meminfo)"
    for k in sys_vendor product_name bios_vendor bios_version bios_date; do
        echo "dmi_$k=$(hw_val /sys/class/dmi/id/$k)"
    done
    echo "arch=$(uname -m)"
    echo "kernel=$(uname -r)"
}

hw_disk() {
    n=${1##*/}
    s=/sys/block/$n
    echo "[disk $1 $2]"
    echo "model=$(hw_val "$s/device/model")"
    fw=$(hw_val "$s/device/firmware_rev")
    [ "$fw" != - ] || fw=$(hw_val "$s/device/rev")
    echo "firmware=$fw"
    echo "serial=$(hw_val "$s/device/serial")"
    echo "size_bytes=$(( $(cat "$s/size" 2>/dev/null || echo 0) * 512 ))"
    echo "logical_block_size=$(hw_val "$s/queue/logical_block_size")"
    echo "physical_block_size=$(hw_val "$s/queue/physical_block_size")"
    echo "rotational=$(hw_val "$s/queue/rotational")"
    echo "write_cache=$(hw_val "$s/queue/write_cache")"
    echo "removable=$(hw_val "$s/removable")"
    path=$(readlink -f "$s/device" 2>/dev/null || true)
    case "$path" in
    *nvme*) bus=nvme ;;
    '') if [ -d "$s/md" ]; then bus=md; else bus=-; fi ;;
    */usb*) bus=usb ;;
    */ata*) bus=sata ;;
    *virtio*) bus=virtio ;;
    *) bus=- ;;
    esac
    echo "bus=$bus"
    echo "link_speed=$(hw_up "$1" current_link_speed)"
    echo "link_width=$(hw_up "$1" current_link_width)"
    echo "link_max_speed=$(hw_up "$1" max_link_speed)"
    echo "link_max_width=$(hw_up "$1" max_link_width)"
    echo "usb_speed_mbps=$(hw_up "$1" speed)"
}

hw_report() {
    boot=$1
    shift
    hw_system
    [ -z "$boot" ] || hw_disk "$boot" boot
    for d in "$@"; do hw_disk "$d" target; done
}
