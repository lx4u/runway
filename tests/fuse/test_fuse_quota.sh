#!/bin/sh
# tests/fuse/test_fuse_quota.sh — A Type 1 entry written through rwy-fuse evicts under QUOTA, as rwyctl update does.
# SPDX-License-Identifier: Apache-2.0
# Skips when FUSE cannot be mounted.
set -u
BUILD=${BUILD:-build}
TDIR=$(mktemp -d /var/tmp/rwy-fuse-quota.XXXXXX)
ST=$TDIR/s
M=$TDIR/mnt
fail() { echo "FUSE-QUOTA FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }
ctl() { "$BUILD/rwyctl" --store "$ST" "$@"; }
slots() { ctl status | awk '$1 ~ /^CONFIG[0-9]+$/ {printf "%s ", $1}'; }
mounted=
cleanup() { [ -n "$mounted" ] && fusermount3 -u "$M" 2>/dev/null; rm -rf "$TDIR"; }
trap cleanup EXIT
# The QUOTA percentages below count on a store that is already about a quarter full.
head -c 17825792 /dev/zero >"$TDIR/ballast"

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-QUOTA: SKIP (no FUSE)"; exit 0; }
mkdir -p "$M"
"$BUILD/mkstore" --size 64 --add BALLAST="$TDIR/ballast" "$ST" >/dev/null || fail mkstore
ctl init || fail init
head -c 2097152 /dev/urandom >"$TDIR/k"
for i in 1 2 3 4 5 6 7; do
    head -c 2097152 /dev/urandom >"$TDIR/k"
    ctl install --kernel "$TDIR/k" >/dev/null || fail "install $i"
    ctl confirm CONFIG$i >/dev/null || fail "confirm $i"
done
ctl set QUOTA 60 >/dev/null || fail "set QUOTA"
OLD=$(slots)

# write_entry NAME: stage a payload and a Type 1 entry through the mount
write_entry() {
    mkdir -p "$M/tok/$1"
    cp "$TDIR/k" "$M/tok/$1/linux" 2>/dev/null || return 1
    printf 'title %s\nlinux /tok/%s/linux\n' "$1" "$1" >"$M/loader/entries/$1.conf" 2>/dev/null
}
mount_store() { "$BUILD/rwy-fuse" -s "$ST" "$M" || fail "mount"; mounted=1; sleep 1; }
# The daemon commits what is still staged when it exits: wait for it
unmount_store() {
    fusermount3 -u "$M" || fail "unmount"
    mounted=
    while pgrep -f "rwy-fuse.* $ST " >/dev/null; do sleep 0.2; done
}

# The quota holds: no slot goes
mount_store
write_entry first || fail "first entry"
unmount_store
want "$(slots)" "CONFIG1 CONFIG2 CONFIG3 CONFIG4 CONFIG5 CONFIG6 CONFIG7 CONFIG8 " "slots with room"

# Over the quota: the oldest OK slot goes, the entry is still accepted
ctl set QUOTA 52 >/dev/null
mount_store
write_entry second || fail "second entry"
unmount_store
ctl log show | grep -q "EVICT .*reason=quota" || fail "no EVICT record"
ctl status | grep -q "^LAST-EVICT=CONFIG[0-9]* quota$" || fail "status does not show the eviction"
case "$(slots)" in *CONFIG9*) ;; *) fail "the entry has no slot" ;; esac
case "$(slots)" in *CONFIG1\ *) fail "CONFIG1 was not evicted" ;; esac

# Nothing fits: the entry never becomes a slot and nothing else is deleted
ctl set QUOTA 30 >/dev/null
BEFORE=$(slots)
mount_store
write_entry third
unmount_store
want "$(slots)" "$BEFORE" "slots after a refusal"
"$BUILD/fsck.runway" "$ST" >"$TDIR/check" 2>&1 || { cat "$TDIR/check" >&2; fail "fsck.runway"; }
echo "FUSE-QUOTA: PASS"
