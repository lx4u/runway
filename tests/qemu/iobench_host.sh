#!/bin/sh
# tests/qemu/iobench_host.sh — Benchmark rwyctl and rwy-fuse on Linux (workload B3 of ADR 0020).
# SPDX-License-Identifier: Apache-2.0
# Usage: tests/qemu/iobench_host.sh OUTDIR [ROUNDS] [PAYLOAD_MIB]
# It makes a scratch store, times a large write, a large read, a small write and a small read through rwyctl
# (device host-ctl) and through a rwy-fuse mount (device host-fuse), and appends rows in the schema of the EFI
# application to OUTDIR/RUNS.CSV and OUTDIR/SAMPLES.CSV. iobench_csv_table.sh reads OUTDIR like any other
# campaign. Every read is compared with what was written; a mismatch sets the result to "bad".
set -eu
BUILD=${BUILD:-build}
MKSTORE=${MKSTORE:-$BUILD/mkstore}; [ -x "$MKSTORE" ] || MKSTORE=$BUILD/tools/mkstore
RWYCTL=${RWYCTL:-$BUILD/rwyctl}; [ -x "$RWYCTL" ] || RWYCTL=$BUILD/tools/rwyctl
RWYFUSE=${RWYFUSE:-$BUILD/rwy-fuse}; [ -x "$RWYFUSE" ] || RWYFUSE=$BUILD/fuse/rwy-fuse
[ -x "$MKSTORE" ] && [ -x "$RWYCTL" ] || { echo "missing mkstore or rwyctl" >&2; exit 1; }

[ $# -ge 1 ] && [ $# -le 3 ] || { echo "usage: $0 OUTDIR [ROUNDS] [PAYLOAD_MIB]" >&2; exit 1; }
out=$1 rounds=${2:-3} mib=${3:-16}
big=$((mib * 1048576)) small=4096
run=host-$(date -u +%Y%m%dT%H%M%SZ)
T=$(mktemp -d /var/tmp/rwy-iobench-host.XXXXXX)
M=$T/mnt
mounted=
cleanup() {
    [ -z "$mounted" ] || fusermount3 -u "$M" 2>/dev/null || true
    rm -rf "$T"
}
trap cleanup EXIT

# Seconds since the epoch in microseconds.
now() { echo $(($(date +%s%N) / 1000)); }
# Print microseconds as seconds with six decimals.
secs() { printf '%d.%06d' $(($1 / 1000000)) $(($1 % 1000000)); }

mkdir -p "$out"
[ -s "$out/SAMPLES.CSV" ] ||
    echo 'run,disk,dev,op,size,sample,stat,seconds,bytes,calls_core,calls_dev,us_per_call,flush_seconds,extents,encrypted,result' >"$out/SAMPLES.CSV"
[ -s "$out/RUNS.CSV" ] ||
    echo 'run,disk,status,arch,build,bench_version,firmware,system_model,devpath,media_id,disk_bytes,block_size,ioalign,write_cache,store_uuid,store_kind,disk_set,config,started,finished,reason,tsc_hz,timing_note,aes' >"$out/RUNS.CSV"

# emit DEV OP SIZE SAMPLE MICROSECONDS BYTES FLUSH_MICROSECONDS RESULT
emit() {
    echo "$run,host,$1,$2,$3,$4,sample,$(secs "$5"),$6,0,0,0.00,$(secs "$7"),0,0,$8" >>"$out/SAMPLES.CSV"
}

head -c "$big" /dev/urandom >"$T/big"
head -c "$small" /dev/urandom >"$T/small"
started=$(date -u +%FT%TZ)

ctl() { "$RWYCTL" --store "$T/s.img" "$@"; }

# Compare what a command reads with the source file; print "ok" or "bad".
check() { cmp -s "$1" "$2" && echo ok || echo bad; }

bench_ctl() {
    n=$1
    t0=$(now); ctl put BIG "$T/big"; t1=$(now)
    emit host-ctl install "$big" "$n" $((t1 - t0)) "$big" 0 ok
    t0=$(now); ctl cat BIG >"$T/big.out"; t1=$(now)
    emit host-ctl read "$big" "$n" $((t1 - t0)) "$big" 0 "$(check "$T/big" "$T/big.out")"
    t0=$(now); ctl put SMALL "$T/small"; t1=$(now)
    emit host-ctl state "$small" "$n" $((t1 - t0)) "$small" 0 ok
    t0=$(now); ctl cat SMALL >"$T/small.out"; t1=$(now)
    emit host-ctl meta "$small" "$n" $((t1 - t0)) "$small" 0 "$(check "$T/small" "$T/small.out")"
}

bench_fuse() {
    n=$1
    t0=$(now); "$RWYFUSE" -s "$T/s.img" "$M"; t1=$(now)
    mounted=1
    emit host-fuse mount 0 "$n" $((t1 - t0)) 0 0 ok
    t0=$(now); cp "$T/big" "$M/BIG"; t1=$(now); sync -f "$M/BIG"; t2=$(now)
    emit host-fuse install "$big" "$n" $((t2 - t0)) "$big" $((t2 - t1)) ok
    t0=$(now); cat "$M/BIG" >"$T/big.out"; t1=$(now)
    emit host-fuse read "$big" "$n" $((t1 - t0)) "$big" 0 "$(check "$T/big" "$T/big.out")"
    t0=$(now); cp "$T/small" "$M/SMALL"; t1=$(now); sync -f "$M/SMALL"; t2=$(now)
    emit host-fuse state "$small" "$n" $((t2 - t0)) "$small" $((t2 - t1)) ok
    t0=$(now); cat "$M/SMALL" >"$T/small.out"; t1=$(now)
    emit host-fuse meta "$small" "$n" $((t1 - t0)) "$small" 0 "$(check "$T/small" "$T/small.out")"
    fusermount3 -u "$M"; mounted=
}

"$MKSTORE" --size $((mib * 3 + 16)) --flight-records 0 --journal-records 0 "$T/s.img" >/dev/null
mkdir "$M"
fuse=1
[ -e /dev/fuse ] && [ -x "$RWYFUSE" ] && command -v fusermount3 >/dev/null || { fuse=; echo "no FUSE: host-fuse skipped" >&2; }

n=1
while [ "$n" -le "$rounds" ]; do
    bench_ctl "$n"
    [ -z "$fuse" ] || bench_fuse "$n"
    n=$((n + 1))
done

finished=$(date -u +%FT%TZ)
uuid=$(ctl stores 2>/dev/null | grep -Eo '^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}' | head -1 || true)
echo "$run,host,done,$(uname -m),$(git rev-parse --short HEAD 2>/dev/null || echo unknown),host1,linux $(uname -r),$(cat /sys/class/dmi/id/product_name 2>/dev/null || echo unknown),file,0,$((mib * 3 + 16)),512,0,0,${uuid:-unknown},plain,host,rounds=$rounds payload_mib=$mib,$started,$finished,,0,host wall clock; reads may be warm in the page cache,openssl" >>"$out/RUNS.CSV"
echo "$out: $rounds round(s), run $run"
