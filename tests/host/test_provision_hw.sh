#!/bin/sh
# provision_hw.sh on the machine running the test: the sections and keys of the report, and no line that is not
# a section or a key=value pair. It reads the first real disk of the host; without one the test is skipped.
# SPDX-License-Identifier: Apache-2.0
set -u
fail() { echo "PROVISION HW FAIL: $*" >&2; exit 1; }
disk=$(ls /sys/block 2>/dev/null | grep -vE '^(loop|ram|dm-|zram|md|sr|nbd)' | head -n 1)
[ -n "$disk" ] || { echo "skip: no disk"; exit 77; }
# shellcheck disable=SC1091
. tests/qemu/provision_hw.sh
out=$(hw_report "/dev/$disk" "/dev/$disk") || fail "hw_report"
echo "$out" | grep -q '^\[system\]$' || fail "no system section"
echo "$out" | grep -q "^\[disk /dev/$disk boot\]$" || fail "no boot disk section"
echo "$out" | grep -q "^\[disk /dev/$disk target\]$" || fail "no target disk section"
for k in cpu_model cpus cpu_aes cpufreq_driver cpufreq_governor cpufreq_cur_mhz intel_pstate_status mem_total_kib arch kernel model size_bytes bus link_speed; do
    echo "$out" | grep -q "^$k=." || fail "no $k"
done
echo "$out" | grep -qE '^size_bytes=[0-9]+$' || fail "size is not a number"
echo "$out" | grep -vE '^(\[[^]]+\]|[a-z_]+=.*)$' | grep -q . && fail "a line is neither a section nor a key"
echo "PROVISION HW OK"
