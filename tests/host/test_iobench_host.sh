#!/bin/sh
# iobench_host.sh (workload B3): two rounds with a small payload, the report schema, the data checks and the
# table. The host-fuse rows are checked only where FUSE can be mounted. Host only.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
export RWYCTL="$BUILD/rwyctl" MKSTORE="$BUILD/mkstore" RWYFUSE="$BUILD/rwy-fuse"
T=$(mktemp -d /var/tmp/rwy-iobench-hosttest.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail() { echo "IOBENCH HOST FAIL: $*" >&2; exit 1; }

sh tests/qemu/iobench_host.sh "$T/out" 2 1 >"$T/log" 2>&1 || { cat "$T/log" >&2; fail "run"; }
sh tests/qemu/iobench_host.sh "$T/out" 1 1 >"$T/log" 2>&1 || { cat "$T/log" >&2; fail "second run"; }

[ "$(grep -c . "$T/out/RUNS.CSV")" = 3 ] || fail "RUNS.CSV rows"
awk -F, 'NR > 1 && NF != 24 { exit 1 }' "$T/out/RUNS.CSV" || fail "RUNS.CSV columns"
awk -F, 'NR > 1 && (NF != 16 || $16 != "ok") { bad = 1 } END { exit bad }' "$T/out/SAMPLES.CSV" ||
    fail "a SAMPLES.CSV row is malformed or not ok"
for op in install read state meta; do
    grep -q ",host-ctl,$op," "$T/out/SAMPLES.CSV" || fail "no host-ctl $op row"
done
if [ -e /dev/fuse ] && command -v fusermount3 >/dev/null 2>&1 && [ -x "$RWYFUSE" ]; then
    for op in mount install read state meta; do
        grep -q ",host-fuse,$op," "$T/out/SAMPLES.CSV" || fail "no host-fuse $op row"
    done
fi
sh tests/qemu/iobench_csv_table.sh "$T/out" >"$T/table" || fail "table"
grep -Eq '^host-ctl +install +1048576 +3 ' "$T/table" || { cat "$T/table" >&2; fail "table row"; }
sh tests/qemu/iobench_host.sh >/dev/null 2>&1 && fail "no directory accepted"
echo "IOBENCH HOST OK"
