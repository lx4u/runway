#!/bin/sh
# tests/qemu/test_iobench_disks.sh — discover and measure two prepared target disks.
# SPDX-License-Identifier: Apache-2.0
# The first target has an ESP, so the file cells run on it; the second has none and leaves them out.
# A third target has the benchmark partitions but no PERF/READY marker: it must be skipped and recorded.
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
LOG="$BUILD/iobench-disks.log"
BOOT="$BUILD/iobench-disks-boot.img"
T1="$BUILD/iobench-disks-1.img"
T2="$BUILD/iobench-disks-2.img"
T3="$BUILD/iobench-disks-3.img"
DEK="$BUILD/iobench-disks.dek"
KERNEL="$BUILD/iobench-disks.kernel"
INITRD="$BUILD/iobench-disks.initrd"
RWYCTL="$BUILD/rwyctl"
RUNS_HEADER='run,disk,status,arch,build,bench_version,firmware,system_model,devpath,media_id,disk_bytes,block_size,ioalign,write_cache,store_uuid,store_kind,disk_set,config,started,finished,reason,tsc_hz,timing_note,aes'
SAMPLES_HEADER='run,disk,dev,op,size,sample,stat,seconds,bytes,calls_core,calls_dev,us_per_call,flush_seconds,extents,encrypted,result'

[ -x "$RWYCTL" ] || RWYCTL="$BUILD/tools/rwyctl"
dd if=/dev/urandom of="$DEK" bs=32 count=1 2>/dev/null
printf x >"$KERNEL"
printf x >"$INITRD"
IOBENCH_ROUNDS=1 IOBENCH_WARMUP=0 IOBENCH_DEVICES=block,file,rwy-blk,rwy-file,rwy-enc IOBENCH_LABEL=multi \
    sh tests/qemu/mkiobench.sh --boot "$BOOT" "$BUILD/IOBENCH.EFI" "" "$KERNEL" "$INITRD" "$DEK"
sh tests/qemu/mkiobench.sh --target "$T1" "$DEK"
IOBENCH_NO_ESP=1 sh tests/qemu/mkiobench.sh --target "$T2" "$DEK"
IOBENCH_NO_MARKER=1 sh tests/qemu/mkiobench.sh --target "$T3" "$DEK"

qemu_vars_copy
# shellcheck disable=SC2046
timeout 900 "$QEMU_BIN" -nographic $(qemu_args) \
    -drive format=raw,file="$BOOT" \
    -drive format=raw,file="$T1" \
    -drive format=raw,file="$T2" \
    -drive format=raw,file="$T3" \
    -serial stdio -display none > "$LOG" 2>&1 || true

grep -E '^(iobench|IOBENCH)' "$LOG"
grep -q '^IOBENCH DONE' "$LOG" || { echo "FAILED: no IOBENCH DONE in $LOG" >&2; exit 1; }
grep -q '^IOBENCH FAIL' "$LOG" && { echo "FAILED: IOBENCH FAIL in $LOG" >&2; exit 1; }
grep -q '^iobench: 3 prepared disk(s)' "$LOG" ||
    { echo "FAILED: discovery did not find three disks" >&2; exit 1; }
grep -q 'no marker' "$LOG" || { echo "FAILED: missing marker was not reported" >&2; exit 1; }
grep -q 'has no ESP, the file cells are skipped' "$LOG" ||
    { echo "FAILED: the disk without an ESP was not reported" >&2; exit 1; }
# Two measured disks: block, rwy-blk and rwy-enc, each 8 timed runs plus the object read of the stores,
# and the file and rwy-file cells on the disk that has an ESP.
[ "$(grep -c '^IOBENCH .* KiB: .*MiB/s' "$LOG")" -eq 69 ] ||
    { echo "FAILED: expected 69 timed lines" >&2; exit 1; }

off=$(img_part_off "$BOOT" 3)
len=$(img_part_bytes "$BOOT" 3)
result="$BOOT@$off+$len"
[ "$("$RWYCTL" --store "$result" cat PERF/RUNS.CSV | head -n1)" = "$RUNS_HEADER" ] ||
    { echo "FAILED: bad RUNS.CSV header" >&2; exit 1; }
[ "$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | head -n1)" = "$SAMPLES_HEADER" ] ||
    { echo "FAILED: bad SAMPLES.CSV header" >&2; exit 1; }
[ "$("$RWYCTL" --store "$result" cat PERF/RUNS.CSV | wc -l)" -eq 4 ] ||
    { echo "FAILED: expected two ok rows and one skipped row" >&2; exit 1; }
[ "$("$RWYCTL" --store "$result" cat PERF/RUNS.CSV | grep -c ',ok,')" -eq 2 ] ||
    { echo "FAILED: expected two ok rows" >&2; exit 1; }
"$RWYCTL" --store "$result" cat PERF/RUNS.CSV | grep -q ',skipped,.*no marker' ||
    { echo "FAILED: skipped row is missing or has no reason" >&2; exit 1; }
[ "$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep -vc -e ',cipher,[a-z]*-' -e ',wpath,' -e ',\(read\|write\)-fw,')" -eq 102 ] ||
    { echo "FAILED: expected 101 sample rows" >&2; exit 1; }
[ "$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep -c ',cipher,\(encrypt\|decrypt\),')" -eq 4 ] ||
    { echo "FAILED: expected an encrypt and a decrypt sample of the cipher on each disk" >&2; exit 1; }
[ "$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep -c ',block,')" -eq 16 ] ||
    { echo "FAILED: wrong number of block samples" >&2; exit 1; }
for d in rwy-blk rwy-enc; do
    [ "$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep ",$d," | grep -vc ',\(read\|write\)-fw,')" -eq 30 ] ||
        { echo "FAILED: wrong number of $d samples" >&2; exit 1; }
done
[ "$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep -c ',file,')" -eq 8 ] ||
    { echo "FAILED: the file cells must run on one disk only" >&2; exit 1; }
[ "$("$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep ',rwy-file,' | grep -vc ',\(read\|write\)-fw,')" -eq 13 ] ||
    { echo "FAILED: wrong number of rwy-file samples" >&2; exit 1; }
"$RWYCTL" --store "$result" cat PERF/RUNS.CSV | grep -q 'no ESP: file cells skipped' ||
    { echo "FAILED: the skipped file cells are not in the report" >&2; exit 1; }
"$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep -q ',failed$' &&
    { echo "FAILED: a sample failed its check" >&2; exit 1; }
"$RWYCTL" --store "$result" cat PERF/RUNS.CSV | grep -q 'label=multi.*devices=all.*disks=internal.*include_boot=0' ||
    { echo "FAILED: config was not recorded" >&2; exit 1; }
if "$RWYCTL" --store "$result" cat PERF/RUNNING >/dev/null 2>&1; then
    echo "FAILED: RUNNING marker was not removed" >&2
    exit 1
fi

# The host collector and the table tool read the report of this run.
OUT=$(mktemp -d /var/tmp/rwy-iobench-disks.XXXXXX)
trap 'rm -rf "$OUT"' EXIT

# The report files are also on the ESP, byte for byte, so that any machine can read them.
esp_off=$(img_part_off "$BOOT" 1)
for f in RUNS SAMPLES; do
    mcopy -o -i "$BOOT@@$esp_off" "::PERF/$f.CSV" "$OUT/esp-$f.CSV" 2>/dev/null ||
        { echo "FAILED: no $f.CSV on the ESP" >&2; exit 1; }
    "$RWYCTL" --store "$result" cat "PERF/$f.CSV" | cmp -s - "$OUT/esp-$f.CSV" ||
        { echo "FAILED: $f.CSV on the ESP differs from the results store" >&2; exit 1; }
done
sh tests/qemu/iobench_collect.sh "$OUT/run" "$BOOT" >/dev/null ||
    { echo "FAILED: the collector cannot read the report" >&2; exit 1; }
[ "$(wc -l <"$OUT/run/RUNS.CSV")" -eq 4 ] ||
    { echo "FAILED: the collected RUNS.CSV differs from the store" >&2; exit 1; }
sh tests/qemu/iobench_csv_table.sh "$OUT/run" >"$OUT/table" ||
    { echo "FAILED: the table tool cannot parse the report" >&2; exit 1; }
for d in block file rwy-blk rwy-file rwy-enc; do
    grep -q "^$d " "$OUT/table" || { echo "FAILED: no $d row in the table" >&2; exit 1; }
done

echo ">>> SUCCESS: two target disks were measured and the unprepared disk was skipped"
