#!/bin/sh
# tests/qemu/test_filebe.sh — The size guard of the file backend against a fake EFI_FILE_PROTOCOL, under QEMU/OVMF.
# SPDX-License-Identifier: Apache-2.0
# FILEBE_TEST.EFI checks that the guard stops writes once the file size changed.
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
LOG="$BUILD/filebe.log"
IMG="$BUILD/filebe.img"

rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
printf 'label: gpt\nstart=2048, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="ESP"\n' | sfdisk "$IMG" >/dev/null
mkfs.vfat --offset 2048 -F 32 "$IMG" >/dev/null
mmd -i "$IMG@@1048576" ::EFI ::EFI/BOOT
mcopy -i "$IMG@@1048576" "$BUILD/FILEBE_TEST.EFI" "::EFI/BOOT/$QEMU_BOOT_FILE"
qemu_vars_copy

timeout 120 "$QEMU_BIN" -nographic $(qemu_args) \
    -drive format=raw,file="$IMG" -serial stdio -display none > "$LOG" 2>&1 || true

grep -a '^FILETEST' "$LOG" | grep -av ' ok ' || true
grep -aq '^FILETEST DONE failures=0' "$LOG" || { echo "FAILED: see $LOG" >&2; exit 1; }
# Three runs of the fake trip the guard; each logs once however many writes follow.
[ "$(grep -ac 'RUNWAY: RUNWAY.DAT changed size from' "$LOG")" = 3 ] || { echo "FAILED: guard log lines: see $LOG" >&2; exit 1; }
echo ">>> SUCCESS: file backend size guard hold ($(grep -ac 'FILETEST: ok' "$LOG") checks)"
