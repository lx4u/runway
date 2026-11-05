#!/bin/sh
# tests/qemu/test_block.sh — The store on a raw XBOOTLDR partition, read through EFI_BLOCK_IO, under QEMU.
# SPDX-License-Identifier: Apache-2.0
#   1) a partition larger than its store boots; the ESP keeps its firmware FAT beside the Runway
#      volume; the payload confirms through BOOTED and the commit reaches the partition only.
#   2) the partition type decides: a store in a data partition is not found.
#   3) a partition smaller than the size in its superblock is refused.
#   4) with a store partition and a RUNWAY.DAT on one disk, the partition wins and the file is reported.
#   5) with no store partition the RUNWAY.DAT is used (file backend).
#   6) the booted store is the one on the disk of the loading ESP, whatever the scan order.
#   7) a store partition and a RUNWAY.DAT of one store make one mirror; a member that lags on either
#      backend is caught up and both end identical.
# The image is made here; the image scripts learn the partition layout in a later change.
set -e
export PATH="$PATH:/sbin:/usr/sbin"
BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
fail() { echo "FAIL: $1"; [ -n "$2" ] && tail -n 60 "$2"; exit 1; }
. tests/qemu/native_lib.sh

LOG=$BUILD/block.log
IMG=$BUILD/block.img
IMG2=$BUILD/block2.img
ST=$BUILD/block.store
ESP_MB=96
ESP_OFF=1048576
PART_MB=$((1 + ESP_MB))
PART_OFF=$((PART_MB * 1048576))
STORE_BYTES=$((64 * 1048576))
ESP_T=C12A7328-F81F-11D2-BA4B-00A0C93EC93B
XBL_T=BC13C2FF-59E6-4262-A352-B275FD6F7172
DATA_T=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
ESP_UUID=11111111-1111-4111-8111-111111111111
UUID_A=22222222-2222-4222-8222-222222222222
UUID_B=33333333-3333-4333-8333-333333333333

# mkimg OUT STORE_FILE|- ESP_FILE|- PART_MB [TYPE] [PARTUUID]
# A GPT disk: an ESP that boots RUNWAY.EFI (with ESP_FILE as RUNWAY.DAT when given) and a
# partition that holds STORE_FILE from byte 0 (a bigger STORE_FILE is cut to the partition).
mkimg() {
    out=$1; store=$2; espdat=$3; pmb=$4; ptype=${5:-$XBL_T}; puuid=${6:-$UUID_A}
    rm -f "$out"
    truncate -s $(( (PART_MB + pmb + 1) * 1048576 )) "$out"
    printf 'label: gpt\nstart=2048, size=%sMiB, type=%s, uuid=%s, name="ESP"\nstart=%s, size=%sMiB, type=%s, uuid=%s, name="STORE"\n' \
        "$ESP_MB" "$ESP_T" "$ESP_UUID" $((PART_MB * 2048)) "$pmb" "$ptype" "$puuid" | sfdisk "$out" >/dev/null
    mkfs.vfat --offset 2048 -F 32 -n ESP "$out" $((ESP_MB * 1024)) >/dev/null
    mmd -i "$out@@$ESP_OFF" ::EFI ::EFI/BOOT
    mcopy -i "$out@@$ESP_OFF" "$BUILD/RUNWAY.EFI" "::EFI/BOOT/$QEMU_BOOT_FILE"
    [ "$espdat" != - ] && mcopy -i "$out@@$ESP_OFF" "$espdat" ::RUNWAY.DAT
    [ "$store" != - ] && dd if="$store" of="$out" bs=1M seek=$PART_MB count=$pmb conv=notrunc status=none
    return 0
}
# win IMG -> the store spec of the partition (a window of the image)
win() { echo "$1@$PART_OFF+$STORE_BYTES"; }
# mkst FILE UUID ARGS: a 64 MiB store with slot 1 OK and slot 2 INSTALLED, both with ARGS
mkst() {
    $BUILD/mkstore --size 64 --arch "$ARCH" --uuid "$2" "$1" >/dev/null
    nat_seed "$1"
    nat_slot "$1" 1 "$3" OK ""
    nat_slot "$1" 2 "$3" INSTALLED ""
}
# psum IMG -> checksum of the 64 MiB store area of the partition
psum() { dd if="$1" bs=1M skip=$PART_MB count=64 status=none | cksum; }
gen() { $BUILD/fsck.runway "$1" | sed -n 's/.*committed:.*gen \([0-9]*\).*/\1/p' | head -1; }
clean() { $BUILD/fsck.runway "$1" | grep -q "check: OK (0 errors, 0 warnings)" || fail "store not clean: $1"; }
boot() { # drives... : first drive first
    qemu_vars_copy
    set -- $(for d in "$@"; do printf -- '-drive format=raw,file=%s ' "$d"; done)
    timeout "${BOOT_TMO:-150}" "$QEMU_BIN" -nographic $(qemu_args) "$@" -serial stdio -display none > $LOG 2>&1 || true
}

echo "--- 1: block backend, partition larger than the store ---"
mkst $ST $UUID_A confirm
mkimg $IMG $ST - 96
boot $IMG
nat_expect $LOG "RUNWAY: store: partition, block backend"
nat_refuse $LOG "file backend"
nat_expect $LOG "RUNWAY: booting CONFIG2 (rev 2, TESTING, 0 tries left)"
nat_expect $LOG "[NATIVE] started args=[confirm]"
nat_expect $LOG "[NATIVE] BOOTED/STATE=TESTING"
nat_expect $LOG "[NATIVE] confirm via BOOTED: 0x0"
nat_expect $LOG "[NATIVE] volume: ESP"
nat_expect $LOG "[NATIVE] volume: RUNWAY (own)"
nat_expect $LOG "[NATIVE] RunwayBootESP: $(echo $ESP_UUID | tr a-f A-F) (attr 0x6)"
nat_expect $LOG "[NATIVE] RunwayBootMembers: $(echo $UUID_A | tr a-f A-F) (attr 0x6)"
clean "$(win $IMG)"
[ "$(nat_cat "$(win $IMG)" $NAT_T/CONFIG2/STATE)" = OK ] || fail "CONFIG2 not confirmed in the partition"
dd if=$IMG of=$BUILD/block.tail bs=1M skip=$((PART_MB + 64)) count=32 status=none
cmp -n $((32 * 1048576)) $BUILD/block.tail /dev/zero || fail "space after the store was written"
mdir -i "$IMG@@$ESP_OFF" :: | grep -q RUNWAY.DAT && fail "the ESP got a RUNWAY.DAT"

echo "--- 2: the partition type decides ---"
mkimg $IMG $ST - 64 $DATA_T
boot $IMG
nat_expect $LOG "boot store cannot be mounted (not found or invalid), not booting"
nat_refuse $LOG "[NATIVE] started"

echo "--- 3: partition smaller than its store ---"
$BUILD/mkstore --size 80 --arch "$ARCH" --uuid $UUID_A $BUILD/block80.store >/dev/null
mkimg $IMG $BUILD/block80.store - 70
boot $IMG
nat_expect $LOG "is smaller than its store (83886080 bytes)"
nat_expect $LOG "boot store cannot be mounted (not found or invalid), not booting"
nat_refuse $LOG "[NATIVE] started"
rm -f $BUILD/block80.store

echo "--- 4: a store partition and a RUNWAY.DAT on one disk: the partition wins ---"
mkst $BUILD/block_file.store $UUID_B file
mkimg $IMG $ST $BUILD/block_file.store 64
boot $IMG
nat_expect $LOG "RUNWAY.DAT on handle"
nat_expect $LOG "ignored: the disk has a store partition"
nat_expect $LOG "RUNWAY: store: partition, block backend"
nat_expect $LOG "[NATIVE] started args=[confirm]"
nat_refuse $LOG "args=[file]"

echo "--- 5: no store partition: RUNWAY.DAT on the ESP ---"
mkimg $IMG - $BUILD/block_file.store 64 $DATA_T
boot $IMG
nat_expect $LOG "RUNWAY: store: RUNWAY.DAT, file backend"
nat_expect $LOG "[NATIVE] started args=[file]"
nat_refuse $LOG "block backend"

echo "--- 6: the booted store is on the disk of the loading ESP ---"
mkst $BUILD/block_other.store $UUID_B other
mkimg $IMG $ST - 64 $XBL_T $UUID_A
mkimg $IMG2 $BUILD/block_other.store - 64 $XBL_T 44444444-4444-4444-8444-444444444444
g2=$(psum $IMG2)
boot $IMG $IMG2
nat_expect $LOG "[NATIVE] started args=[confirm]"
nat_expect $LOG "[NATIVE] RunwayBootMembers: $(echo $UUID_A | tr a-f A-F) (attr 0x6)"
[ "$(psum $IMG2)" = "$g2" ] || fail "the other disk's store was written"
mkst $ST $UUID_A confirm
mkimg $IMG $ST - 64 $XBL_T $UUID_A
boot $IMG2 $IMG
nat_expect $LOG "[NATIVE] started args=[other]"
nat_expect $LOG "[NATIVE] RunwayBootMembers: 44444444-4444-4444-8444-444444444444 (attr 0x6)"
[ "$(psum $IMG)" = "$(cksum < $ST)" ] || fail "the other disk's store was written (second order)"

echo "--- 7: one store on a partition and on a file: a mixed mirror ---"
# both: members in step; part_old: the partition lags; file_old: the file lags
for case in both part_old file_old; do
    mkst $ST $UUID_A confirm
    cp -f $ST $BUILD/block_new.store
    nat_fs $BUILD/block_new.store put EXTRA "newer"
    case $case in
        both)     part=$BUILD/block_new.store; file=$BUILD/block_new.store ;;
        part_old) part=$ST;                    file=$BUILD/block_new.store ;;
        file_old) part=$BUILD/block_new.store; file=$ST ;;
    esac
    mkimg $IMG "$part" - 64 $XBL_T $UUID_A
    mkimg $IMG2 - "$file" 64 $DATA_T
    boot $IMG $IMG2
    nat_expect $LOG "RUNWAY: store: partition, block backend"
    nat_expect $LOG "mirror: active mirror array initialized with 2 device(s)"
    [ $case = both ] || nat_expect $LOG "mirror: catch-up synchronization complete (1 replica(s) caught up)"
    nat_expect $LOG "[NATIVE] started args=[confirm]"
    mcopy -o -n -i "$IMG2@@$ESP_OFF" ::RUNWAY.DAT $BUILD/block_file_out.store
    clean "$(win $IMG)"; clean $BUILD/block_file_out.store
    [ "$(gen "$(win $IMG)")" = "$(gen $BUILD/block_file_out.store)" ] || fail "$case: the members differ in generation"
    [ "$(nat_cat "$(win $IMG)" $NAT_T/CONFIG2/STATE)" = OK ] || fail "$case: partition not confirmed"
    [ "$(nat_cat $BUILD/block_file_out.store $NAT_T/CONFIG2/STATE)" = OK ] || fail "$case: file not confirmed"
    [ "$(nat_cat "$(win $IMG)" EXTRA)" = newer ] || fail "$case: partition lacks the newer file"
    [ "$(nat_cat $BUILD/block_file_out.store EXTRA)" = newer ] || fail "$case: file lacks the newer file"
    echo "  $case: ok"
done

rm -f $IMG $IMG2 $ST $BUILD/block*.store $BUILD/block.tail
echo ">>> SUCCESS: block backend boots, pairs, mirrors with a file and keeps the ESP"
