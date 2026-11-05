#!/bin/sh
# tests/fuse/test_fuse_index.sh — the virtual INDEX through a real FUSE mount
# (its oid has bit 63 set, the kernel sees it as st_ino). Skips when FUSE cannot be mounted.
# SPDX-License-Identifier: Apache-2.0
set -e
BUILD=${BUILD:-build}
D=$BUILD/fuse_t8
ST=$D/f.store
M=$D/mnt
T=EFI/RUNWAY
BIG=18446744073709551615
fail() { echo "FAIL: $1"; exit 1; }

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE: SKIP (no FUSE)"; exit 0; }
rm -rf $D; mkdir -p $M
cp $BUILD/RUNWAY.DAT $ST
fs() { $BUILD/host_fs $ST "$@"; }
fs mkdir $T
for n in 3 $BIG; do
    fs mkdir $T/CONFIG$n
    fs put $T/CONFIG$n/KERNEL x
    fs put $T/CONFIG$n/STATE "INSTALLED
"
done
fs booted CONFIG3
$BUILD/rwy-fuse -s -odefault_permissions $ST $M
trap 'fusermount3 -u $M 2>/dev/null || true; fusermount3 -u $D/mnt2 2>/dev/null || true; rm -rf $D' EXIT
sleep 1
C=$M/$T/CONFIG$BIG

[ "$(cat $C/INDEX)" = "$BIG" ] || fail "INDEX content"
[ "$(cat $M/$T/CONFIG3/INDEX)" = 3 ] || fail "INDEX of CONFIG3"
[ "$(stat -c '%a %s %u' $C/INDEX)" = "444 21 0" ] || fail "INDEX attributes: $(stat -c '%a %s %u' $C/INDEX)"
ino=$(stat -c %i $C/INDEX)
[ "$ino" != "$(stat -c %i $C/KERNEL)" ] && [ "$ino" != "$(stat -c %i $C/STATE)" ] || fail "INDEX inode not distinct"
[ "$(cat $M/$T/BOOTED/INDEX)" = 3 ] || fail "BOOTED/INDEX"
ls $C | grep -qx INDEX || fail "INDEX not listed"
ls -l $C/INDEX | grep -q '?' && fail "ls -l shows ? (xattr read failed)"
python3 -I - $C/INDEX <<'PY' || fail "xattr reads"
import os, sys
p = sys.argv[1]
assert os.listxattr(p) == []
try: os.getxattr(p, "user.x"); raise SystemExit(1)
except OSError as e: assert e.errno == 61, e
PY
(echo 9 > $C/INDEX) 2>/dev/null && fail "write to INDEX"
rm $C/INDEX 2>/dev/null && fail "unlink INDEX"
mv $C/INDEX $C/X 2>/dev/null && fail "rename INDEX"
[ "$(cat $C/INDEX)" = "$BIG" ] || fail "INDEX changed"

# the mount holds the store lock; a writer is refused, a reader is not
rc=0; $BUILD/rwyctl --store $ST confirm CONFIG3 >/dev/null 2>&1 || rc=$?
[ $rc -eq 5 ] || fail "rwyctl write under a mount: exit $rc, want 5"
$BUILD/rwyctl --store $ST status >/dev/null 2>&1 || fail "rwyctl status under a mount"
mkdir -p $D/mnt2
rc=0; timeout 5 $BUILD/rwy-fuse -f $ST $D/mnt2 >/dev/null 2>&1 </dev/null || rc=$?
[ $rc -ne 0 ] && [ $rc -ne 124 ] || fail "second mount of a locked store: exit $rc"
# a mirror mount locks every member; a member that cannot be locked counts as missing
fusermount3 -u $M
M2=$D/mnt2; M1=$D/m1.store; M2S=$D/m2.store
cp $ST $M1; cp $ST $M2S
$BUILD/rwy-fuse -s $M1 $M2S $M2 2>$D/err || fail "mirror mount: $(cat $D/err)"
sleep 1
for f in $M1 $M2S; do
    rc=0; $BUILD/rwyctl --store $f confirm CONFIG3 >/dev/null 2>&1 || rc=$?
    [ $rc -eq 5 ] || fail "rwyctl write on $f under a mirror mount: exit $rc, want 5"
done
# the virtual files of a slot from a shell
V=$M2/$T/CONFIG7
rm $M2/$T/CONFIG3/INDEX || fail "unlink INDEX is a no-op"
[ "$(cat $M2/$T/CONFIG3/INDEX)" = 3 ] || fail "INDEX lost"
mkdir $V && echo k > $V/KERNEL && echo INSTALLED > $V/STATE || fail "install CONFIG7"
[ -n "$(cat $V/REVISION)" ] || fail "CONFIG7 not finalized"
echo FAILED > $V/STATE || fail "write STATE through FUSE"
[ "$(cat $V/STATE)" = FAILED ] || fail "STATE content"
ls $V | grep -qx TRIES || fail "TRIES not listed"
ls -l $V | grep -q '?' && fail "ls -l of a slot shows ?"
ls -l $M2/$T/BOOTED | grep -q -- '->' || fail "BOOTED is not a symlink"
ls $M2/$T/BOOTED/ | grep -qx STATE || fail "ls of BOOTED/ shows no STATE"
touch $M2/$T/RECOVER_NEXT || fail "touch RECOVER_NEXT"
chmod 600 $M2/$T/RECOVER_NEXT || fail "chmod RECOVER_NEXT"
[ -e $M2/$T/RECOVER_NEXT ] || fail "RECOVER_NEXT not created"
rm $M2/$T/RECOVER_NEXT || fail "rm RECOVER_NEXT"
[ ! -e $M2/$T/RECOVER_NEXT ] || fail "RECOVER_NEXT not removed"
rm $V/REVISION $V/STATE 2>/dev/null || fail "rm of REVISION and STATE is a no-op"
[ -n "$(cat $V/REVISION)" ] && [ "$(cat $V/STATE)" = FAILED ] || fail "REVISION or STATE lost"
rm -r $V || fail "rm -r of a slot"
[ ! -e $V ] || fail "slot not removed"
W=$M2/$T/CONFIG8
mkdir $W && echo k > $W/KERNEL && : > $W/STATE || fail "prepare CONFIG8"
exec 3<$W/STATE
echo INSTALLED > $W/STATE || fail "finalize CONFIG8"
exec 3<&- || fail "closing a handle on STATE across finalize"
[ -n "$(cat $W/REVISION)" ] || fail "CONFIG8 not finalized"
rm -r $W || fail "rm -r of CONFIG8"

mkdir $M2/$T/BLOBS && echo hello > $M2/$T/BLOBS/h || fail "write through the mirror mount"
fusermount3 -u $M2
for f in $M1 $M2S; do [ "$($BUILD/host_fs $f cat $T/BLOBS/h)" = hello ] || fail "mirror member $f missed the write"; done
if command -v flock >/dev/null; then
    flock -n $M2S sleep 5 &   # holds the lock for a few seconds
    sleep 1
    rc=0; $BUILD/rwy-fuse -s $M1 $M2S $M2 2>$D/err || rc=$?
    [ $rc -ne 0 ] || fail "mount with a busy member must abort"
    grep -q "busy" $D/err || fail "no busy message: $(cat $D/err)"
    ! mountpoint -q $M2 || fail "a busy member left a mount"
    rc=0; timeout 5 $BUILD/rwy-fuse -f $M2S $M2 >/dev/null 2>&1 </dev/null || rc=$?
    [ $rc -ne 0 ] && [ $rc -ne 124 ] || fail "mount with no lockable member: exit $rc"
    # a member that does not exist is missing: the mount runs degraded
    $BUILD/rwy-fuse -s $M1 $D/nothing.store $M2 2>$D/err || fail "mount with a missing member: $(cat $D/err)"
    grep -q "degraded" $D/err || fail "no degraded warning: $(cat $D/err)"
    fusermount3 -u $M2
    wait
fi
# a mirror member that lags is counted in the state record, a healthy one is not
L0=$D/l0.store; L1=$D/l1.store; M3=$D/mnt3
mkdir -p $M3; cp $BUILD/RUNWAY.DAT $L0; cp $L0 $L1
$BUILD/rwy-fuse -s $L0 $L1 $M3 2>$D/err || fail "mount of a mirror in step: $(cat $D/err)"
sleep 1
echo x > $M3/A || fail "write through the mirror"
fusermount3 -u $M3
UL1=$($BUILD/fsck.runway $L1 | sed -n 's/^slot [AB]: member uuid //p' | sort -u)
[ -n "$UL1" ] || fail "the second member was not named"
[ "$($BUILD/host_fs $L0 health $UL1)" = none ] || fail "a healthy mirror recorded an event"
$BUILD/host_fs $L0 mkdir AHEAD
$BUILD/rwy-fuse -s $L0 $L1 $M3 2>$D/err || fail "mount with a lagging member: $(cat $D/err)"
sleep 1
fusermount3 -u $M3
[ "$($BUILD/host_fs $L0 health $UL1)" = "lagging=1 sync_failed=0 write_failed=0 missing=0" ] || fail "lagging member not counted: $($BUILD/host_fs $L0 health $UL1)"
# a roster member that is not mounted is counted as missing
$BUILD/rwy-fuse -s $L0 $M3 2>$D/err || fail "mount with a missing member: $(cat $D/err)"
sleep 1
fusermount3 -u $M3
[ "$($BUILD/host_fs $L0 health $UL1)" = "lagging=1 sync_failed=0 write_failed=0 missing=1" ] || fail "missing member not counted: $($BUILD/host_fs $L0 health $UL1)"
echo "TEST-FUSE: PASS"
