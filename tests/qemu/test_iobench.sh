#!/bin/sh
# tests/qemu/test_iobench.sh — Time file, raw block I/O and Runway on both backends on one disk under QEMU.
# SPDX-License-Identifier: Apache-2.0
# The numbers show the firmware's driver cost on a virtual disk, not a device speed; the test checks the data only.
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
LOG="$BUILD/iobench.log"
IMG="$BUILD/iobench.img"
RWYCTL="$BUILD/rwyctl"
RUNS_HEADER='run,disk,status,arch,build,bench_version,firmware,system_model,devpath,media_id,disk_bytes,block_size,ioalign,write_cache,store_uuid,store_kind,disk_set,config,started,finished,reason,tsc_hz,timing_note,aes'
SAMPLES_HEADER='run,disk,dev,op,size,sample,stat,seconds,bytes,calls_core,calls_dev,us_per_call,flush_seconds,extents,encrypted,result'

sh tests/qemu/mkiobench.sh "$IMG" "$BUILD/IOBENCH.EFI"
qemu_vars_copy

# shellcheck disable=SC2046
timeout 600 "$QEMU_BIN" -nographic $(qemu_args) \
    -drive format=raw,file="$IMG" -serial stdio -display none > "$LOG" 2>&1 || true

grep -E '^(block area|iobench|IOBENCH)' "$LOG"
grep -q '^IOBENCH DONE' "$LOG" || { echo "FAILED: no IOBENCH DONE in $LOG" >&2; exit 1; }
grep -q '^IOBENCH FAIL' "$LOG" && { echo "FAILED: IOBENCH FAIL in $LOG" >&2; exit 1; }
# Five devices, four run sizes, write and read; the three stores also read their large object.
[ "$(grep -c '^IOBENCH .* KiB: .*MiB/s' "$LOG")" -eq 43 ] || { echo "FAILED: expected 43 timed lines" >&2; exit 1; }
for d in rwy-blk rwy-file rwy-enc; do
    grep -q "^IOBENCH calls $d  *open" "$LOG" || { echo "FAILED: no open line of $d" >&2; exit 1; }
done
sh tests/qemu/iobench_table.sh "$LOG"

off=$(img_part_off "$IMG" 4)
len=$(img_part_bytes "$IMG" 4)
result="$IMG@$off+$len"
[ "$("$RWYCTL" --store "$result" cat PERF/RUNS.CSV | head -n1)" = "$RUNS_HEADER" ] ||
    { echo "FAILED: bad RUNS.CSV header" >&2; exit 1; }
[ "$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | head -n1)" = "$SAMPLES_HEADER" ] ||
    { echo "FAILED: bad SAMPLES.CSV header" >&2; exit 1; }
[ "$("$RWYCTL" --store "$result" cat PERF/RUNS.CSV | wc -l)" -eq 2 ] ||
    { echo "FAILED: expected one run row" >&2; exit 1; }
# The 43 timed runs, the workloads (B1 on three stores, 4 phases each, and B2 on two, 2 phases each) and the
# cipher alone (encrypt and decrypt).
[ "$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep -vc -e ',cipher,[a-z]*-' -e ',wpath,' -e ',\(read\|write\)-fw,')" -eq 62 ] ||
    { echo "FAILED: expected 61 sample rows" >&2; exit 1; }
[ "$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep -c ',cipher,\(encrypt\|decrypt\),')" -eq 2 ] ||
    { echo "FAILED: expected an encrypt and a decrypt sample of the cipher" >&2; exit 1; }
# The firmware time rows (read-fw, write-fw), the write path cells (device wpath), the hyphenated cells of the cipher (cache, bounce, copy) and the clock rows
# (only where the CPU has the APERF and MPERF counters) are left out of the counts. The sector spread is always printed.
[ "$(grep -c '^IOBENCH cipher .*: sector [0-9]* to [0-9]* us' "$LOG")" -eq 2 ] ||
    { echo "FAILED: expected a sector spread line for encrypt and decrypt" >&2; exit 1; }
for op in plain-64k copy-64k encrypt-64k encrypt-copy-64k plain-128k copy-128k encrypt-128k encrypt-copy-128k; do
    [ "$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep -c ",wpath,$op,")" -eq 1 ] ||
        { echo "FAILED: expected a $op sample of the write path" >&2; exit 1; }
done
for d in rwy-blk rwy-file rwy-enc; do
    for op in read-fw write-fw; do
        [ "$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep -c ",$d,$op,")" -eq 4 ] ||
            { echo "FAILED: expected 4 $op samples of $d" >&2; exit 1; }
    done
done
for op in encrypt-cache encrypt-bounce copy-bounce copy-area; do
    [ "$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep -c ",cipher,$op,")" -eq 1 ] ||
        { echo "FAILED: expected a $op sample of the cipher" >&2; exit 1; }
done
for op in mount lookup read-frag state; do
    [ "$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep -c ",$op,")" -eq 3 ] ||
        { echo "FAILED: expected a $op sample on each store" >&2; exit 1; }
done
for op in install meta; do
    [ "$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep -c ",$op,")" -eq 2 ] ||
        { echo "FAILED: expected an $op sample on rwy-blk and rwy-enc" >&2; exit 1; }
done
"$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep -q ',failed$' &&
    { echo "FAILED: a sample failed its check" >&2; exit 1; }
"$RWYCTL" --store "$result" cat PERF/RUNS.CSV | grep -q ',ok,' ||
    { echo "FAILED: run status is not ok" >&2; exit 1; }
"$RWYCTL" --store "$result" cat PERF/RUNS.CSV | grep -q ',[1-9][0-9]*,reads may be warm' ||
    { echo "FAILED: no cycle counter frequency or timing note" >&2; exit 1; }
"$RWYCTL" --store "$result" cat PERF/RUNS.CSV | grep -q ',all,' ||
    { echo "FAILED: disk set is not all" >&2; exit 1; }
"$RWYCTL" --store "$result" cat PERF/RUNS.CSV | grep -q 'layout=1 label=bf rounds=1 warmup=0 payload_mib=16 devices=all disks=all include_boot=1 result=auto' ||
    { echo "FAILED: config was not recorded" >&2; exit 1; }
for d in block rwy-blk file rwy-file rwy-enc; do
    "$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep -q ",$d," ||
        { echo "FAILED: no $d sample" >&2; exit 1; }
done
if "$RWYCTL" --store "$result" cat PERF/RUNNING >/dev/null 2>&1; then
    echo "FAILED: RUNNING marker was not removed" >&2
    exit 1
fi

# The results store is absent: the same report must appear on the ESP.
IMG="$BUILD/iobench-fallback.img"
LOG="$BUILD/iobench-fallback.log"
sh tests/qemu/mkiobench.sh "$IMG" "$BUILD/IOBENCH.EFI"
sfdisk --part-uuid "$IMG" 4 5D0A7D6E-3B1C-4C1E-9A42-6F1B8E7021C8 >/dev/null
qemu_vars_copy
# shellcheck disable=SC2046
timeout 600 "$QEMU_BIN" -nographic $(qemu_args) \
    -drive format=raw,file="$IMG" -serial stdio -display none > "$LOG" 2>&1 || true
grep -q '^IOBENCH DONE' "$LOG" || { echo "FAILED: no IOBENCH DONE in fallback run" >&2; exit 1; }
esp=$((2048 * 512))
[ "$(mtype -i "$IMG@@$esp" ::/PERF/RUNS.CSV | head -n1)" = "$RUNS_HEADER" ] ||
    { echo "FAILED: bad ESP RUNS.CSV header" >&2; exit 1; }
[ "$(mtype -i "$IMG@@$esp" ::/PERF/SAMPLES.CSV | head -n1)" = "$SAMPLES_HEADER" ] ||
    { echo "FAILED: bad ESP SAMPLES.CSV header" >&2; exit 1; }
[ "$(mtype -i "$IMG@@$esp" ::/PERF/RUNS.CSV | wc -l)" -eq 2 ] ||
    { echo "FAILED: expected one ESP run row" >&2; exit 1; }
[ "$(mtype -i "$IMG@@$esp" ::/PERF/SAMPLES.CSV | grep -vc -e ',cipher,[a-z]*-' -e ',wpath,' -e ',\(read\|write\)-fw,')" -eq 62 ] ||
    { echo "FAILED: expected 61 ESP sample rows" >&2; exit 1; }
if mtype -i "$IMG@@$esp" ::/PERF/RUNNING >/dev/null 2>&1; then
    echo "FAILED: ESP RUNNING marker was not removed" >&2
    exit 1
fi

# A config that selects devices and sends the report to the ESP.
IMG="$BUILD/iobench-config.img"
LOG="$BUILD/iobench-config.log"
IOBENCH_DEVICES=block,file IOBENCH_RESULT=esp IOBENCH_DISKS=boot IOBENCH_LABEL=cfg \
    sh tests/qemu/mkiobench.sh "$IMG" "$BUILD/IOBENCH.EFI"
qemu_vars_copy
# shellcheck disable=SC2046
timeout 600 "$QEMU_BIN" -nographic $(qemu_args) \
    -drive format=raw,file="$IMG" -serial stdio -display none > "$LOG" 2>&1 || true
grep -q '^iobench: config layout=1 label=cfg .* devices=block,file ' "$LOG" ||
    { echo "FAILED: config was not loaded" >&2; exit 1; }
grep -q '^iobench: results=esp' "$LOG" || { echo "FAILED: results did not go to the ESP" >&2; exit 1; }
grep -q '^IOBENCH DONE' "$LOG" || { echo "FAILED: no IOBENCH DONE in config run" >&2; exit 1; }
[ "$(grep -c '^IOBENCH .* KiB: .*MiB/s' "$LOG")" -eq 16 ] ||
    { echo "FAILED: expected 16 configured timed lines" >&2; exit 1; }
[ "$(mtype -i "$IMG@@$esp" ::/PERF/RUNS.CSV | wc -l)" -eq 2 ] ||
    { echo "FAILED: expected one configured run row" >&2; exit 1; }
[ "$(mtype -i "$IMG@@$esp" ::/PERF/SAMPLES.CSV | grep -vc -e ',wpath,' -e ',\(read\|write\)-fw,')" -eq 17 ] ||
    { echo "FAILED: expected 16 configured sample rows" >&2; exit 1; }
mtype -i "$IMG@@$esp" ::/PERF/RUNS.CSV | grep -q ',boot,' ||
    { echo "FAILED: configured disk set is not boot" >&2; exit 1; }
mtype -i "$IMG@@$esp" ::/PERF/RUNS.CSV | grep -q 'label=cfg.*devices=block,file.*disks=boot.*result=esp' ||
    { echo "FAILED: configured settings were not recorded" >&2; exit 1; }
mtype -i "$IMG@@$esp" ::/PERF/SAMPLES.CSV | grep -q ',block,' ||
    { echo "FAILED: no block sample in config run" >&2; exit 1; }
mtype -i "$IMG@@$esp" ::/PERF/SAMPLES.CSV | grep -q ',file,' ||
    { echo "FAILED: no file sample in config run" >&2; exit 1; }
if mtype -i "$IMG@@$esp" ::/PERF/SAMPLES.CSV | grep -q ',rwy-'; then
    echo "FAILED: an unselected Runway sample was recorded" >&2
    exit 1
fi

# Two rounds after a warm-up: the warm-up is not recorded and every cell gets a median, a lowest and a highest row.
IMG="$BUILD/iobench-rounds.img"
LOG="$BUILD/iobench-rounds.log"
IOBENCH_ROUNDS=2 IOBENCH_WARMUP=1 IOBENCH_DEVICES=block,rwy-blk IOBENCH_RESULT=esp IOBENCH_DISKS=boot \
    sh tests/qemu/mkiobench.sh "$IMG" "$BUILD/IOBENCH.EFI"
qemu_vars_copy
# shellcheck disable=SC2046
timeout 900 "$QEMU_BIN" -nographic $(qemu_args) \
    -drive format=raw,file="$IMG" -serial stdio -display none > "$LOG" 2>&1 || true
grep -q '^IOBENCH DONE' "$LOG" || { echo "FAILED: no IOBENCH DONE in rounds run" >&2; exit 1; }
samples=$(mtype -i "$IMG@@$esp" ::/PERF/SAMPLES.CSV | grep -v -e ',wpath,' -e ',\(read\|write\)-fw,')
for r in 0 1; do
    [ "$(echo "$samples" | grep -c ",$r,sample,")" -eq 23 ] ||
        { echo "FAILED: expected 23 samples in round $r" >&2; exit 1; }
done
echo "$samples" | grep -q ',2,sample,' && { echo "FAILED: a third round was recorded" >&2; exit 1; }
for stat in median min max; do
    [ "$(echo "$samples" | grep -c ",$stat,")" -eq 23 ] ||
        { echo "FAILED: expected 23 $stat rows" >&2; exit 1; }
done
echo "$samples" | grep -q ',failed$' && { echo "FAILED: a sample failed its check" >&2; exit 1; }

echo ">>> SUCCESS: file, block and Runway runs and the report paths were verified"
