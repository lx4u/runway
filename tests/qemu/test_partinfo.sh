#!/bin/sh
# tests/qemu/test_partinfo.sh — the type of a partition without the partition info protocol, under QEMU/OVMF.
# SPDX-License-Identifier: Apache-2.0
# PARTINFO_TEST.EFI removes the protocol from each partition handle and reads the type from the GPT instead. The
# second run damages the primary table first, so that the type comes from the backup one.
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
LOG="$BUILD/partinfo.log"
IMG="$BUILD/partinfo.img"

rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
sfdisk "$IMG" >/dev/null <<PARTS
label: gpt
start=2048, size=40MiB, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="ESP"
size=8MiB, type=BC13C2FF-59E6-4262-A352-B275FD6F7172, name="STORE"
size=8MiB, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="DATA"
PARTS
mkfs.vfat --offset 2048 -F 32 -s 1 "$IMG" 40960 >/dev/null
mmd -i "$IMG@@1048576" ::EFI ::EFI/BOOT
mcopy -i "$IMG@@1048576" "$BUILD/PARTINFO_TEST.EFI" "::EFI/BOOT/$QEMU_BOOT_FILE"

run() {
    qemu_vars_copy
    # shellcheck disable=SC2046
    timeout 120 "$QEMU_BIN" -nographic $(qemu_args) \
        -drive format=raw,file="$IMG" -serial stdio -display none > "$LOG" 2>&1 || true
    grep -a '^PARTINFOTEST' "$LOG" | grep -av ': ok ' || true
    grep -aq '^PARTINFOTEST DONE partitions=3 failures=0' "$LOG" || { echo "FAILED ($1): see $LOG" >&2; exit 1; }
}

run "primary table"
# A byte of the primary header changes after its CRC was made: the firmware falls back to the backup table too.
printf '\377' | dd of="$IMG" bs=1 seek=$((512 + 40)) conv=notrunc status=none
run "damaged primary table"
echo ">>> SUCCESS: the partition type is read from the GPT, from the backup table when the first is damaged"
