#!/bin/sh
# tests/qemu/test_loopguard.sh — the boot loop guard demotes an OK slot that resets in a quick loop.
# SPDX-License-Identifier: Apache-2.0
#   1) LOOP-GUARD on, LOOP-LIMIT 3: the newest OK slot resets at once; the fourth entry demotes it
#      (FAILED, CAUSE boot_loop) and the older OK slot boots with REASON boot_loop
#   2) the guard is off: the same slot is entered again and again and never demoted
#   3) the only OK slot, no RECOVERY: it is never demoted and keeps booting
set -e
export PATH="$PATH:/sbin:/usr/sbin"
BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
IMG=$BUILD/loopguard.img
LOG=$BUILD/loopguard.log
ST=$BUILD/loopguard.store
VARS=$BUILD/loopguard_vars.fd
T=EFI/RUNWAY
fail() { echo "FAIL: $1"; [ -n "$2" ] && tail -n 40 "$2"; exit 1; }

fs() { HOST_FS_SYSTEM=1 $BUILD/host_fs $ST "$@"; }
expect() { grep -qF -- "$1" $LOG || fail "missing: $1" $LOG; }
refuse() { grep -qF -- "$1" $LOG && fail "unexpected: $1" $LOG; return 0; }
entries() { grep -cF -- "booting $1 (" $LOG || true; }

# slot N ARGS: a confirmed slot whose payload runs with ARGS
slot() {
    fs mkdir $T/CONFIG$1
    fs link $T/BLOBS/uki.efi $T/CONFIG$1/KERNEL
    fs put $T/CONFIG$1/ARGS "$2"
    fs put $T/CONFIG$1/STATE "INSTALLED
"
    fs put $T/CONFIG$1/STATE "OK
"
}
newdisk() {
    tests/qemu/mkdisk_multivol.sh $IMG 0 0 >/dev/null
    store_get $IMG 0 $ST
    fs mkdir $T
    fs mkdir $T/BLOBS
    fs put $T/BLOBS/uki.efi @$BUILD/NATIVE_PAYLOAD.EFI
}
guard() { # LIMIT
    fs put $T/LOOP-GUARD "on
"
    fs put $T/LOOP-LIMIT "$1
"
}
run() { # seconds
    store_put $IMG 0 $ST
    qemu_vars_copy "$VARS"
    timeout "$1" "$QEMU_BIN" $(qemu_args "$VARS") \
        -drive format=raw,file=$IMG -display none -serial stdio > $LOG 2>&1 || true
    store_get $IMG 0 $ST
}

echo "--- 1: loop, demotion, fallback ---"
newdisk
slot 1 ok
slot 2 reset
guard 3
run 120
[ "$(entries CONFIG2)" -eq 3 ] || fail "expected 3 entries into CONFIG2" $LOG
expect "RUNWAY: CONFIG2 FAILED, boot loop (more than 3 entries in 600 s), skipped"
expect "RUNWAY: booting CONFIG1 (rev 1, OK)"
expect "[NATIVE] started args=[ok]"
[ "$(fs cat $T/CONFIG2/STATE)" = "FAILED" ] || fail "CONFIG2 is not FAILED"
[ "$(fs cat $T/CONFIG2/CAUSE)" = "boot_loop" ] || fail "CAUSE is not boot_loop"
[ "$(fs cat $T/REASON)" = "boot_loop" ] || fail "REASON is not boot_loop"
$BUILD/fsck.runway $ST | grep -q "check: OK (0 errors, 0 warnings)" || fail "store not clean"
echo "1: PASS"

echo "--- 2: guard off ---"
newdisk
slot 1 ok
slot 2 reset
run 40
[ "$(entries CONFIG2)" -gt 3 ] || fail "CONFIG2 was not entered more than 3 times" $LOG
refuse "boot loop"
[ "$(fs cat $T/CONFIG2/STATE)" = "OK" ] || fail "CONFIG2 is not OK"
echo "2: PASS"

echo "--- 3: nothing to fall back on ---"
newdisk
slot 1 reset
guard 3
run 60
[ "$(entries CONFIG1)" -gt 3 ] || fail "CONFIG1 was not entered more than 3 times" $LOG
expect "RUNWAY: CONFIG1 reached the boot loop limit (more than 3 entries in 600 s), no other slot, booting it"
[ "$(fs cat $T/CONFIG1/STATE)" = "OK" ] || fail "CONFIG1 is not OK"
echo "3: PASS"

rm -f $IMG $ST $VARS
echo "TEST-LOOPGUARD: PASS"
