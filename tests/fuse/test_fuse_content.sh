#!/bin/sh
# tests/fuse/test_fuse_content.sh — content names (64 lower case hex digits) in BLOBS/ through the mount:
# USER cannot make, link, rename, unlink or write them; other names and slot writes work as before.
# SPDX-License-Identifier: Apache-2.0
# Skips when FUSE cannot be mounted.
set -u
BUILD=${BUILD:-build}
D=$(mktemp -d /var/tmp/rwy-fuse-content.XXXXXX)
ST=$D/f.store
M=$D/mnt
T=EFI/RUNWAY
H=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
fail() { echo "FUSE-CONTENT FAIL: $1" >&2; exit 1; }
trap 'fusermount3 -u $M 2>/dev/null; rm -rf $D' EXIT

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-CONTENT: SKIP (no FUSE)"; exit 0; }
mkdir -p $M
cp $BUILD/RUNWAY.DAT $ST
fs() { HOST_FS_SYSTEM=1 $BUILD/host_fs $ST "$@"; }
fs mkdir $T
fs mkdir $T/BLOBS
fs mkdir $T/CONFIG1
fs put $T/BLOBS/plain data
fs link $T/BLOBS/plain $T/BLOBS/$H || fail "SYSTEM link to a content name"
fs put $T/BLOBS/other more
RWYCTL_ROOT=$D $BUILD/rwy-fuse -s $ST $M || fail "mount"
sleep 1
B=$M/$T/BLOBS
refused() { "$@" 2>&1 | grep -q "not permitted"; }

echo x >$B/n1 || fail "plain name create"
refused cp /etc/hostname $B/fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210 || fail "cp to a content name"
refused ln $B/other $B/fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210 || fail "ln to a content name"
refused mv $B/n1 $B/fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210 || fail "mv to a content name"
refused mv $B/other $B/$H || fail "mv over a content name"
refused rm $B/$H || fail "rm of a content name"
refused mv $B/$H $B/moved || fail "mv away a content name"
refused dd if=/etc/hostname of=$B/$H conv=notrunc || fail "write to a content object"
ln $B/$H $B/alias || fail "ordinary second name"
refused dd if=/etc/hostname of=$B/alias conv=notrunc || fail "write through another name"
rm $B/alias || fail "rm of the ordinary name"
cp /etc/hostname $B/UPPER-$H || fail "other BLOBS name"
echo OK >$M/$T/CONFIG1/STATE 2>/dev/null
echo kernel >$M/$T/CONFIG1/KERNEL || fail "slot write"
refused cp /etc/hostname $M/$H || fail "bridge path that encodes to a content name"
echo k >$M/vmlinuz-x || fail "bridge payload with a normal name"
fs rm $T/BLOBS/$H || fail "SYSTEM unlink of a content name"
echo "TEST-FUSE-CONTENT: PASS"
