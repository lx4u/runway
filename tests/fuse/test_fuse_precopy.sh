#!/bin/sh
# tests/fuse/test_fuse_precopy.sh — rwyctl install with --dedup on a mounted store
# executes via RPC, storing and deduplicating blobs in-process.
# SPDX-License-Identifier: Apache-2.0
# Skips when FUSE cannot be mounted.
set -u
BUILD=${BUILD:-build}
D=$(mktemp -d /var/tmp/rwy-fuse-dedup.XXXXXX)
M=$D/mnt
RUN=$D/run
export RWY_RUN_DIR=$RUN
CTL=$(cd "$BUILD" && pwd)/rwyctl
BLOBS=$M/EFI/RUNWAY/BLOBS
fail() { echo "FUSE-DEDUP FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }
mounted=
BGPIDS=
cleanup() {
    [ -n "$mounted" ] && fusermount3 -u "$M" 2>/dev/null
    [ -n "$BGPIDS" ] && kill -9 $BGPIDS 2>/dev/null
    rm -rf "$D"
}
trap cleanup EXIT
# The QUOTA percentages below count on a store that is already about a quarter full.
head -c 17825792 /dev/zero >"$D/ballast"

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-PRECOPY: SKIP (no FUSE)"; exit 0; }
mkdir -p "$M" "$RUN"

waitfor() {
    i=0
    while ! "$@"; do
        i=$((i + 1)); [ $i -gt 200 ] && return 1
        sleep 0.1
    done
}
havesock() { ls "$RUN"/*.sock >/dev/null 2>&1; }
nodaemon() { ! pgrep -f "rwy-fuse.* $M\$" >/dev/null; }
code() { "$@" >"$D/out" 2>"$D/err"; echo $?; }
blobs() { ls "$BLOBS" 2>/dev/null | grep -c '^[0-9a-f]\{64\}$'; }
start() {
    "$BUILD/rwy-fuse" "$@" "$M" 2>"$D/err" || fail "mount: $(cat "$D/err")"
    mounted=1
    waitfor havesock || fail "no RPC socket"
}
stop() {
    fusermount3 -u "$M" || fail "unmount"
    mounted=
    waitfor nodaemon || fail "daemon still running after unmount"
}

"$BUILD/mkstore" --size 64 --add BALLAST="$D/ballast" "$D/base" >/dev/null || fail mkstore
"$BUILD/rwyctl" --store "$D/base" init >/dev/null || fail init
head -c 3000000 /dev/urandom >"$D/k1"
head -c 1000000 /dev/urandom >"$D/i1"
head -c 2000000 /dev/urandom >"$D/k2"
cp "$D/base" "$D/s"
start "$D/s"

# A first install with --dedup stores the blobs and links them into the slot
want "$(code "$CTL" --store "$D/s" install --dedup --kernel "$D/k1" --initrd "$D/i1" --label one)" 0 "first install"
cmp "$D/k1" "$M/EFI/RUNWAY/CONFIG1/KERNEL" || fail "kernel of the first install"
cmp "$D/i1" "$M/EFI/RUNWAY/CONFIG1/INITRD" || fail "initrd of the first install"
want "$(blobs)" 2 "two blobs after the first install"

# The same kernel again: one blob, a second link
want "$(code "$CTL" --store "$D/s" install --dedup --kernel "$D/k1" --label two)" 0 "second install"
want "$(blobs)" 2 "the kernel is stored once"
want "$(stat -c %h "$M/EFI/RUNWAY/CONFIG2/KERNEL")" 3 "three names for the kernel (blob and two slots)"

# Two installs of the same file at once
"$CTL" --store "$D/s" remove CONFIG1 >/dev/null && "$CTL" --store "$D/s" remove CONFIG2 >/dev/null || fail "remove CONFIG1 CONFIG2"
for n in a b; do
    "$CTL" --store "$D/s" install --dedup --kernel "$D/k2" --label "par$n" >"$D/par$n.out" 2>&1 &
    BGPIDS="$BGPIDS $!"
done
wait
want "$(blobs)" 1 "one blob for two installs at once"
want "$(ls "$M/EFI/RUNWAY" | grep -c '^CONFIG')" 2 "both slots exist"

# A kernel that is a file of the mount (as /boot/vmlinuz-X is) is linked and deduped
used() { "$CTL" --store "$D/s" status | sed -n 's/^USED=[0-9]*% (\([0-9]*\)\/.*/\1/p'; }
"$CTL" --store "$D/s" remove CONFIG1 >/dev/null && "$CTL" --store "$D/s" remove CONFIG2 >/dev/null || fail "remove before the link tests"
cp "$D/k1" "$M/vmlinuz-1.0"
ln -s "$M/vmlinuz-1.0" "$D/vmlinuz"
want "$(code "$CTL" --store "$D/s" install --dedup --kernel "$D/vmlinuz" --label link1)" 0 "install from a file of the mount"
cmp "$D/k1" "$M/EFI/RUNWAY/CONFIG1/KERNEL" || fail "kernel linked from the mount"
base=$(used)
for n in 2 3 4 5 6 7 8 9 10; do
    want "$(code "$CTL" --store "$D/s" install --dedup --kernel "$D/vmlinuz" --label link$n)" 0 "install $n of the same kernel"
done
want "$(blobs)" 1 "ten installs, one blob"
[ $(( $(used) - base )) -lt 200 ] || fail "the repeated installs wrote data: $base -> $(used) blocks"

# Clean up slots
rm "$M/vmlinuz-1.0"
for n in 1 2 3 4 5 6 7 8 9 10; do "$CTL" --store "$D/s" remove CONFIG$n >/dev/null || fail "remove CONFIG$n"; done
want "$(blobs)" 0 "no blob is left"
head -c 2000000 /dev/urandom >"$D/k2" ; "$CTL" --store "$D/s" install --dedup --kernel "$D/k2" --label back >/dev/null || fail "install back"
want "$(blobs)" 1 "one blob again"
rm -f "$D/vmlinuz"

# A refused install leaves no blob
"$CTL" --store "$D/s" set SLOTS 1 >/dev/null || fail "set SLOTS"
head -c 1500000 /dev/urandom >"$D/k3"
want "$(code "$CTL" --store "$D/s" install --dedup --kernel "$D/k3" --label full)" 7 "install at the SLOTS cap"
want "$(blobs)" 1 "no blob for the refused install"

# Installing without --dedup on a mounted store creates independent physical files with link count 1
"$CTL" --store "$D/s" remove CONFIG1 >/dev/null || fail "remove before iso"
"$CTL" --store "$D/s" set SLOTS 2 >/dev/null || fail "set SLOTS 2"
want "$(code "$CTL" --store "$D/s" install --kernel "$D/k1" --label iso1)" 0 "install without dedup on mount"
want "$(code "$CTL" --store "$D/s" install --kernel "$D/k1" --label iso2)" 0 "second install without dedup on mount"
want "$(stat -c %h "$M/EFI/RUNWAY/CONFIG1/KERNEL")" 1 "first slot kernel link count is 1"
want "$(stat -c %h "$M/EFI/RUNWAY/CONFIG2/KERNEL")" 1 "second slot kernel link count is 1"
in1=$(stat -c %i "$M/EFI/RUNWAY/CONFIG1/KERNEL")
in2=$(stat -c %i "$M/EFI/RUNWAY/CONFIG2/KERNEL")
[ "$in1" != "$in2" ] || fail "slots share inode without --dedup: $in1 == $in2"
stop
"$CTL" --store "$D/s" remove CONFIG1 >/dev/null && "$CTL" --store "$D/s" remove CONFIG2 >/dev/null || fail "cleanup iso"

# A full store: the files are copied after the eviction
"$BUILD/mkstore" --size 64 --add BALLAST="$D/ballast" "$D/full" >/dev/null && "$BUILD/rwyctl" --store "$D/full" init >/dev/null || fail "mkstore full"
"$BUILD/rwyctl" --store "$D/full" set QUOTA 90 >/dev/null || fail "set QUOTA"
start "$D/full"
for n in 1 2 3 4 5 6 7 8; do
    head -c 4000000 /dev/urandom >"$D/fill"
    want "$(code "$CTL" --store "$D/full" update --dedup --kernel "$D/fill")" 0 "fill $n"
    "$CTL" --store "$D/full" confirm CONFIG$n >/dev/null || fail "confirm $n"
done
head -c 20000000 /dev/urandom >"$D/huge"
want "$(code "$CTL" -v --store "$D/full" update --dedup --kernel "$D/huge" --label huge)" 0 "update of a payload that does not fit"
cmp "$D/huge" "$M/EFI/RUNWAY/CONFIG9/KERNEL" || fail "kernel after the fallback"
ls "$M/EFI/RUNWAY" | grep -q '^CONFIG1$' && fail "the oldest slot was not evicted"
stop

"$BUILD/fsck.runway" "$D/full" >"$D/out" 2>&1 || { cat "$D/out" >&2; fail "check after the fallback"; }
"$BUILD/rwyctl" --store "$D/s" check >"$D/out" 2>&1 || { cat "$D/out" >&2; fail "check after the installs"; }
echo "FUSE-PRECOPY: PASS"
