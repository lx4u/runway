#!/bin/sh
# tests/qemu/test_native.sh — native boot selection under QEMU/OVMF.
# SPDX-License-Identifier: Apache-2.0
#   1) confirm: newest slot INSTALLED -> TESTING, Runway hands over ARGS (LoadOptions), INITRD
#      (LoadFile2) and DTB (config table); the payload confirms through EFI/RUNWAY/BOOTED/STATE
#      (a symlink seen by the EFI adapter). A second boot of the OK slot commits nothing.
#   2) hang (watchdog stand-in): the payload never confirms, the host resets the VM over QMP;
#      TRIES=2 gives two boots, then FAILED and fallback to the older OK slot.
#   3) nothing bootable and no RECOVERY: RUNWAY.EFI returns to the firmware, which goes on with
#      the next boot entry (a second disk).
#   4) every slot FAILED with a sealed RECOVERY: it boots with REASON all_failed, and the OS
#      cannot confirm it through BOOTED.
#   5) RECOVER_NEXT (created by the USER actor) boots RECOVERY once, with REASON requested; the
#      request is consumed before the boot, so the next boot is the normal slot again.
#   6) the selection commit fails (store full): nothing boots, no try is consumed, RUNWAY.EFI
#      returns to the firmware which goes on with the next boot entry (a second disk).
#   7) LoadImage refuses the chosen slot's KERNEL (not a PE image): the slot is marked FAILED and
#      the next slot is booted in the same run; or RECOVERY when no slot is left; or the firmware
#      when nothing is left.
#   8) scale: 64 slots, the limit, only the oldest is bootable and all are walked; a store with
#      more (HOST_FS_PRIV fixture) only has its 64 highest revisions considered, with RECOVERY or
#      the firmware as the way out; fsck.runway reports the excess.
#   9) slot-name corners: CONFIG0 and CONFIG18446744073709551615 (26 characters) are selected,
#      confirmed through BOOTED and fall back by REVISION, whatever the names say.
#  10) slots installed by rwyctl (one commit each) boot like hand-made ones; RECOVERY is set
#      by `recovery set --unlock` and booted by `recover-next`.
#  11) the selection commit fails (the block layer fails every write): after retries RUNWAY.EFI boots
#      the best OK slot, else RECOVERY, without recording it and without consuming a try; the
#      volatile RunwayBoot variable says so; with nothing to boot it returns to the firmware.
#  12) BOOT-DELAY: RUNWAY.EFI waits that many seconds after the selection before the slot starts
#      (a slot and RECOVERY); no file means no wait (the value rules are in bootcfgtest).
#  13) a read error on the EFI/RUNWAY directory (the store mounts, its directory block does not read) is
#      not a store without EFI/RUNWAY: no legacy boot (no chain-load), the commit retries, then the
#      firmware boots the next disk.
#  14) RunwayBootLabel: the volatile variable is set for recorded, unrecorded, and RECOVERY boots
#      when a label is present; absent labels report none, and stale labels are deleted.
#  15) flight recorder: RUNWAY.EFI selection and condemn append to the flight ring; virtual file
#      EFI/RUNWAY/FLIGHT displays reverse chronological records; raw plaintext inspection.
#  16) Boot Loader Interface variables: LoaderInfo, LoaderDevicePartUUID, LoaderEntrySelected,
#      LoaderFeatures (boot counting, devicetree), LoaderEntries, LoaderEntryDefault, and LoaderBootCountPath set for trial/OK slots, Type 1 and Type 2, and chain-loaded loader.
set -e
export PATH="$PATH:/sbin:/usr/sbin"
BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
IMG=$BUILD/native.img
IMG2=$BUILD/native2.img
LOG=$BUILD/native.log
ST=$BUILD/nat.store
QMP=$BUILD/native.qmp
fail() { echo "FAIL: $1"; [ -n "$2" ] && tail -n 60 "$2"; exit 1; }
T=EFI/RUNWAY

KERNEL=$BUILD/NATIVE_PAYLOAD.EFI

extract() { store_get $IMG 0 $ST; }
inject() { store_put $IMG 0 $ST; }
fs() { $BUILD/host_fs $ST "$@"; }
cat_() { $BUILD/host_fs $ST cat "$1"; }
gen() { $BUILD/fsck.runway $ST | sed -n 's/.*committed:.*gen \([0-9]*\).*/\1/p' | head -1; }
clean() { $BUILD/fsck.runway $ST > $BUILD/nat.out || { cat $BUILD/nat.out; fail "fsck.runway"; }
          grep -q "check: OK (0 errors, 0 warnings)" $BUILD/nat.out || fail "store not clean"; }

# slot N ARGS STATE [TRIES] [INITRD] [DTB] [LABEL] [ENTRY]: installed through the finalize protocol (USER actor)
slot() {
    n=$1
    fs mkdir $T/CONFIG$n
    fs link $T/BLOBS/${KBLOB:-uki.efi} $T/CONFIG$n/KERNEL
    [ -n "$2" ] && fs put $T/CONFIG$n/ARGS "$2"
    [ -n "$4" ] && fs put $T/CONFIG$n/TRIES "$4"
    [ -n "$5" ] && fs put $T/CONFIG$n/INITRD "$5"
    [ -n "$6" ] && fs put $T/CONFIG$n/DTB "$6"
    [ -n "$7" ] && fs put $T/CONFIG$n/LABEL "$7"
    [ -n "$8" ] && fs put $T/CONFIG$n/ENTRY "$8
"
    fs put $T/CONFIG$n/STATE "INSTALLED
"
    [ "$3" != INSTALLED ] && fs put $T/CONFIG$n/STATE "$3
"
    return 0
}
newdisk() {
    tests/qemu/mkdisk_multivol.sh $IMG 0 0 >/dev/null
    extract
    fs mkdir $T
    fs mkdir $T/BLOBS
    fs put $T/BLOBS/uki.efi @$KERNEL
}

qemu_cmd() {
    qemu_vars_copy
    set -- $(qemu_args) \
        -drive format=raw,file=${WERR:+blkdebug:$BUILD/native.blkdebug:}$IMG -display none "$@"
    "$QEMU_BIN" "$@" &
    q=$!
    (sleep 180; kill $q 2>/dev/null) </dev/null >/dev/null 2>&1 &
    w=$!
    wait $q 2>/dev/null || true
    kill $w 2>/dev/null || true
}
# seconddisk: a disk with only the fake payload, for the firmware to fall back to
seconddisk() {
    rm -f $IMG2; dd if=/dev/zero of=$IMG2 bs=1M count=64 status=none
    printf 'label: gpt\nstart=2048, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="ESP"\n' | sfdisk $IMG2 >/dev/null
    sz=$(sfdisk -d $IMG2 | awk -v n=1 '/start=/ {c++; if (c==n) {gsub(/[^0-9]/,"",$6); print $6}}')
    mkfs.vfat --offset 2048 -F 16 $IMG2 $((sz / 2)) >/dev/null 2>&1
    mmd -i "$IMG2@@1048576" ::EFI ::EFI/BOOT
    mcopy -i "$IMG2@@1048576" $KERNEL "::EFI/BOOT/$QEMU_BOOT_FILE"
}
run() { # [extra qemu args]
    inject
    qemu_cmd -serial stdio "$@" > $LOG 2>&1 || true
    extract
}
expect() { grep -qF -- "$1" $LOG || fail "missing: $1" $LOG; }
refuse() { grep -qF -- "$1" $LOG && fail "unexpected: $1" $LOG; return 0; }
# Every RUNWAY.EFI and payload line must end in CR LF, or a screen staggers the lines.
crlf() { cr=$(printf '\r'); bare=$(grep -a -e 'RUNWAY: ' -e '\[NATIVE\]' $LOG | grep -av "$cr\$" | head -3)
         [ -z "$bare" ] || fail "line without CR: $bare" $LOG; }

echo "--- 1: confirm through BOOTED ---"
newdisk
slot 1 confirm OK
slot 2 confirm INSTALLED "" initrd-two dtb-two
run
expect "RUNWAY: booting CONFIG2 (rev 2, TESTING, 0 tries left)"
expect "[NATIVE] started args=[confirm]"
expect "[NATIVE] initrd: 10 bytes: initrd-two"
expect "[NATIVE] dtb: dtb-two"
expect "[NATIVE] BOOTED/STATE=TESTING"
expect "[NATIVE] BOOTED/INDEX=2"
expect "[NATIVE] write INDEX refused: yes"
expect "[NATIVE] CONFIG2 lists STATE=yes TRIES=yes INDEX=yes"
expect "[NATIVE] BOOTED lists STATE=yes TRIES=yes INDEX=yes"
expect "[NATIVE] RECOVER_NEXT create=0x0 visible=yes delete=0x0 gone=yes"
expect "[NATIVE] RunwayBoot: CONFIG2 recorded (attr 0x6)"
expect "[NATIVE] RunwayBootLabel: none"
esp=$(/sbin/sfdisk --part-uuid $IMG 1 | tr a-f A-F)
[ -n "$esp" ] || fail "no ESP partition UUID"
expect "[NATIVE] RunwayBootESP: $esp (attr 0x6)"
expect "[NATIVE] RunwayBootMembers: $(store_partuuid $IMG 0) (attr 0x6)"
expect "RUNWAY: $STORE_LOG"
refuse "$STORE_LOG_OTHER"
if [ "$STORE" = file ]; then expect "RUNWAY: store: size guard on RUNWAY.DAT"; else refuse "size guard"; fi
refuse "RUNWAY.DAT changed size"
expect "REASON=normal"
expect "[NATIVE] confirm via BOOTED: 0x0"
crlf
# Handle plumbing is debug output, not part of the boot log.
for m in "volume group(s) discovered" "filesystem protocol" "child DeviceHandle" "Runway handle:" "FAT ESP root" "--- chain-loading ---"; do
    refuse "$m"
done
# The console already reaches the serial port: each line must show once.
[ "$(grep -ao 'RUNWAY: booting CONFIG2' $LOG | wc -l)" = 1 ] || fail "booting line not printed once" $LOG
[ "$(cat_ $T/CONFIG2/STATE)" = OK ] || fail "CONFIG2 not confirmed"
[ "$(cat_ $T/CONFIG2/TRIES)" = 0 ] || fail "TRIES not consumed"
[ "$(cat_ $T/BOOTED/STATE)" = OK ] || fail "BOOTED does not reach CONFIG2"
[ "$(cat_ $T/REASON)" = normal ] || fail "REASON"
[ "$(cat_ $T/CONFIG1/STATE)" = OK ] || fail "CONFIG1 changed"
clean
G1=$(gen)
run
expect "RUNWAY: booting CONFIG2 (rev 2, OK)"
expect "[NATIVE] already confirmed"
extract; clean
[ "$(gen)" = "$G1" ] || fail "confirmed OK slot rebooting committed (gen $G1 -> $(gen))"
echo "1: PASS"

echo "--- 2: hang, QMP reset, tries, fallback ---"
newdisk
slot 1 confirm OK
slot 2 hang INSTALLED 2 "" "" "hang-label"
inject
rm -f $QMP
qemu_vars_copy
: > $LOG
"$QEMU_BIN" $(qemu_args) \
    -drive format=raw,file=$IMG -display none -serial file:$LOG -qmp unix:$QMP,server,nowait &
QPID=$!
seen=0; waited=0
while kill -0 $QPID 2>/dev/null && [ $waited -lt 240 ]; do
    n=$(( $(grep -c "\[NATIVE\] hanging" $LOG || true) ))
    if [ "$n" -gt "$seen" ]; then
        seen=$n
        python3 - $QMP <<'PY'
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f = s.makefile("rw")
f.readline()
for c in ("qmp_capabilities", "system_reset"):
    f.write(json.dumps({"execute": c}) + "\n"); f.flush(); f.readline()
PY
    fi
    sleep 1; waited=$((waited + 1))
done
kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true
[ "$seen" = 2 ] || fail "expected 2 hangs, saw $seen" $LOG
expect 'RUNWAY: booting CONFIG2 (rev 2, TESTING, 1 try left) "hang-label"'
expect 'RUNWAY: booting CONFIG2 (rev 2, TESTING, 0 tries left) "hang-label"'
expect 'RUNWAY: CONFIG2 FAILED, skipped (tries_used_up) "hang-label"'
expect "RUNWAY: booting CONFIG1 (rev 1, OK)"
expect "[NATIVE] started args=[confirm]"
expect "[NATIVE] RunwayBootLabel: hang-label (attr 0x6)"
expect "[NATIVE] RunwayBootLabel: none"
extract
[ "$(cat_ $T/CONFIG2/STATE)" = FAILED ] || fail "CONFIG2 not FAILED"
[ "$(cat_ $T/CONFIG2/CAUSE)" = tries_used_up ] || fail "CONFIG2 CAUSE"
[ "$(cat_ $T/BOOTED/STATE)" = OK ] && [ "$(cat_ $T/CONFIG1/STATE)" = OK ] || fail "BOOTED does not point to CONFIG1"
clean
echo "2: PASS"

echo "--- 3: nothing bootable, no RECOVERY: back to the firmware ---"
newdisk
slot 1 confirm FAILED
slot 2 confirm FAILED
seconddisk
inject
qemu_cmd -serial stdio -drive format=raw,file=$IMG2 > $LOG 2>&1 || true
expect "RUNWAY: no bootable config, no RECOVERY"
refuse "RUNWAY: booting"
expect "[NATIVE] started args=[]"
extract; clean
echo "3: PASS"

# recovery [LABEL]: RECOVERY made as SYSTEM with the unlock (sealed afterwards), ARGS confirm
recovery() {
    export HOST_FS_SYSTEM=1
    fs mkdir $T/RECOVERY
    fs link $T/BLOBS/uki.efi $T/RECOVERY/KERNEL
    fs put $T/RECOVERY/ARGS confirm
    [ -n "$1" ] && fs put $T/RECOVERY/LABEL "$1"
    unset HOST_FS_SYSTEM
}

echo "--- 4: all slots FAILED, sealed RECOVERY ---"
newdisk
slot 1 confirm FAILED
slot 2 confirm FAILED
recovery
run
expect "RUNWAY: no bootable config (all_failed), booting RECOVERY"
expect "[NATIVE] started args=[confirm]"
expect "[NATIVE] RunwayBootLabel: none"
expect "REASON=all_failed"
expect "[NATIVE] confirm via BOOTED: 0x"
refuse "[NATIVE] confirm via BOOTED: 0x0"
extract
[ "$(cat_ $T/REASON)" = all_failed ] || fail "REASON"
[ "$(cat_ $T/BOOTED/ARGS)" = confirm ] || fail "BOOTED does not reach RECOVERY"
[ "$(cat_ $T/CONFIG1/STATE)" = FAILED ] && [ "$(cat_ $T/CONFIG2/STATE)" = FAILED ] || fail "slot state changed"
clean
G4=$(gen)
run
expect "RUNWAY: no bootable config (all_failed), booting RECOVERY"
extract; clean
[ "$(gen)" = "$G4" ] || fail "recovery reboot committed"
echo "4: PASS"

echo "--- 5: RECOVER_NEXT ---"
newdisk
slot 1 confirm OK
recovery
fs put $T/RECOVER_NEXT ""
run
expect "RUNWAY: recovery requested, booting RECOVERY"
expect "[NATIVE] started args=[confirm]"
expect "REASON=requested"
extract
[ "$(cat_ $T/REASON)" = requested ] || fail "REASON"
$BUILD/host_fs $ST cat $T/RECOVER_NEXT >/dev/null 2>&1 && fail "RECOVER_NEXT not consumed"
clean
run
expect "RUNWAY: booting CONFIG1 (rev 1, OK)"
refuse "recovery requested"
extract
[ "$(cat_ $T/REASON)" = normal ] || fail "REASON after recovery"
clean
echo "5: PASS"

# fill: pad files, large to small, until the store refuses another 4 KiB (only the state-change
# reserve is left)
fill() {
    for sz in 1048576 65536 4096; do
        dd if=/dev/zero of=$BUILD/pad.bin bs=$sz count=1 status=none
        i=0
        while fs put pad$sz.$i @$BUILD/pad.bin 2>/dev/null; do i=$((i + 1)); done
    done
    rm -f $BUILD/pad.bin
}

echo "--- 6: store full: the reserve still lets the selection commit and the OS confirm ---"
newdisk
slot 1 confirm OK
slot 2 confirm INSTALLED
fill
fs put probe x 2>/dev/null && fail "store not full"
run
expect "RUNWAY: booting CONFIG2 (rev 2, TESTING, 0 tries left)"
refuse "cannot update boot state"
expect "[NATIVE] started args=[confirm]"
expect "[NATIVE] confirm via BOOTED: 0x0"
extract; clean
[ "$(cat_ $T/CONFIG2/STATE)" = OK ] || fail "CONFIG2 not confirmed on a full store"
echo "6: PASS"

echo "--- 7: LoadImage refuses the chosen slot ---"
bad() { KBLOB=bad.efi slot "$@"; }
newdisk
fs put $T/BLOBS/bad.efi "this is not a PE image"
slot 1 confirm OK
bad 2 confirm INSTALLED
run
expect "RUNWAY: booting CONFIG2 (rev 2, TESTING, 0 tries left)"
expect "RUNWAY: CONFIG2 cannot be started"
expect ", marked FAILED"
expect "RUNWAY: CONFIG2 FAILED, skipped (load_error)"
expect "RUNWAY: booting CONFIG1 (rev 1, OK)"
expect "[NATIVE] started args=[confirm]"
extract
[ "$(cat_ $T/CONFIG2/STATE)" = FAILED ] || fail "CONFIG2 not FAILED"
[ "$(cat_ $T/CONFIG2/CAUSE)" = load_error ] || fail "CONFIG2 CAUSE"
[ "$(cat_ $T/BOOTED/STATE)" = OK ] && [ "$(cat_ $T/CONFIG1/STATE)" = OK ] || fail "BOOTED does not point to CONFIG1"
[ "$(cat_ $T/REASON)" = normal ] || fail "REASON"
clean
run
refuse "cannot be started"
expect "RUNWAY: booting CONFIG1 (rev 1, OK)"
echo "7a: PASS (next slot)"

newdisk
fs put $T/BLOBS/bad.efi "this is not a PE image"
bad 1 confirm INSTALLED
recovery
run
expect "RUNWAY: CONFIG1 cannot be started"
expect "RUNWAY: no bootable config (all_failed), booting RECOVERY"
expect "REASON=all_failed"
expect "[NATIVE] started args=[confirm]"
extract
[ "$(cat_ $T/CONFIG1/STATE)" = FAILED ] || fail "CONFIG1 not FAILED"
[ "$(cat_ $T/REASON)" = all_failed ] || fail "REASON"
clean
echo "7b: PASS (RECOVERY)"

newdisk
fs put $T/BLOBS/bad.efi "this is not a PE image"
bad 1 confirm INSTALLED
seconddisk
inject
qemu_cmd -serial stdio -drive format=raw,file=$IMG2 > $LOG 2>&1 || true
expect "RUNWAY: CONFIG1 cannot be started"
expect "RUNWAY: no bootable config, no RECOVERY"
refuse "[NATIVE] started args=[confirm]"
expect "[NATIVE] started args=[]"
extract
[ "$(cat_ $T/CONFIG1/STATE)" = FAILED ] || fail "CONFIG1 not FAILED"
clean
echo "7c: PASS (firmware)"

BOOT_STORE_EXTRA="--recovery $KERNEL --args area-fallback" newdisk
esp=$(/sbin/sfdisk --part-uuid $IMG 1 | tr a-f A-F)
fs put $T/BLOBS/bad.efi "this is not a PE image"
bad 1 confirm INSTALLED
inject
qemu_cmd -serial stdio > $LOG 2>&1 || true
expect "RUNWAY: CONFIG1 cannot be started"
expect "RUNWAY: no bootable config, booting the recovery area"
expect "[NATIVE] started args=[area-fallback]"
expect "[NATIVE] LoaderInfo: RUNWAY.EFI 0.1 (attr 0x6)"
expect "[NATIVE] LoaderDevicePartUUID: $esp (attr 0x6)"
extract
[ "$(cat_ $T/CONFIG1/STATE)" = FAILED ] || fail "CONFIG1 not FAILED"
clean
echo "7d: PASS (recovery area fallback)"
echo "7: PASS"

# lean slot for the scale cases: N STATE [REVISION]; REVISION is written by hand under HOST_FS_PRIV
lean() {
    fs mkdir $T/CONFIG$1
    fs link $T/BLOBS/uki.efi $T/CONFIG$1/KERNEL
    if [ -n "$3" ]; then
        fs put $T/CONFIG$1/STATE "$2
"
        fs put $T/CONFIG$1/REVISION "$3
"
    else
        fs put $T/CONFIG$1/STATE "INSTALLED
"
        [ "$2" != INSTALLED ] && fs put $T/CONFIG$1/STATE "$2
"
    fi
    return 0
}

echo "--- 8: scale ---"
newdisk
lean 1 OK
n=2; while [ $n -le 64 ]; do lean $n FAILED; n=$((n + 1)); done
fs mkdir $T/CONFIG65 2>/dev/null && fail "65th slot accepted"
run
expect "RUNWAY: CONFIG64 FAILED, skipped"
expect "RUNWAY: CONFIG2 FAILED, skipped"
expect "RUNWAY: booting CONFIG1 (rev 1, OK)"
refuse "slots, only the"
extract; clean
echo "8a: PASS (64 slots, the oldest boots)"

for n in 65 66; do
    HOST_FS_PRIV=1 fs mkdir $T/CONFIG$n             # past the limit: only the bypass may create the directory
    fs link $T/BLOBS/uki.efi $T/CONFIG$n/KERNEL
    fs put $T/CONFIG$n/STATE "INSTALLED
"
    HOST_FS_SYSTEM=1 fs put $T/CONFIG$n/STATE "FAILED
"
done
seconddisk
inject
qemu_cmd -serial stdio -drive format=raw,file=$IMG2 > $LOG 2>&1 || true
expect "RUNWAY: 66 slots, only the 64 highest REVISION considered"
expect "RUNWAY: no bootable config, no RECOVERY"
refuse "RUNWAY: booting"
expect "[NATIVE] started args=[]"
extract
[ "$(cat_ $T/CONFIG1/STATE)" = OK ] || fail "CONFIG1 changed"
$BUILD/fsck.runway $ST > $BUILD/nat.out && fail "fsck.runway accepted 66 slots"
grep -q "66 slots, more than 64" $BUILD/nat.out || fail "fsck.runway did not report the excess" $BUILD/nat.out
echo "8b: PASS (66 slots, CONFIG1 outside the window: firmware)"

recovery
run
expect "RUNWAY: 66 slots, only the 64 highest REVISION considered"
expect "RUNWAY: no bootable config (all_failed), booting RECOVERY"
expect "[NATIVE] started args=[confirm]"
extract
[ "$(cat_ $T/CONFIG1/STATE)" = OK ] || fail "CONFIG1 changed"
echo "8c: PASS (66 slots, RECOVERY)"
echo "8: PASS"

echo "--- 9: slot-name corners ---"
BIG=18446744073709551615
newdisk
slot 0 confirm OK
slot $BIG confirm INSTALLED
run
expect "RUNWAY: booting CONFIG$BIG (rev 2, TESTING, 0 tries left)"
expect "[NATIVE] confirm via BOOTED: 0x0"
expect "[NATIVE] BOOTED/INDEX=$BIG"
expect "[NATIVE] write INDEX refused: yes"
[ "$(cat_ $T/CONFIG$BIG/STATE)" = OK ] || fail "CONFIG$BIG not confirmed"
[ "$(cat_ $T/BOOTED/INDEX)" = "$BIG" ] || fail "BOOTED/INDEX"
[ "$(cat_ $T/CONFIG0/INDEX)" = 0 ] || fail "CONFIG0/INDEX"
clean
fs put $T/CONFIG$BIG/STATE "FAILED
"
run
expect "RUNWAY: CONFIG$BIG FAILED, skipped"
expect "RUNWAY: booting CONFIG0 (rev 1, OK)"
[ "$(cat_ $T/BOOTED/INDEX)" = 0 ] || fail "BOOTED/INDEX of CONFIG0"
extract; clean
echo "9: PASS"

echo "--- 10: slots made by rwyctl ---"
ctl() { $BUILD/rwyctl --store $ST "$@"; }
newdisk
ctl install --kernel $KERNEL --args confirm > $BUILD/nat.out || fail "install CONFIG1"
[ "$(cut -d' ' -f1 $BUILD/nat.out)" = CONFIG1 ] || fail "install did not name CONFIG1"
ctl confirm CONFIG1
ctl install --kernel $KERNEL --args confirm --tries 1 --initrd $KERNEL > /dev/null || fail "install CONFIG2"
clean
run
expect "RUNWAY: booting CONFIG2 (rev 2, TESTING, 0 tries left)"
expect "[NATIVE] initrd: "
expect "[NATIVE] confirm via BOOTED: 0x0"
[ "$(cat_ $T/CONFIG2/STATE)" = OK ] || fail "CONFIG2 not confirmed"
clean
echo "10a: PASS (installed slot boots and confirms)"

ctl recovery set --unlock --kernel $KERNEL --args confirm || fail "recovery set"
ctl recover-next || fail "recover-next"
run
expect "RUNWAY: recovery requested, booting RECOVERY"
expect "[NATIVE] started args=[confirm]"
expect "REASON=requested"
extract; clean
ctl status | grep -q '^RECOVERY=valid' || fail "status RECOVERY"
echo "10b: PASS (recovery set, recover-next)"
echo "10: PASS"

echo "--- 11: selection commit fails (block write error): boot without recording (N2) ---"
# werr: run with every write of the boot disk failing; the second disk is the firmware's next entry
werr() {
    printf '[inject-error]\nevent = "write_aio"\nerrno = "5"\n' > $BUILD/native.blkdebug
    seconddisk
    inject
    WERR=1 qemu_cmd -serial stdio -drive format=raw,file=$IMG2 > $LOG 2>&1 || true
    unset WERR
    extract
}
newdisk
slot 1 one OK
slot 2 confirm INSTALLED
werr
expect "RUNWAY: cannot update boot state (I/O error), retrying"
expect "RUNWAY: cannot update boot state (I/O error), booting CONFIG1 without recording it"
refuse "RUNWAY: booting"
expect "[NATIVE] started args=[one]"
expect "[NATIVE] RunwayBoot: CONFIG1 unrecorded (attr 0x6)"
expect "[NATIVE] RunwayBootLabel: none"
[ "$(cat_ $T/CONFIG2/STATE)" = INSTALLED ] || fail "CONFIG2 changed by a failed commit"
[ "$(cat_ $T/CONFIG2/TRIES)" = 1 ] || fail "a try was consumed by a failed record write"
clean
echo "11a: PASS (top slot under test, the OK slot boots)"

newdisk
slot 1 one OK
slot 2 confirm INSTALLED 2
HOST_FS_SYSTEM=1 fs put $T/CONFIG2/STATE "TESTING
"
werr
expect "booting CONFIG1 without recording it"
expect "[NATIVE] started args=[one]"
[ "$(cat_ $T/CONFIG2/TRIES)" = 2 ] || fail "TRIES of the slot under test changed"
clean
echo "11b: PASS (TESTING slot is not run, TRIES untouched)"

newdisk
fs put $T/BLOBS/bad.efi "this is not a PE image"
slot 1 one OK
bad 2 confirm OK
slot 3 confirm INSTALLED
werr
expect "RUNWAY: CONFIG2 cannot be started"
expect "not usable as a fallback"
refuse "booting CONFIG2"
expect "booting CONFIG1 without recording it"
expect "[NATIVE] started args=[one]"
[ "$(cat_ $T/CONFIG2/STATE)" = OK ] || fail "an unrecorded boot marked CONFIG2"
clean
echo "11c: PASS (OK slot that cannot start, the next OK slot boots)"

newdisk
slot 1 confirm FAILED
slot 2 confirm INSTALLED
recovery
werr
expect "booting RECOVERY without recording it"
expect "[NATIVE] RunwayBoot: RECOVERY unrecorded"
expect "[NATIVE] RunwayBootLabel: none"
expect "[NATIVE] started args=[confirm]"
clean
echo "11d: PASS (no OK slot: RECOVERY)"

newdisk
slot 1 confirm INSTALLED
werr
expect "RUNWAY: cannot update boot state (I/O error), not booting"
refuse "without recording it"
expect "[NATIVE] started args=[]"
refuse "[NATIVE] started args=[confirm]"
[ "$(cat_ $T/CONFIG1/STATE)" = INSTALLED ] || fail "CONFIG1 changed by a failed commit"
clean
echo "11e: PASS (nothing bootable: the firmware boots the next disk)"
echo "11: PASS"

echo "--- 12: BOOT-DELAY ---"
# runs the VM and sets $secs to the time between the "RUNWAY: booting"/"booting RECOVERY" line and the payload
timed_run() {
    inject
    qemu_cmd -serial stdio 2>&1 | while IFS= read -r l; do printf '%s %s\n' "$(date +%s.%N)" "$l"; done > $LOG.ts || true
    sed 's/^[0-9.]* //' $LOG.ts > $LOG
    secs=$(awk '/RUNWAY: booting|booting RECOVERY/ {a=$1} /\[NATIVE\] started/ {b=$1} END {printf "%.1f", b-a}' $LOG.ts)
    extract
}
newdisk
slot 1 confirm OK
timed_run
refuse "BOOT-DELAY"
awk -v s=$secs 'BEGIN {exit !(s < 2)}' || fail "waited $secs s without BOOT-DELAY" $LOG
fs put $T/BOOT-DELAY "1
"
timed_run
expect "RUNWAY: BOOT-DELAY 1s"
awk -v s=$secs 'BEGIN {exit !(s >= 0.9)}' || fail "BOOT-DELAY 1 waited only $secs s" $LOG
clean
HOST_FS_SYSTEM=1 fs put $T/CONFIG1/STATE "FAILED
"
recovery
timed_run
expect "booting RECOVERY"
expect "RUNWAY: BOOT-DELAY 1s"
awk -v s=$secs 'BEGIN {exit !(s >= 0.9)}' || fail "BOOT-DELAY 1 waited only $secs s before RECOVERY" $LOG
echo "12: PASS"

echo "--- 13: read error on the EFI/RUNWAY directory: no legacy boot (N6) ---"
newdisk
slot 1 one OK
slot 2 confirm INSTALLED
seconddisk
inject
# fail the reads of every sector that holds the names of the EFI/RUNWAY directory
python3 -I - $IMG > $BUILD/native.blkdebug <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
secs, i = set(), d.find(b"CONFIG1\0")
while i >= 0:
    blk_off = (i // 4096) * 4096
    if not d[blk_off:blk_off+8].startswith(b"RUNWAYB1") and b"BLOBS\0" in d[blk_off:blk_off+4096]:
        secs.add(i // 512)
    i = d.find(b"CONFIG1\0", i + 1)
for n in sorted(secs):
    print('[inject-error]\nevent = "read_aio"\nerrno = "5"\nsector = "%d"\n' % n)
PY
[ -s $BUILD/native.blkdebug ] || fail "no directory sector found"
WERR=1 qemu_cmd -serial stdio -drive format=raw,file=$IMG2 > $LOG 2>&1 || true
unset WERR
extract
expect "RUNWAY: cannot update boot state (I/O error), retrying"
expect "RUNWAY: cannot update boot state (I/O error), not booting"
refuse "chain-load target"
refuse "RUNWAY: booting"
expect "[NATIVE] started args=[]"
refuse "[NATIVE] started args=[confirm]"
clean
echo "13: PASS (read error is a failure, not a legacy boot)"

echo "--- 14: RunwayBootLabel with slot and RECOVERY labels ---"
newdisk
slot 1 confirm OK "" "" "" "slot-one"
slot 2 confirm INSTALLED "" "" "" "slot-two"
run
expect 'RUNWAY: booting CONFIG2 (rev 2, TESTING, 0 tries left) "slot-two"'
expect "[NATIVE] started args=[confirm]"
expect "[NATIVE] RunwayBoot: CONFIG2 recorded (attr 0x6)"
expect "[NATIVE] RunwayBootLabel: slot-two (attr 0x6)"
clean
echo "14a: PASS (recorded boot with label)"

newdisk
slot 1 one OK "" "" "" "slot-one-unrec"
slot 2 confirm INSTALLED "" "" "" "slot-two-unrec"
werr
expect "RUNWAY: cannot update boot state (I/O error), booting CONFIG1 without recording it"
expect "[NATIVE] started args=[one]"
expect "[NATIVE] RunwayBoot: CONFIG1 unrecorded (attr 0x6)"
expect "[NATIVE] RunwayBootLabel: slot-one-unrec (attr 0x6)"
clean
echo "14b: PASS (unrecorded boot with label)"

newdisk
slot 1 confirm FAILED
slot 2 confirm FAILED
recovery "rec-sealed"
run
expect 'RUNWAY: no bootable config (all_failed), booting RECOVERY "rec-sealed"'
expect "[NATIVE] started args=[confirm]"
expect "[NATIVE] RunwayBoot: RECOVERY recorded (attr 0x6)"
expect "[NATIVE] RunwayBootLabel: rec-sealed (attr 0x6)"
clean
echo "14c: PASS (RECOVERY recorded boot with label)"

newdisk
slot 1 confirm FAILED
slot 2 confirm INSTALLED
recovery "rec-unrec"
werr
expect "booting RECOVERY without recording it"
expect "[NATIVE] started args=[confirm]"
expect "[NATIVE] RunwayBoot: RECOVERY unrecorded (attr 0x6)"
expect "[NATIVE] RunwayBootLabel: rec-unrec (attr 0x6)"
clean
echo "14d: PASS (RECOVERY unrecorded boot with label)"
echo "14: PASS"

echo "--- 15: flight recorder: boot selection and condemn in plaintext ring ---"
newdisk
fs put $T/BLOBS/bad.efi "this is not a PE image"
slot 1 confirm OK "" "" "" "slot-one"
bad 2 confirm INSTALLED "" "" "" "slot-two"
run
expect 'RUNWAY: booting CONFIG2 (rev 2, TESTING, 0 tries left) "slot-two"'
expect 'marked FAILED'
expect 'RUNWAY: booting CONFIG1 (rev 1, OK) "slot-one"'
expect "[NATIVE] started args=[confirm]"
clean
cat_ $T/FLIGHT > $BUILD/flight.log
grep -q "event=BOOT slot=CONFIG2" $BUILD/flight.log || fail "flight log missing BOOT CONFIG2" $BUILD/flight.log
grep -q "event=CONDEMN slot=CONFIG2" $BUILD/flight.log || fail "flight log missing CONDEMN CONFIG2" $BUILD/flight.log
grep -q "event=BOOT slot=CONFIG1" $BUILD/flight.log || fail "flight log missing BOOT CONFIG1" $BUILD/flight.log

# Plaintext raw inspection of the unmounted store directly
grep -q "CONFIG2" $ST || fail "raw store missing plaintext CONFIG2 in flight area"

boot1_line=$(grep -n "event=BOOT slot=CONFIG1" $BUILD/flight.log | head -1 | cut -d: -f1)
condemn_line=$(grep -n "event=CONDEMN slot=CONFIG2" $BUILD/flight.log | head -1 | cut -d: -f1)
boot2_line=$(grep -n "event=BOOT slot=CONFIG2" $BUILD/flight.log | head -1 | cut -d: -f1)
[ "$boot1_line" -lt "$condemn_line" ] || fail "BOOT CONFIG1 not before CONDEMN" $BUILD/flight.log
[ "$condemn_line" -lt "$boot2_line" ] || fail "CONDEMN not before BOOT CONFIG2" $BUILD/flight.log
echo "15: PASS (flight recorder records boot events)"

echo "--- 16: Boot Loader Interface variables ---"
newdisk
esp=$(/sbin/sfdisk --part-uuid $IMG 1 | tr a-f A-F)
slot 1 one INSTALLED 3 "" "" "" "6a98ec7245c74b37a1928f844a4aa95b-6.12.10.conf"
run
expect "RUNWAY: booting CONFIG1 (rev 1, TESTING, 2 tries left)"
expect "[NATIVE] started args=[one]"
expect "[NATIVE] LoaderInfo: RUNWAY.EFI 0.1 (attr 0x6)"
expect "[NATIVE] LoaderFeatures: 0x410"
expect "[NATIVE] LoaderDevicePartUUID: $esp (attr 0x6)"
expect "[NATIVE] LoaderEntrySelected: 6a98ec7245c74b37a1928f844a4aa95b-6.12.10.conf (attr 0x6)"
expect "[NATIVE] LoaderEntryDefault: 6a98ec7245c74b37a1928f844a4aa95b-6.12.10.conf (attr 0x6)"
expect "[NATIVE] LoaderEntries: 6a98ec7245c74b37a1928f844a4aa95b-6.12.10.conf (attr 0x6)"
expect "[NATIVE] LoaderBootCountPath: /loader/entries/6a98ec7245c74b37a1928f844a4aa95b-6.12.10+2.conf (attr 0x6)"
clean
echo "16a: PASS (trial slot with Type 1 name)"

newdisk
esp=$(/sbin/sfdisk --part-uuid $IMG 1 | tr a-f A-F)
slot 1 "" OK "" "" "" "" "linux-6.12.10.efi"
run
expect "RUNWAY: booting CONFIG1 (rev 1, OK)"
expect "[NATIVE] started args=[]"
expect "[NATIVE] LoaderInfo: RUNWAY.EFI 0.1 (attr 0x6)"
expect "[NATIVE] LoaderFeatures: 0x410"
expect "[NATIVE] LoaderDevicePartUUID: $esp (attr 0x6)"
expect "[NATIVE] LoaderEntrySelected: linux-6.12.10.efi (attr 0x6)"
expect "[NATIVE] LoaderEntryDefault: linux-6.12.10.efi (attr 0x6)"
expect "[NATIVE] LoaderEntries: linux-6.12.10.efi (attr 0x6)"
expect "[NATIVE] LoaderBootCountPath: none"
clean
echo "16b: PASS (OK slot with Type 2 name)"

newdisk
esp=$(/sbin/sfdisk --part-uuid $IMG 1 | tr a-f A-F)
slot 1 "" INSTALLED 2 "" "" "" "linux-6.12.10.efi"
run
expect "RUNWAY: booting CONFIG1 (rev 1, TESTING, 1 try left)"
expect "[NATIVE] started args=[]"
expect "[NATIVE] LoaderInfo: RUNWAY.EFI 0.1 (attr 0x6)"
expect "[NATIVE] LoaderFeatures: 0x410"
expect "[NATIVE] LoaderDevicePartUUID: $esp (attr 0x6)"
expect "[NATIVE] LoaderEntrySelected: linux-6.12.10.efi (attr 0x6)"
expect "[NATIVE] LoaderEntryDefault: linux-6.12.10.efi (attr 0x6)"
expect "[NATIVE] LoaderEntries: linux-6.12.10.efi (attr 0x6)"
expect "[NATIVE] LoaderBootCountPath: /EFI/Linux/linux-6.12.10+1.efi (attr 0x6)"
clean
echo "16c: PASS (trial slot with Type 2 name)"

BOOT_EFI=$KERNEL newdisk
esp=$(/sbin/sfdisk --part-uuid $IMG 1 | tr a-f A-F)
run
expect "chain-load target: EFI\\BOOT\\$QEMU_BOOT_FILE"
expect "[NATIVE] started args=[]"
expect "[NATIVE] LoaderInfo: RUNWAY.EFI 0.1 (attr 0x6)"
expect "[NATIVE] LoaderFeatures: 0x410"
expect "[NATIVE] LoaderDevicePartUUID: $esp (attr 0x6)"
expect "[NATIVE] LoaderEntrySelected: none"
expect "[NATIVE] LoaderEntryDefault: none"
expect "[NATIVE] LoaderEntries: none"
expect "[NATIVE] LoaderBootCountPath: none"
clean
echo "16d: PASS (chain-loaded loader)"
echo "16: PASS"

rm -f $IMG $IMG2 $ST $BUILD/nat.out $QMP $BUILD/native.blkdebug $LOG.ts $BUILD/flight.log
echo "TEST-NATIVE: PASS"
