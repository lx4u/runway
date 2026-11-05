#!/bin/sh
# tests/qemu/iobench_csv_table.sh — Table of the benchmark reports collected by iobench_collect.sh.
# SPDX-License-Identifier: Apache-2.0
# Usage: tests/qemu/iobench_csv_table.sh [--base DIR]... DIR...
# The rows are the ok sample rows of all boots and disks in the DIRs, by device, operation and size: the median
# time with the lowest and highest, and the median MiB/s where the cell moves data. With --base DIR (repeatable)
# the same table is made for that campaign and a column gives the new median time as a share of the base one.
# The old serial logs stay with iobench_table.sh.
set -eu
base=
while [ $# -gt 0 ] && [ "$1" = --base ]; do
    [ $# -ge 2 ] || { echo "--base needs a directory" >&2; exit 1; }
    base="$base $2"; shift 2
done
[ $# -gt 0 ] || { echo "usage: $0 [--base DIR]... DIR..." >&2; exit 1; }

# A campaign is the sample rows of its directories, tagged with the campaign number (0 = base, 1 = new).
rows() {
    tag=$1; shift
    for d in "$@"; do
        [ -f "$d/SAMPLES.CSV" ] || { echo "$d: no SAMPLES.CSV" >&2; exit 1; }
        # A quoted field (the device path) may hold commas: turn them into ';' before the split.
        awk -v t="$tag" -v d="$d" '
            { s = ""; q = 0
              for (i = 1; i <= length($0); i++) {
                  c = substr($0, i, 1)
                  if (c == "\"") { q = !q; continue }
                  s = s ((c == "," && q) ? ";" : c)
              }
              $0 = s }
            NR > 1 && NF == 16 && $7 == "sample" && $16 == "ok" { print t "," d "," $0 }' FS=, "$d/SAMPLES.CSV"
    done
}

{
    # shellcheck disable=SC2086
    [ -z "$base" ] || rows 0 $base
    rows 1 "$@"
} | awk -F, -v cmp="${base:+1}" '
function sort(a, n,    i, j, t) {
    for (i = 2; i <= n; i++) {
        t = a[i]
        for (j = i - 1; j >= 1 && a[j] > t; j--) a[j + 1] = a[j]
        a[j + 1] = t
    }
}
function med(k, c,    i, v, n) {
    n = cnt[c, k]
    for (i = 1; i <= n; i++) v[i] = sec[c, k, i]
    sort(v, n)
    return n % 2 ? v[(n + 1) / 2] : (v[n / 2] + v[n / 2 + 1]) / 2
}
function mibs(k, c,    i, v, n) {
    n = cnt[c, k]
    for (i = 1; i <= n; i++) v[i] = rate[c, k, i]
    sort(v, n)
    return n % 2 ? v[(n + 1) / 2] : (v[n / 2] + v[n / 2 + 1]) / 2
}
# Fields: tag dir run disk dev op size sample stat seconds bytes ... result
{
    c = $1; k = $5 "," $6 "," $7
    if (!(k in seen)) { seen[k] = 1; keys[++nk] = k }
    n = ++cnt[c, k]; sec[c, k, n] = $10 + 0
    rate[c, k, n] = ($10 > 0 && $11 > 0) ? $11 / $10 / 1048576 : 0
    disks[c, k, $4] = 1; runs[c, k, $3 ","  $4] = 1
}
END {
    sort(keys, nk)
    printf "%-9s %-10s %9s %5s %12s %12s %12s %10s", "dev", "op", "size", "n", "median s", "lowest s", "highest s", "MiB/s"
    if (cmp) printf " %10s", "vs base"
    printf "\n"
    for (i = 1; i <= nk; i++) {
        k = keys[i]; if (!cnt[1, k]) continue
        split(k, p, ",")
        m = med(k, 1); lo = hi = sec[1, k, 1]
        for (j = 2; j <= cnt[1, k]; j++) { if (sec[1, k, j] < lo) lo = sec[1, k, j]; if (sec[1, k, j] > hi) hi = sec[1, k, j] }
        r = mibs(k, 1)
        printf "%-9s %-10s %9s %5d %12.6f %12.6f %12.6f %10s", p[1], p[2], p[3], cnt[1, k], m, lo, hi, (r > 0 ? sprintf("%.1f", r) : "-")
        if (cmp) {
            if (cnt[0, k] && med(k, 0) > 0) printf " %9.0f%%", 100 * m / med(k, 0)
            else printf " %10s", "-"
        }
        printf "\n"
    }
}'
