#!/bin/sh
# iobench_collect.sh and iobench_csv_table.sh on a results image made by hand: partition lookup by GUID,
# the dump, a missing partition, and the table (median, spread, bad rows, base comparison). Host only.
# SPDX-License-Identifier: Apache-2.0
set -u
export PATH="$PATH:/sbin:/usr/sbin"
BUILD=${BUILD:-build}
export RWYCTL="$BUILD/rwyctl" MKSTORE="$BUILD/mkstore"
T=$(mktemp -d /var/tmp/rwy-iobench-collect.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail() { echo "IOBENCH COLLECT FAIL: $*" >&2; exit 1; }
command -v sfdisk >/dev/null 2>&1 || { echo "skip: no sfdisk"; exit 77; }
COLLECT=tests/qemu/iobench_collect.sh
TABLE=tests/qemu/iobench_csv_table.sh
GUID=5D0A7D6E-3B1C-4C1E-9A42-6F1B8E7021C7
RUNS='run,disk,status,arch'
SAMPLES_HDR='run,disk,dev,op,size,sample,stat,seconds,bytes,calls_core,calls_dev,us_per_call,flush_seconds,extents,encrypted,result'

# An image with a decoy partition first and the results partition second.
truncate -s 12M "$T/boot.img"
sfdisk "$T/boot.img" >/dev/null <<PARTS || fail sfdisk
label: gpt
size=2MiB, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, uuid=5D0A7D6E-3B1C-4C1E-9A42-6F1B8E7021C9
size=8MiB, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, uuid=$GUID
PARTS
off=$(sfdisk -d "$T/boot.img" | awk -v g="$GUID" 'index(toupper($0), g) { match($0, /start= *[0-9]+/); print (substr($0, RSTART + 6, RLENGTH - 6) + 0) * 512 }')

# Two boots, two disks; one failed sample, one bad row and one stat row that must not count.
row() { echo "$1,$2,$3,$4,$5,$6,$7,$8,$9,1,1,0,0,0,0,${10}"; }
{
    echo "$SAMPLES_HDR"
    row b1 d1 rwy-blk write 1048576 1 sample 0.10 1048576 ok
    row b1 d1 rwy-blk write 1048576 2 sample 0.20 1048576 ok
    row b1 d1 rwy-blk write 1048576 3 sample 0.40 1048576 ok
    row b1 d1 rwy-blk write 1048576 3 median 0.20 1048576 ok
    row b1 d2 rwy-blk write 1048576 1 sample 9.00 1048576 failed
    row b1 d1 rwy-blk mount 0 1 sample 0.001 0 ok
    echo "b1,d1,short,row"
    row b2 d1 rwy-blk write 1048576 1 sample 0.20 1048576 ok
} >"$T/samples"
echo "$RUNS" >"$T/runs"
"$MKSTORE" --size 8 --flight-records 0 --journal-records 0 --add PERF/SAMPLES.CSV="$T/samples" \
    --add PERF/RUNS.CSV="$T/runs" "$T/boot.img@$off+8388608" >/dev/null || fail mkstore

sh $COLLECT "$T/a" "$T/boot.img" >"$T/out" 2>&1 || { cat "$T/out" >&2; fail "collect"; }
cmp "$T/a/SAMPLES.CSV" "$T/samples" || fail "SAMPLES.CSV differs"
cmp "$T/a/RUNS.CSV" "$T/runs" || fail "RUNS.CSV differs"

truncate -s 4M "$T/none.img"
sfdisk "$T/none.img" >/dev/null <<PARTS
label: gpt
size=2MiB, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
PARTS
sh $COLLECT "$T/b" "$T/none.img" >"$T/out" 2>&1 && fail "collect from a disk without results succeeded"
grep -q 'no results partition' "$T/out" || fail "missing partition not reported"

# write: 4 ok samples (0.10 0.20 0.40 0.20) -> median 0.2; mount is a cell without MiB/s.
sh $TABLE "$T/a" >"$T/table" || fail "table"
grep -E '^rwy-blk +write +1048576 +4 +0\.200000 +0\.100000 +0\.400000 +5\.0 *$' "$T/table" >/dev/null || { cat "$T/table" >&2; fail "write row"; }
grep -E '^rwy-blk +mount +0 +1 .* +- *$' "$T/table" >/dev/null || { cat "$T/table" >&2; fail "mount row"; }

# A campaign twice as slow: base median 0.2, new 0.4 -> 200%.
mkdir "$T/slow"
{ echo "$SAMPLES_HDR"; row s1 d1 rwy-blk write 1048576 1 sample 0.40 1048576 ok; } >"$T/slow/SAMPLES.CSV"
sh $TABLE --base "$T/a" "$T/slow" >"$T/table" || fail "compare"
grep -E '^rwy-blk +write .* 200% *$' "$T/table" >/dev/null || { cat "$T/table" >&2; fail "compare row"; }

# The device path is quoted and holds a comma: the row must still count.
mkdir "$T/quoted"
{ echo "$SAMPLES_HDR"; row q1 '"Pci(0x1F,0x2)/Sata(0x1)"' block write 4096 1 sample 0.50 4096 ok; } >"$T/quoted/SAMPLES.CSV"
sh $TABLE "$T/quoted" >"$T/table" || fail "quoted table"
grep -E '^block +write +4096 +1 ' "$T/table" >/dev/null || { cat "$T/table" >&2; fail "quoted row"; }

sh $TABLE >"$T/out" 2>&1 && fail "table without a directory succeeded"
echo "IOBENCH COLLECT OK"
