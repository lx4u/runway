#!/bin/sh
# tests/fuse/test_fuse_guard.sh — OK written to STATE through the mount clears the boot loop counter
# of the slot that started, and only that one. Skips when FUSE cannot be mounted.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
D=$(mktemp -d /var/tmp/rwy-fuse-guard.XXXXXX)
ST=$D/f.store
M=$D/mnt
T=EFI/RUNWAY
V=$D/sys/firmware/efi/efivars
G=$V/RunwayBootGuard-b371a076-b7ef-4e6e-b98a-8d0d2c493c7b
fail() { echo "FUSE-GUARD FAIL: $1" >&2; exit 1; }
trap 'fusermount3 -u $M 2>/dev/null; rm -rf $D' EXIT

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-GUARD: SKIP (no FUSE)"; exit 0; }
mkdir -p $M $V
cp $BUILD/RUNWAY.DAT $ST
fs() { $BUILD/host_fs $ST "$@"; }
fs mkdir $T
for n in 1 2; do
    fs mkdir $T/CONFIG$n
    fs put $T/CONFIG$n/KERNEL x
    fs put $T/CONFIG$n/STATE "INSTALLED
"
done
fs booted CONFIG2
RWYCTL_ROOT=$D $BUILD/rwy-fuse -s $ST $M || fail "mount"
sleep 1
guard() { printf '\007\000\000\000counter' >$G; }

guard
echo OK >$M/$T/CONFIG1/STATE || fail "write CONFIG1"
[ -e $G ] || fail "a slot that did not start cleared the counter"
echo FAILED >$M/$T/CONFIG1/STATE || fail "write FAILED"
[ -e $G ] || fail "FAILED cleared the counter"
echo OK >$M/$T/CONFIG2/STATE || fail "write CONFIG2"
[ -e $G ] && fail "the slot that started kept the counter"
guard
printf '\007\000\000\000CONFIG1 recorded' >$V/RunwayBoot-b371a076-b7ef-4e6e-b98a-8d0d2c493c7b
echo OK >$M/$T/CONFIG2/STATE || fail "write CONFIG2 again"
[ -e $G ] || fail "a start by another slot cleared the counter"
printf '\007\000\000\000CONFIG2 recorded' >$V/RunwayBoot-b371a076-b7ef-4e6e-b98a-8d0d2c493c7b
echo OK >$M/$T/BOOTED/STATE || fail "write BOOTED"
[ -e $G ] && fail "BOOTED kept the counter"
echo "TEST-FUSE-GUARD: PASS"
