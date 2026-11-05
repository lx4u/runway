#!/bin/sh
# tests/qemu/iobench_table.sh — Print the IOBENCH lines of one or more logs as a table.
# SPDX-License-Identifier: Apache-2.0
# Usage: tests/qemu/iobench_table.sh LOG...
# The first table gives the median MiB/s of the logs and the speed of each Runway store as a share of its own
# baseline (block for rwy-blk and rwy-enc, file for rwy-file). The second gives the lowest and highest value.
set -eu
[ $# -gt 0 ] || { echo "usage: $0 LOG..." >&2; exit 1; }

# shellcheck disable=SC2016
grep -ahE '^IOBENCH [^ ]+ +(write|read|obj) +[0-9]+ KiB: .*MiB/s' "$@" | awk -v logs=$# '
function sort(a, n,    i, j, t) {
    for (i = 2; i <= n; i++) {
        t = a[i]
        for (j = i - 1; j >= 1 && a[j] > t; j--) a[j + 1] = a[j]
        a[j + 1] = t
    }
}
function key(dev, op, run) { return dev "/" op "/" run }
function med(dev, op, run,    k, i, v, n) {
    k = key(dev, op, run); n = cnt[k]
    for (i = 1; i <= n; i++) v[i] = val[k, i]
    sort(v, n)
    return n ? (n % 2 ? v[(n + 1) / 2] : (v[n / 2] + v[n / 2 + 1]) / 2) : 0
}
function lo(dev, op, run,    k, i, m) { k = key(dev, op, run); m = val[k, 1]; for (i = 2; i <= cnt[k]; i++) if (val[k, i] < m) m = val[k, i]; return m }
function hi(dev, op, run,    k, i, m) { k = key(dev, op, run); m = val[k, 1]; for (i = 2; i <= cnt[k]; i++) if (val[k, i] > m) m = val[k, i]; return m }
function share(a, b) { return b > 0 ? sprintf("%4.0f%%", 100 * a / b) : "    -" }
{
    dev = $2; op = $3; run = $4
    for (i = 1; i <= NF; i++) if ($i == "MiB/s") v = $(i - 1)
    k = key(dev, op, run); cnt[k]++; val[k, cnt[k]] = v
    if (!(run in seen)) { seen[run] = 1; runs[++nruns] = run }
}
END {
    split("write read", ops, " ")
    sort(runs, nruns)
    printf "MiB/s, median of %d log(s)\n", logs
    printf "%-9s %-5s %9s %9s %6s %9s %9s %6s %9s %6s\n", "run KiB", "op", "block", "rwy-blk", "of blk", "file", "rwy-file", "of fil", "rwy-enc", "of blk"
    for (r = 1; r <= nruns; r++) for (o = 1; o <= 2; o++) {
        run = runs[r]; op = ops[o]
        b = med("block", op, run); rb = med("rwy-blk", op, run); f = med("file", op, run); rf = med("rwy-file", op, run); re = med("rwy-enc", op, run)
        printf "%-9s %-5s %9.1f %9.1f %6s %9.1f %9.1f %6s %9.1f %6s\n", run, op, b, rb, share(rb, b), f, rf, share(rf, f), re, share(re, b)
    }
    for (r = 1; r <= nruns; r++) if (cnt[key("rwy-blk", "obj", runs[r])]) {
        run = runs[r]
        printf "%-9s %-5s %9s %9.1f %6s %9s %9.1f %6s %9.1f %6s\n", run, "obj", "", med("rwy-blk", "obj", run), "", "", med("rwy-file", "obj", run), "", med("rwy-enc", "obj", run), ""
    }
    printf "\nlowest-highest MiB/s\n"
    printf "%-9s %-5s %-15s %-15s %-15s %-15s %-15s\n", "run KiB", "op", "block", "rwy-blk", "file", "rwy-file", "rwy-enc"
    for (r = 1; r <= nruns; r++) for (o = 1; o <= 3; o++) {
        run = runs[r]; op = (o < 3) ? ops[o] : "obj"
        if (!cnt[key("rwy-blk", op, run)]) continue
        printf "%-9s %-5s", run, op
        for (d = 1; d <= 5; d++) {
            split("block rwy-blk file rwy-file rwy-enc", dv, " ")
            if (cnt[key(dv[d], op, run)]) printf " %7.1f-%-7.1f", lo(dv[d], op, run), hi(dv[d], op, run)
            else printf " %-15s", "-"
        }
        printf "\n"
    }
}'
