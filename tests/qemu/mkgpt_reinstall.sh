#!/bin/sh
# tests/qemu/mkgpt_reinstall.sh — Generate a GPT disk image with a FAT32 ESP for Phase 0 reinstall probe.
# SPDX-License-Identifier: Apache-2.0
# Usage: tests/qemu/mkgpt_reinstall.sh [out.img] [probe.efi] [child.efi]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
OUT="${1:-$BUILD/reinstall_gpt.img}"
PROBE="${2:-$BUILD/REINSTALL_PROBE.EFI}"
CHILD="${3:-$BUILD/REINSTALL_CHILD.EFI}"

IMG_MB=64
PART_OFFSET_SECTORS=2048
PART_OFFSET_BYTES=$((PART_OFFSET_SECTORS * 512))

command -v sfdisk >/dev/null 2>&1 || { echo "missing sfdisk" >&2; exit 1; }
command -v mkfs.vfat >/dev/null 2>&1 || { echo "missing dosfstools" >&2; exit 1; }
command -v mcopy >/dev/null 2>&1 || { echo "missing mtools" >&2; exit 1; }

rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count="$IMG_MB" status=none

# Partition with GPT: Partition 1 as EFI System Partition
sfdisk_script="label: gpt
start=${PART_OFFSET_SECTORS}, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, uuid=E2A54E18-9366-4BEB-8A10-67B7100D5DE7, name=\"ESP\"
"
printf "%s" "$sfdisk_script" | sfdisk "$OUT" >/dev/null

# Format Partition 1 as FAT32
mkfs.vfat --offset "$PART_OFFSET_SECTORS" -F 32 -n ESP "$OUT" >/dev/null

# Populate directories
mmd -i "$OUT@@$PART_OFFSET_BYTES" ::EFI
mmd -i "$OUT@@$PART_OFFSET_BYTES" ::EFI/BOOT
mmd -i "$OUT@@$PART_OFFSET_BYTES" ::EFI/REINSTALL

# Install the probe as the bootloader
mcopy -i "$OUT@@$PART_OFFSET_BYTES" "$PROBE" ::EFI/BOOT/${QEMU_BOOT_FILE:-BOOTX64.EFI}
# Install the child payload
mcopy -i "$OUT@@$PART_OFFSET_BYTES" "$CHILD" ::EFI/REINSTALL/CHILD.EFI

# Create a 4 MiB test RUNWAY.DAT
DUMMY_STORE="$(mktemp /var/tmp/rwy-reinstall.XXXXXX)"
dd if=/dev/zero of="$DUMMY_STORE" bs=1M count=4 status=none
mcopy -i "$OUT@@$PART_OFFSET_BYTES" "$DUMMY_STORE" ::RUNWAY.DAT
rm -f "$DUMMY_STORE"

echo "image created: $OUT ($(stat -c%s "$OUT") bytes)"
