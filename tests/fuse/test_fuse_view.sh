#!/bin/sh
# tests/fuse/test_fuse_view.sh — the mounted BLS projection follows every change of the store, and a request does not
# cost more reads when there are more slots. Skips when FUSE cannot be
# mounted or python3 is missing.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
D=$(mktemp -d /var/tmp/rwy-fuse-view.XXXXXX)
M=$D/mnt
RUN=$D/run
export RWY_RUN_DIR=$RUN
CTL=$BUILD/rwyctl
E=$M/loader/entries
fail() { echo "FUSE-VIEW FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }
mounted=
cleanup() {
    [ -n "$mounted" ] && fusermount3 -u "$M" 2>/dev/null
    rm -rf "$D"
}
trap cleanup EXIT

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-VIEW: SKIP (no FUSE)"; exit 0; }
command -v python3 >/dev/null || { echo "TEST-FUSE-VIEW: SKIP (no python3)"; exit 0; }
mkdir -p "$M" "$RUN"

waitfor() {
    i=0
    while ! "$@"; do
        i=$((i + 1)); [ $i -gt 100 ] && return 1
        sleep 0.1
    done
}
sock() { ls "$RUN"/*.sock 2>/dev/null | head -n 1; }
havesock() { [ -n "$(sock)" ]; }
nodaemon() { ! pgrep -f "rwy-fuse.* $M\$" >/dev/null; }
start() {
    "$BUILD/rwy-fuse" "$@" "$M" 2>"$D/err" || fail "mount: $(cat "$D/err")"
    mounted=1
    waitfor havesock || fail "no freeze socket"
}
stop() {
    fusermount3 -u "$M" || fail "unmount"
    mounted=
    waitfor nodaemon || fail "daemon still running after unmount"
    rm -f "$RUN"/*.sock
}
entries() { ls "$E" | tr '\n' ' '; }
has() { ls "$E" | grep -qx -- "$1"; }
count() { ls "$E" | wc -l; }

# mkslots FILE N: a store with N slots, all INSTALLED.
head -c 4096 /dev/urandom >"$D/k"
head -c 4096 /dev/urandom >"$D/i"
mkslots() {
    "$BUILD/mkstore" --size 128 "$1" >/dev/null || fail mkstore
    "$CTL" --store "$1" init >/dev/null || fail init
    n=1
    while [ $n -le "$2" ]; do
        "$CTL" --store "$1" install --kernel "$D/k" --initrd "$D/i" --label "s$n" >/dev/null 2>&1 || fail "install $n"
        n=$((n + 1))
    done
}

mkslots "$D/base" 30
cp "$D/base" "$D/s"
start "$D/s"

# The mount lists every slot, and a second look at it gives the same
want "$(count)" 30 "entries of 30 slots"
want "$(find "$M" | wc -l)" "$(find "$M" | wc -l)" "a second find"
has runway-3+1.conf || fail "runway-3+1.conf not listed"

# A write through the mount
echo OK >"$M/EFI/RUNWAY/CONFIG3/STATE" || fail "write STATE"
has runway-3.conf && ! has runway-3+1.conf || fail "STATE written through the mount: $(entries)"

# rwyctl on the mounted store (freeze and thaw)
"$CTL" --store "$D/s" confirm CONFIG4 >/dev/null 2>&1 || fail "confirm"
has runway-4.conf && ! has runway-4+1.conf || fail "confirm: $(entries)"
"$CTL" --store "$D/s" fail CONFIG5 >/dev/null 2>&1 || fail "fail"
has runway-5+0.conf && ! has runway-5+1.conf || fail "fail: $(entries)"
"$CTL" --store "$D/s" install --kernel "$D/k" --initrd "$D/i" --label new >/dev/null 2>&1 || fail "install"
want "$(count)" 31 "entries after an install"
has runway-31+1.conf || fail "the new entry is not listed"
rm "$E/runway-31+1.conf" || fail "remove the entry"
want "$(count)" 30 "entries after a removal"
has runway-31+1.conf && fail "the removed entry is still listed"

# A kernel-install style entry: payload files, then the entry
mkdir -p "$M/kit/v1"
echo kernel-kit >"$M/kit/v1/linux"
printf 'title Kit\nversion v1\nlinux /kit/v1/linux\n' >"$E/kit.conf"
has kit+3.conf || fail "kit entry not listed: $(entries)"
want "$(ls "$M" | grep -cx kit)" 1 "kit in the root"
want "$(cat "$M/kit/v1/linux")" kernel-kit "payload of the kit entry"
rm "$E/kit+3.conf" || fail "remove the kit entry"
has kit+3.conf && fail "the kit entry is still listed"

stop

# The reads of a find per entry do not grow with the slots
reads_of_find() {
    pid=$(pgrep -f "rwy-fuse.* $M\$" | head -n 1)
    find "$M" >/dev/null
    r0=$(awk '/^syscr/ {print $2}' "/proc/$pid/io")
    n=$(find "$M" | wc -l)
    r1=$(awk '/^syscr/ {print $2}' "/proc/$pid/io")
    echo "$n $((r1 - r0))"
}
mkslots "$D/s10" 10
mkslots "$D/s30" 30
start "$D/s10"; set -- $(reads_of_find); n10=$1; r10=$2; stop
start "$D/s30"; set -- $(reads_of_find); n30=$1; r30=$2; stop
echo "find: $n10 entries $r10 reads (10 slots), $n30 entries $r30 reads (30 slots)"
[ $((r10 / n10)) -le 12 ] || fail "10 slots: $r10 reads for $n10 entries"
[ $((r30 / n30)) -le 12 ] || fail "30 slots: $r30 reads for $n30 entries"
# per entry at 30 slots less than twice the cost at 10 slots (a quadratic cost is about 3 times)
[ $((r30 * n10)) -lt $((2 * r10 * n30)) ] || fail "reads per entry grew from $r10/$n10 to $r30/$n30"

echo "FUSE-VIEW OK"
