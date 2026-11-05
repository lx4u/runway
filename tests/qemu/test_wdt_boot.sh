#!/bin/sh
# tests/qemu/test_wdt_boot.sh — RUNWAY.EFI probes the applications of EFI/RUNWAY/WDT.
# SPDX-License-Identifier: Apache-2.0
#   1) sorted order, a failing and an unsupported application are skipped, the first success
#      wins and the rest are never loaded; the slot still boots
#   2) no application claims the device: the slot boots and the "not armed" line is printed
#   3) no WATCHDOG, or WATCHDOG 0: nothing is loaded
#   4) a slot WATCHDOG overrides the global one
#   5) i6300esb: a payload that hangs is reset by the watchdog, TRIES runs out and the older OK slot boots
#   6) remembered driver: the next boot tries only the recorded application; a recorded application
#      that is unsupported or deleted falls back to the full loop and the record follows;
#      WATCHDOG 0 leaves the record alone
#   7) RECOVERY boot (all slots FAILED): armed with the global WATCHDOG
#   8) Secure Boot: an unsigned application is skipped, a signed one arms
#   9) provisioning: WDT/ filled by `rwyctl wdt set --unlock` (a copy and a blob link), then armed
set -e
export PATH="$PATH:/sbin:/usr/sbin"
BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
IMG=$BUILD/wdtboot.img
LOG=$BUILD/wdtboot.log
ST=$BUILD/wdtboot.store
VARS=$BUILD/wdtboot_vars.fd
T=EFI/RUNWAY
fail() { echo "FAIL: $1"; [ -n "$2" ] && tail -n 40 "$2"; exit 1; }

part_off() { img_part_off $IMG 1; }
fs() { HOST_FS_SYSTEM=1 $BUILD/host_fs $ST "$@"; }
expect() { grep -qF -- "$1" $LOG || fail "missing: $1" $LOG; }
refuse() { grep -qF -- "$1" $LOG && fail "unexpected: $1" $LOG; return 0; }

# newdisk [WATCHDOG]: a confirmed slot (with its own WATCHDOG), and the stubs in BLOBS; UKI overrides the payload
newdisk() {
    tests/qemu/mkdisk_multivol.sh $IMG 0 0 >/dev/null
    store_get $IMG 0 $ST
    fs mkdir $T
    fs mkdir $T/BLOBS
    fs put $T/BLOBS/uki.efi @${UKI:-$BUILD/NATIVE_PAYLOAD.EFI}
    fs put $T/BLOBS/fail.efi @$BUILD/WDT_STUB_FAIL.EFI
    fs put $T/BLOBS/unsup.efi @$BUILD/WDT_STUB_UNSUP.EFI
    fs put $T/BLOBS/ok.efi @$BUILD/WDT_STUB_OK.EFI
    fs mkdir $T/CONFIG1
    fs link $T/BLOBS/uki.efi $T/CONFIG1/KERNEL
    fs put $T/CONFIG1/ARGS confirm
    if [ -n "$1" ]; then fs put $T/CONFIG1/WATCHDOG "$1
"; fi
    fs put $T/CONFIG1/STATE "INSTALLED
"
    fs put $T/CONFIG1/STATE "OK
"
}
# wdt NAME BLOB: an application in WDT/
wdt() { fs mkdir $T/WDT 2>/dev/null || true; fs link $T/BLOBS/$2.efi $T/WDT/$1; }
run() { # [extra qemu args]
    store_put $IMG 0 $ST
    qemu_vars_copy "$VARS"
    timeout 120 "$QEMU_BIN" $(qemu_args "$VARS") \
        -drive format=raw,file=$IMG -display none -serial stdio "$@" > $LOG 2>&1 || true
    store_get $IMG 0 $ST
}
hint() { fs cat $T/WDT_HINT 2>/dev/null || true; }

echo "--- 1: order, skip, stop at the first success ---"
newdisk
fs put $T/WATCHDOG "30
"
wdt A1.EFI fail
wdt A2.EFI unsup
wdt A3.EFI ok
wdt A4.EFI ok
run
expect "WSTUB fail started [arm 30]"
expect "WSTUB unsup started [arm 30]"
expect "WSTUB ok started [arm 30]"
expect "RUNWAY: WATCHDOG 30s not armed (A1.EFI: 0x8000000000000007), trying next"
expect "RUNWAY: WATCHDOG 30s armed by A3.EFI"
refuse "A2.EFI:"
[ "$(grep -c "WSTUB ok started" $LOG)" -eq 1 ] || fail "A4.EFI was loaded" $LOG
expect "[NATIVE] started args=[confirm]"
echo "1: PASS"

echo "--- 2: nothing claims the device ---"
newdisk
fs put $T/WATCHDOG "30
"
wdt A1.EFI unsup
run
expect "WSTUB unsup started [arm 30]"
expect "RUNWAY: WATCHDOG 30s not armed (no driver claimed the device)"
expect "[NATIVE] started args=[confirm]"
echo "2: PASS"

echo "--- 3: no WATCHDOG, WATCHDOG 0 ---"
newdisk
wdt A1.EFI ok
run
refuse "WSTUB"
refuse "RUNWAY: WATCHDOG"
expect "[NATIVE] started args=[confirm]"
fs put $T/WATCHDOG "0
"
run
refuse "WSTUB"
refuse "RUNWAY: WATCHDOG"
echo "3: PASS"

echo "--- 4: the slot WATCHDOG wins ---"
newdisk 7
fs put $T/WATCHDOG "30
"
wdt A1.EFI ok
run
expect "RUNWAY: WATCHDOG 7s armed by A1.EFI"
expect "WSTUB ok started [arm 7]"
echo "4: PASS"

echo "--- 5: hang, watchdog reset, tries, fallback ---"
newdisk
fs put $T/BLOBS/i6300esb.efi @$BUILD/I6300ESB.EFI
fs mkdir $T/CONFIG2
fs link $T/BLOBS/uki.efi $T/CONFIG2/KERNEL
fs put $T/CONFIG2/ARGS hang
fs put $T/CONFIG2/TRIES 2
fs put $T/CONFIG2/WATCHDOG "5
"
fs put $T/CONFIG2/STATE "INSTALLED
"
wdt I6300ESB.EFI i6300esb
run -device i6300esb -watchdog-action reset
expect "RUNWAY: WATCHDOG 5s armed by I6300ESB.EFI"
expect "RUNWAY: booting CONFIG2 (rev 2, TESTING, 1 try left)"
expect "RUNWAY: booting CONFIG2 (rev 2, TESTING, 0 tries left)"
expect "RUNWAY: CONFIG2 FAILED, skipped"
expect "RUNWAY: booting CONFIG1 (rev 1, OK)"
expect "[NATIVE] started args=[confirm]"
[ "$(grep -c "\[NATIVE\] hanging" $LOG)" -eq 2 ] || fail "expected 2 hangs" $LOG
echo "5: PASS"

echo "--- 6: remembered driver ---"
newdisk
fs put $T/WATCHDOG "30
"
wdt A1.EFI unsup
wdt A2.EFI ok
wdt A3.EFI ok
wdt A4.EFI ok
run
expect "RUNWAY: WATCHDOG 30s armed by A2.EFI"
[ "$(hint)" = "A2.EFI" ] || fail "hint not recorded after the first boot" $LOG
run
expect "RUNWAY: WATCHDOG 30s armed by A2.EFI"
refuse "WSTUB unsup"
[ "$(grep -c "WSTUB ok started" $LOG)" -eq 1 ] || fail "second boot loaded more than A2.EFI" $LOG
fs rm $T/WDT/A2.EFI
wdt A2.EFI unsup
run
expect "WSTUB unsup started [arm 30]"
expect "RUNWAY: WATCHDOG 30s armed by A3.EFI"
[ "$(hint)" = "A3.EFI" ] || fail "hint not updated after the recorded application turned unsupported" $LOG
fs rm $T/WDT/A3.EFI
run
expect "RUNWAY: WATCHDOG 30s armed by A4.EFI"
[ "$(hint)" = "A4.EFI" ] || fail "hint not updated after the recorded file was deleted" $LOG
fs rm $T/WDT/A4.EFI
run
expect "RUNWAY: WATCHDOG 30s not armed (no driver claimed the device)"
[ -z "$(hint)" ] || fail "hint not cleared when nothing armed" $LOG
wdt A3.EFI ok
run
[ "$(hint)" = "A3.EFI" ] || fail "hint not recorded again" $LOG
fs put $T/WATCHDOG "0
"
run
refuse "WSTUB"
[ "$(hint)" = "A3.EFI" ] || fail "WATCHDOG 0 changed the hint" $LOG
echo "6: PASS"

echo "--- 7: RECOVERY armed with the global WATCHDOG ---"
newdisk 7
fs put $T/CONFIG1/STATE "FAILED
"
fs put $T/WATCHDOG "30
"
wdt A1.EFI ok
fs mkdir $T/RECOVERY
fs link $T/BLOBS/uki.efi $T/RECOVERY/KERNEL
fs put $T/RECOVERY/ARGS confirm
run
expect "RUNWAY: no bootable config (all_failed), booting RECOVERY"
expect "RUNWAY: WATCHDOG 30s armed by A1.EFI"
refuse "WATCHDOG 7s"
expect "[NATIVE] started args=[confirm]"
echo "7: PASS"

echo "--- 8: Secure Boot, unsigned application skipped ---"
SNAKE=/usr/share/ovmf/PkKek-1-snakeoil
SKEY=$(mktemp /var/tmp/rwy-wdtboot-XXXXXX)
openssl pkey -in $SNAKE.key -passin pass:snakeoil -out $SKEY
sign() { sbsign --key $SKEY --cert $SNAKE.pem --output "$2" "$1" >/dev/null; }
sign $BUILD/RUNWAY.EFI $BUILD/wdtboot_rwy.efi
sign $BUILD/rwy-loader.efi $BUILD/wdtboot_loader.efi
sign $BUILD/NATIVE_PAYLOAD.EFI $BUILD/wdtboot_uki.efi
sign $BUILD/WDT_STUB_OK.EFI $BUILD/wdtboot_ok.efi
LOADER_EFI=$BUILD/wdtboot_loader.efi UKI=$BUILD/wdtboot_uki.efi newdisk
mcopy -o -n -i "$IMG@@$(part_off)" $BUILD/wdtboot_rwy.efi "::EFI/BOOT/$QEMU_BOOT_FILE"
fs put $T/BLOBS/signed.efi @$BUILD/wdtboot_ok.efi
fs put $T/WATCHDOG "30
"
wdt A1.EFI ok
wdt A2.EFI signed
SAVED_CODE=$QEMU_CODE
SAVED_VARS=$QEMU_VARS_TEMPLATE
if [ "$ARCH" = "aarch64" ]; then
    AAVMF_CODE=/usr/share/AAVMF/AAVMF_CODE.snakeoil.fd
    AAVMF_VARS_TEMPLATE=/usr/share/AAVMF/AAVMF_VARS.snakeoil.fd
else
    OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.snakeoil.fd
    OVMF_VARS_TEMPLATE=/usr/share/OVMF/OVMF_VARS_4M.snakeoil.fd
fi
run
expect "RUNWAY: WATCHDOG 30s not armed (A1.EFI: 0x800000000000000f), trying next"
expect "RUNWAY: WATCHDOG 30s armed by A2.EFI"
[ "$(grep -c "WSTUB ok started" $LOG)" -eq 1 ] || fail "the unsigned application ran" $LOG
echo "8: PASS"

echo "--- 9: WDT/ provisioned with rwyctl ---"
AAVMF_CODE=$SAVED_CODE
AAVMF_VARS_TEMPLATE=$SAVED_VARS
OVMF_CODE=$SAVED_CODE
OVMF_VARS_TEMPLATE=$SAVED_VARS
mcopy -o -n -i "$IMG@@$(part_off)" $BUILD/RUNWAY.EFI "::EFI/BOOT/$QEMU_BOOT_FILE"
newdisk
fs put $T/WATCHDOG "30
"
mkdir -p $BUILD/wdtboot_dir
cp $BUILD/WDT_STUB_UNSUP.EFI $BUILD/wdtboot_dir/A1.EFI
cp $BUILD/WDT_STUB_OK.EFI $BUILD/wdtboot_dir/ok.efi
$BUILD/rwyctl --store $ST wdt set --unlock $BUILD/wdtboot_dir/A1.EFI $BUILD/wdtboot_dir/ok.efi || fail "wdt set"
$BUILD/fsck.runway $ST >/dev/null || fail "fsck.runway after wdt set"
run
expect "RUNWAY: WATCHDOG 30s armed by ok.efi"
expect "WSTUB unsup started [arm 30]"
echo "9: PASS"

rm -rf $BUILD/wdtboot_dir
rm -f $SKEY $IMG $ST $VARS $LOG $BUILD/wdtboot_*.efi
