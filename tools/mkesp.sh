#!/bin/sh
# Build esp.img: a GPT disk with a FAT ESP that holds the EFI binaries, and the store.
# STORE=block (default): the store is an XBOOTLDR partition after a small ESP.
# STORE=file: the store is RUNWAY.DAT on a large ESP (256 MiB store + binaries + FAT overhead).
# Usage: tools/mkesp.sh [out.img] [store.store] [partuuid of the partition that holds the store]
# Requires: dosfstools (mkfs.vfat), mtools (mmd/mcopy). No root needed.
set -eu

BUILD=${BUILD:-build}
. "$(dirname "$0")/imglib.sh"
OUT="${1:-esp.img}"
STORE_FILE="${2:-$BUILD/RUNWAY.DAT}"
if [ "$STORE" = block ]; then
    ESP_MB=$BLOCK_ESP_MB
    STORE_MB=$((($(stat -c %s "$STORE_FILE") + 1048575) / 1048576))
    IMG_MB=$((1 + ESP_MB + STORE_MB + 1))
else
    IMG_MB=330
fi

command -v sfdisk >/dev/null 2>&1 || { echo "missing sfdisk" >&2; exit 1; }
command -v mkfs.vfat >/dev/null 2>&1 || { echo "missing dosfstools" >&2; exit 1; }
command -v mcopy >/dev/null 2>&1 || { echo "missing mtools" >&2; exit 1; }

rm -f "$OUT"
truncate -s "${IMG_MB}M" "$OUT"

PART_OFFSET_SECTORS=2048
PART_OFFSET_BYTES=$((PART_OFFSET_SECTORS * 512))

if [ "$STORE" = block ]; then
    printf 'label: gpt\nstart=%s, size=%sMiB, type=%s, name="ESP"\nstart=%s, size=%sMiB, type=%s, %sname="STORE"\n' \
        "$PART_OFFSET_SECTORS" "$ESP_MB" "$ESP_TYPE" $(((1 + ESP_MB) * 2048)) "$STORE_MB" "$XBL_TYPE" \
        "${3:+uuid=$3, }" | sfdisk "$OUT" >/dev/null
else
    printf 'label: gpt\nstart=%s, type=%s, %sname="ESP"\n' "$PART_OFFSET_SECTORS" "$ESP_TYPE" \
        "${3:+uuid=$3, }" | sfdisk "$OUT" >/dev/null
fi

sz=$(sfdisk -d "$OUT" | awk -v n=1 '/start=/ {c++; if (c==n) {gsub(/[^0-9]/,"",$6); print $6}}')
mkfs_esp "$OUT" "$PART_OFFSET_SECTORS" $((sz / 2))

mmd -i "$OUT@@$PART_OFFSET_BYTES" ::EFI
mmd -i "$OUT@@$PART_OFFSET_BYTES" ::EFI/BOOT
mmd -i "$OUT@@$PART_OFFSET_BYTES" ::EFI/RUNWAY

RUNWAY_EFI="$BUILD/RUNWAY.EFI"
[ ! -f "$RUNWAY_EFI" ] && [ -f "$BUILD/efi/RUNWAY.EFI" ] && RUNWAY_EFI="$BUILD/efi/RUNWAY.EFI"

BOOT_FILE="${BOOT_FILE:-${QEMU_BOOT_FILE:-BOOTX64.EFI}}"
mcopy -i "$OUT@@$PART_OFFSET_BYTES" "$RUNWAY_EFI" ::EFI/RUNWAY/RUNWAY.EFI
mcopy -i "$OUT@@$PART_OFFSET_BYTES" "$RUNWAY_EFI" "::EFI/BOOT/$BOOT_FILE"
if [ "$BOOT_FILE" != "BOOTX64.EFI" ]; then
    mcopy -i "$OUT@@$PART_OFFSET_BYTES" "$RUNWAY_EFI" ::EFI/BOOT/BOOTX64.EFI
fi
store_put "$OUT" 0 "$STORE_FILE"

echo "--- ESP contents ---"
mdir -i "$OUT@@$PART_OFFSET_BYTES" ::EFI/BOOT
mdir -i "$OUT@@$PART_OFFSET_BYTES" :: | tail -n 8
echo "image: $OUT ($(stat -c%s "$OUT") bytes)"
