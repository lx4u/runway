#!/bin/sh
# iobench_linux on a scratch file: the files it writes have the schema of the report, every sample is ok, and the
# table tool reads them next to the other devices. Host only; no direct I/O, so any file system will do.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
TOOL=$BUILD/iobench_linux
T=$(mktemp -d /var/tmp/rwy-iobench-linux.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail() { echo "IOBENCH LINUX FAIL: $*" >&2; exit 1; }

truncate -s 128M "$T/disk"
"$TOOL" --rounds 2 --warmup 1 --no-direct --out "$T" "$T/disk" >"$T/log" 2>&1 || { cat "$T/log" >&2; fail "run"; }
grep -q '^IOBENCH linux DONE' "$T/log" || fail "no verdict"
[ "$(wc -l <"$T/RUNS.CSV")" = 2 ] || fail "RUNS.CSV rows"
awk 'BEGIN { FPAT = "([^,]*)|(\"[^\"]*\")" } NR > 1 && NF != 24 { exit 1 }' "$T/RUNS.CSV" || fail "RUNS.CSV columns"
awk 'BEGIN { FPAT = "([^,]*)|(\"[^\"]*\")" } NR > 1 && (NF != 16 || $16 != "ok") { bad = 1 } END { exit bad }' "$T/SAMPLES.CSV" ||
    fail "a sample is malformed or not ok"
# Two timed rounds: four run sizes for write and read, two for write-sync, and encrypt and decrypt of the cipher.
[ "$(grep -c ',linux-block,' "$T/SAMPLES.CSV")" = 20 ] || fail "linux-block rows"
[ "$(grep -c ',linux-block,write-sync,' "$T/SAMPLES.CSV")" = 4 ] || fail "write-sync rows"
[ "$(grep -c ',linux-cipher,\(en\|de\)crypt,' "$T/SAMPLES.CSV")" = 4 ] || fail "linux-cipher rows"
# Three clock rows per round, with the MHz of the add chain in calls_core.
[ "$(grep -c ',linux-cipher,[a-z]*-clock,' "$T/SAMPLES.CSV")" = 6 ] || fail "clock rows"
awk 'BEGIN { FPAT = "([^,]*)|(\"[^\"]*\")" } $4 ~ /-clock$/ && $10 < 100 { bad = 1 } END { exit bad }' "$T/SAMPLES.CSV" || fail "a clock is below 100 MHz"
grep -q ',0,sample,' "$T/SAMPLES.CSV" && grep -q ',1,sample,' "$T/SAMPLES.CSV" || fail "rounds are not numbered from 0"
sh tests/qemu/iobench_csv_table.sh "$T" >"$T/table" || fail "table"
grep -Eq '^linux-block +write +4096 +2 ' "$T/table" || { cat "$T/table" >&2; fail "table row"; }
grep -Eq '^linux-cipher +encrypt +0 +2 ' "$T/table" || { cat "$T/table" >&2; fail "cipher row"; }

truncate -s 64M "$T/small"
"$TOOL" --rounds 1 --no-direct --out "$T" "$T/small" >/dev/null 2>&1 && fail "a disk smaller than the area was accepted"
"$TOOL" >/dev/null 2>&1 && fail "no arguments were accepted"
echo "IOBENCH LINUX OK"
