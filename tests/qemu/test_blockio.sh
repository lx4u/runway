#!/bin/sh
# tests/qemu/test_blockio.sh — The block backend against a fake EFI_BLOCK_IO, under QEMU/OVMF.
# SPDX-License-Identifier: Apache-2.0
# BLOCKIO_TEST.EFI checks the size, block size, IoAlign, MediaId and flush rules in memory.
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
LOG="$BUILD/blockio.log"
IMG="$BUILD/blockio.img"

rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
printf 'label: gpt\nstart=2048, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="ESP"\n' | sfdisk "$IMG" >/dev/null
mkfs.vfat --offset 2048 -F 32 "$IMG" >/dev/null
mmd -i "$IMG@@1048576" ::EFI ::EFI/BOOT
mcopy -i "$IMG@@1048576" "$BUILD/BLOCKIO_TEST.EFI" "::EFI/BOOT/$QEMU_BOOT_FILE"
qemu_vars_copy

timeout 120 "$QEMU_BIN" -nographic $(qemu_args) \
    -drive format=raw,file="$IMG" -serial stdio -display none > "$LOG" 2>&1 || true

grep -a '^BLOCKTEST' "$LOG" | grep -av ' ok ' || true
grep -aq '^BLOCKTEST DONE failures=0' "$LOG" || { echo "FAILED: see $LOG" >&2; exit 1; }
echo ">>> SUCCESS: block backend rules hold ($(grep -ac 'BLOCKTEST: ok' "$LOG") checks)"
