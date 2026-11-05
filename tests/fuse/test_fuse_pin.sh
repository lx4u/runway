#!/bin/sh
# tests/fuse/test_fuse_pin.sh — A Type 1 entry written through rwy-fuse evicts old INSTALLED slots under SLOTS,
# and never a pinned one. Skips when FUSE cannot be mounted.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
TDIR=$(mktemp -d /var/tmp/rwy-fuse-pin.XXXXXX)
ST=$TDIR/s
M=$TDIR/mnt
fail() { echo "FUSE-PIN FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }
ctl() { "$BUILD/rwyctl" --store "$ST" "$@"; }
slots() { ctl status | awk '$1 ~ /^CONFIG[0-9]+$/ {printf "%s ", $1}'; }
mounted=
cleanup() { [ -n "$mounted" ] && fusermount3 -u "$M" 2>/dev/null; rm -rf "$TDIR"; }
trap cleanup EXIT

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-PIN: SKIP (no FUSE)"; exit 0; }
mkdir -p "$M"
"$BUILD/mkstore" --size 64 "$ST" >/dev/null || fail mkstore
ctl init || fail init
head -c 65536 /dev/urandom >"$TDIR/k"
for i in 1 2 3 4 5 6; do
    head -c 65536 /dev/urandom >"$TDIR/k"
    ctl install --kernel "$TDIR/k" >/dev/null || fail "install $i"
done
ctl confirm CONFIG1 >/dev/null || fail "confirm 1"
ctl confirm CONFIG2 >/dev/null || fail "confirm 2"
ctl set SLOTS 4 >/dev/null || fail "set SLOTS"
ctl set CONFIG3 PIN 1 >/dev/null || fail "pin"

write_entry() {
    mkdir -p "$M/tok/$1"
    cp "$TDIR/k" "$M/tok/$1/linux" 2>/dev/null || return 1
    printf 'title %s\nlinux /tok/%s/linux\n' "$1" "$1" >"$M/loader/entries/$1.conf" 2>/dev/null
}
mount_store() { "$BUILD/rwy-fuse" -s "$ST" "$M" || fail "mount"; mounted=1; sleep 1; }
unmount_store() {
    fusermount3 -u "$M" || fail "unmount"
    mounted=
    while pgrep -f "rwy-fuse.* $ST " >/dev/null; do sleep 0.2; done
}

# Only pinned and protected slots are left: the entry never becomes a slot and nothing is deleted
BEFORE=$(slots)
mount_store
write_entry first
unmount_store
want "$(slots)" "$BEFORE" "slots after a refusal"

# Unpinned: the oldest INSTALLED slots go, the OK slots and the next boot stay
ctl set CONFIG3 PIN 0 >/dev/null || fail "unpin"
mount_store
write_entry second || fail "second entry"
unmount_store
want "$(slots)" "CONFIG1 CONFIG2 CONFIG6 CONFIG7 " "slots after the eviction"
"$BUILD/fsck.runway" "$ST" >"$TDIR/check" 2>&1 || { cat "$TDIR/check" >&2; fail "fsck.runway"; }
echo "FUSE-PIN: PASS"
