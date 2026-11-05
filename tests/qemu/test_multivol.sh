#!/bin/sh
# tests/qemu/test_multivol.sh — multi-volume integration test.
# SPDX-License-Identifier: Apache-2.0
#   A: ESP + 2 config + XBOOTLDR, distinct UUIDs. Config B pre-advanced on host
#      (gen 2). After boot: all 4 volumes mounted, each marker commit bumps only
#      its own volume (A 1->2, B 2->3, XBOOTLDR 1->2), fsck.runway clean.
#   B: config 1+2 share a UUID (mirror on secondary partitions); member 2
#      pre-advanced (gen 2). Boot must catch member 1 up and mirror both.
#   C: native boot selects from the boot group only.
#   D: WDT/ is read from the boot group only.
set -e
export PATH="$PATH:/sbin:/usr/sbin"
BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
IMG=$BUILD/multivol.img
fail() { echo "FAIL: $1"; [ -n "$2" ] && tail -n 60 "$2"; exit 1; }


boot() { # log
    qemu_vars_copy
    "$QEMU_BIN" -nographic $(qemu_args) \
        -drive format=raw,file=$IMG -serial stdio -display none > "$1" 2>&1 || true
}
# gen N -> generation of store N (0 is the boot store; via fsck.runway)
gen() {
    store_get $IMG "$1" $BUILD/mv_x.store
    $BUILD/fsck.runway $BUILD/mv_x.store > $BUILD/mv_x.out || { cat $BUILD/mv_x.out; fail "fsck.runway store $1"; }
    grep -q "check: OK (0 errors, 0 warnings)" $BUILD/mv_x.out || fail "store $1 not clean"
    sed -n 's/.*committed:.*gen \([0-9]*\).*/\1/p' $BUILD/mv_x.out | head -1
}

echo "--- A: independent volumes ---"
$BUILD/mkstore --size 64 --uuid 00000000-0000-4000-8000-000000000002 $BUILD/mv_ahead.store >/dev/null
$BUILD/host_mutate $BUILD/mv_ahead.store /HOST.TXT x >/dev/null
STORE_2=$BUILD/mv_ahead.store tests/qemu/mkdisk_multivol.sh $IMG 2 1 >/dev/null
boot $BUILD/multivol_a.log
grep -q "scan: 4 volume group(s) discovered" $BUILD/multivol_a.log || fail "expected 4 groups" $BUILD/multivol_a.log
grep -qF "$STORE_LOG" $BUILD/multivol_a.log || fail "missing: $STORE_LOG" $BUILD/multivol_a.log
grep -qF "$STORE_LOG_OTHER" $BUILD/multivol_a.log && fail "unexpected: $STORE_LOG_OTHER" $BUILD/multivol_a.log
# With STORE=block the ESP is a volume of its own, so the test application has one more to write to
OTHERS=3; [ "$STORE" = block ] && OTHERS=4
grep -q "multivol: wrote marker on $OTHERS of $OTHERS other volume(s): PASS" $BUILD/multivol_a.log || fail "marker writes" $BUILD/multivol_a.log
grep -q "chain-loaded application: OK" $BUILD/multivol_a.log || fail "chain app" $BUILD/multivol_a.log
G1=$(gen 1); G2=$(gen 2); G3=$(gen 3)
echo "generations: config1=$G1 config2=$G2 xbootldr=$G3"
[ "$G1" = 2 ] && [ "$G2" = 3 ] && [ "$G3" = 2 ] || fail "generations not independent (want 2/3/2)"
echo "A: PASS"

echo "--- B: mirror on secondary partitions ---"
$BUILD/mkstore --size 64 --uuid 00000000-0000-4000-8000-000000000001 $BUILD/mv_ahead.store >/dev/null
$BUILD/host_mutate $BUILD/mv_ahead.store /HOST.TXT x >/dev/null
MIRROR_AUX=1 STORE_2=$BUILD/mv_ahead.store tests/qemu/mkdisk_multivol.sh $IMG 2 0 >/dev/null
boot $BUILD/multivol_b.log
grep -q "scan: 2 volume group(s) discovered" $BUILD/multivol_b.log || fail "expected 2 groups" $BUILD/multivol_b.log
grep -q "member 0 is lagging" $BUILD/multivol_b.log || grep -q "mirror: replica caught up" $BUILD/multivol_b.log || fail "no catch-up" $BUILD/multivol_b.log
grep -q "chain-loaded application: OK" $BUILD/multivol_b.log || fail "chain app" $BUILD/multivol_b.log
M1=$(gen 1); M2=$(gen 2)
echo "mirror generations: member1=$M1 member2=$M2"
[ "$M1" = "$M2" ] && [ "$M1" -ge 3 ] || fail "mirror members diverged or not advanced"
echo "B: PASS"

echo "--- C: native boot on a multi-volume disk ---"
# Selection runs on the boot group only. A config volume that also holds EFI/RUNWAY is never
# selected from, and its INSTALLED slot stays untouched.
. tests/qemu/native_lib.sh
tests/qemu/mkdisk_multivol.sh $IMG 2 0 >/dev/null
seed_vol() { # N : EFI/RUNWAY with one INSTALLED slot on store N
    store_get $IMG "$1" $BUILD/mv_n.store
    nat_seed $BUILD/mv_n.store
    nat_slot $BUILD/mv_n.store 1 confirm INSTALLED
    store_put $IMG "$1" $BUILD/mv_n.store
}
seed_vol 0; seed_vol 1
G_AUX=$(gen 1)
boot $BUILD/multivol_c.log
nat_expect $BUILD/multivol_c.log "scan: 3 volume group(s) discovered"
nat_expect $BUILD/multivol_c.log "RUNWAY: booting CONFIG1 (rev 1, TESTING, 0 tries left)"
nat_expect $BUILD/multivol_c.log "[NATIVE] confirm via BOOTED: 0x0"
store_get $IMG 0 $BUILD/mv_n.store
[ "$(nat_cat $BUILD/mv_n.store $NAT_T/CONFIG1/STATE)" = OK ] || fail "boot group slot not confirmed"
[ "$(nat_cat $BUILD/mv_n.store $NAT_T/REASON)" = normal ] || fail "boot group REASON"
store_get $IMG 1 $BUILD/mv_n.store
[ "$(nat_cat $BUILD/mv_n.store $NAT_T/CONFIG1/STATE)" = INSTALLED ] || fail "config volume slot was selected"
nat_cat $BUILD/mv_n.store $NAT_T/REASON >/dev/null 2>&1 && fail "config volume got a REASON"
[ "$(gen 1)" = "$G_AUX" ] || fail "config volume was committed to"
echo "C: PASS"

echo "--- D: WDT/ is read from the boot group only ---"
seed_wdt() { # N STUB NAME : WDT/ application on store N
    store_get $IMG "$1" $BUILD/mv_n.store
    nat_fs $BUILD/mv_n.store put $NAT_T/BLOBS/wdt.efi @$BUILD/$2.EFI
    HOST_FS_SYSTEM=1 nat_fs $BUILD/mv_n.store mkdir $NAT_T/WDT
    HOST_FS_SYSTEM=1 nat_fs $BUILD/mv_n.store link $NAT_T/BLOBS/wdt.efi $NAT_T/WDT/$3
    nat_fs $BUILD/mv_n.store put $NAT_T/WATCHDOG "30
"
    store_put $IMG "$1" $BUILD/mv_n.store
}
tests/qemu/mkdisk_multivol.sh $IMG 2 0 >/dev/null
seed_vol 0; seed_vol 1
seed_wdt 0 WDT_STUB_OK A1.EFI
seed_wdt 1 WDT_STUB_FAIL B1.EFI
boot $BUILD/multivol_d.log
nat_expect $BUILD/multivol_d.log "RUNWAY: WATCHDOG 30s armed by A1.EFI"
nat_refuse $BUILD/multivol_d.log "WSTUB fail"
nat_expect $BUILD/multivol_d.log "RUNWAY: booting CONFIG1 (rev 1, TESTING, 0 tries left)"
echo "D: PASS"

rm -f $BUILD/mv_n.store $BUILD/mv_ahead.store $BUILD/mv_x.store $BUILD/mv_x.out
echo "TEST-MULTIVOL: PASS"
