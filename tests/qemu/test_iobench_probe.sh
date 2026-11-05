#!/bin/sh
# tests/qemu/test_iobench_probe.sh — run the read-delay probe on the raw area and check that every cell reports.
# SPDX-License-Identifier: Apache-2.0
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
LOG="$BUILD/iobench-probe.log"
IMG="$BUILD/iobench-probe.img"
RWYCTL="$BUILD/rwyctl"
CELLS="r64k-first w4k r64k-after-w4k r64k-again r4k-after-r64k w4k-second r64k-after-pause w64k r64k-after-w64k
    r64k-gap-0us r64k-gap-20us r64k-gap-100us r64k-gap-500us r64k-gap-2000us r64k-store-region"

IOBENCH_DEVICES=block,probe sh tests/qemu/mkiobench.sh "$IMG" "$BUILD/IOBENCH.EFI"
qemu_vars_copy

# shellcheck disable=SC2046
timeout 600 "$QEMU_BIN" -nographic $(qemu_args) \
    -drive format=raw,file="$IMG" -serial stdio -display none > "$LOG" 2>&1 || true

grep -E '^(iobench|IOBENCH probe)' "$LOG"
grep -q '^IOBENCH DONE' "$LOG" || { echo "FAILED: no IOBENCH DONE in $LOG" >&2; exit 1; }
grep -q '^IOBENCH FAIL' "$LOG" && { echo "FAILED: IOBENCH FAIL in $LOG" >&2; exit 1; }
for c in $CELLS; do
    grep -q "^IOBENCH probe $c  *slowest call [0-9]* of [0-9]*: [0-9]* us, [0-9]* over [0-9]* us" "$LOG" ||
        { echo "FAILED: no probe line for $c" >&2; exit 1; }
done

result="$IMG@$(img_part_off "$IMG" 4)+$(img_part_bytes "$IMG" 4)"
samples=$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV)
for c in $CELLS; do
    echo "$samples" | grep -q ",probe,$c," || { echo "FAILED: no $c sample in the report" >&2; exit 1; }
done
echo "$samples" | grep ',probe,' | grep -q ',failed$' && { echo "FAILED: a probe cell failed" >&2; exit 1; }
echo ">>> SUCCESS: the probe ran every cell and reported it"
