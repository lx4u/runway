#!/bin/sh
# tests/qemu/iobench_collect.sh — Copy the benchmark reports out of the results store of a boot disk.
# SPDX-License-Identifier: Apache-2.0
# Usage: tests/qemu/iobench_collect.sh OUTDIR [DISK|IMAGE]
# Without DISK it uses the partition with the results GUID from /dev/disk/by-partuuid. It writes RUNS.CSV and
# SAMPLES.CSV to OUTDIR (one directory per boot disk or campaign) and needs read access to the partition only.
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
RESULT_GUID=5D0A7D6E-3B1C-4C1E-9A42-6F1B8E7021C7
BUILD=${BUILD:-build}
RWYCTL=${RWYCTL:-$BUILD/rwyctl}
[ -x "$RWYCTL" ] || RWYCTL=$BUILD/tools/rwyctl
[ -x "$RWYCTL" ] || RWYCTL=$(command -v rwyctl) || { echo "missing rwyctl" >&2; exit 1; }

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    echo "usage: $0 OUTDIR [DISK|IMAGE]" >&2
    exit 1
fi
out=$1 disk=${2:-}

if [ -n "$disk" ]; then
    # Find the partition on the disk and read it through a window.
    window=$(sfdisk -d "$disk" | awk -v g="$RESULT_GUID" '
        index(toupper($0), g) {
            match($0, /start= *[0-9]+/); s = substr($0, RSTART + 6, RLENGTH - 6) + 0
            match($0, /size= *[0-9]+/); n = substr($0, RSTART + 5, RLENGTH - 5) + 0
            printf "%d+%d\n", s * 512, n * 512; exit }')
    [ -n "$window" ] || { echo "no results partition ($RESULT_GUID) on $disk" >&2; exit 1; }
    store="$disk@$window"
else
    store=$(echo "$RESULT_GUID" | tr A-F a-f)
    store=/dev/disk/by-partuuid/$store
    [ -e "$store" ] || { echo "no results partition ($RESULT_GUID) found" >&2; exit 1; }
fi

mkdir -p "$out"
for f in RUNS SAMPLES; do
    "$RWYCTL" --store "$store" cat "PERF/$f.CSV" >"$out/$f.CSV" ||
        { echo "cannot read PERF/$f.CSV from $store" >&2; exit 1; }
done
# The facts about the machine, when the provisioning slot saved them.
"$RWYCTL" --store "$store" cat PERF/HW.TXT >"$out/HW.TXT" 2>/dev/null || rm -f "$out/HW.TXT"
echo "$out: $(($(wc -l <"$out/RUNS.CSV") - 1)) run row(s), $(($(wc -l <"$out/SAMPLES.CSV") - 1)) sample row(s)"
