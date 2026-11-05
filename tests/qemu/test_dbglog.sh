#!/bin/sh
# tests/qemu/test_dbglog.sh — The debug loader leaves \RUNWAY.LOG on the ESP when it cannot boot.
# SPDX-License-Identifier: Apache-2.0
# The ESP has no store, so the loader stops and waits for a key; the test reads the file from the image.
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
LOG="$BUILD/dbglog.log"
IMG="$BUILD/dbglog.img"
OUT="$BUILD/dbglog.out"

rm -f "$IMG" "$OUT"
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
printf 'label: gpt\nstart=2048, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="ESP"\n' | sfdisk "$IMG" >/dev/null
mkfs.vfat --offset 2048 -F 32 "$IMG" >/dev/null
mmd -i "$IMG@@1048576" ::EFI ::EFI/BOOT
LOADER_BIN="$BUILD/RUNWAY_DEBUG.EFI"
[ ! -f "$LOADER_BIN" ] && LOADER_BIN="$BUILD/rwy-loader.efi"
mcopy -i "$IMG@@1048576" "$LOADER_BIN" "::EFI/BOOT/$QEMU_BOOT_FILE"
qemu_vars_copy

: > "$LOG"
"$QEMU_BIN" -nographic $(qemu_args) \
    -drive format=raw,file="$IMG" -serial stdio -display none > "$LOG" 2>&1 &
QEMU_PID=$!
trap 'kill $QEMU_PID 2>/dev/null || true' EXIT
for _ in $(seq 120); do
    grep -aq 'Press any key to continue' "$LOG" && break
    sleep 1
done
kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true

grep -aq 'Press any key to continue' "$LOG" || { echo "FAILED: loader did not stop: see $LOG" >&2; exit 1; }
mcopy -i "$IMG@@1048576" ::RUNWAY.LOG "$OUT" || { echo "FAILED: no RUNWAY.LOG on the ESP" >&2; exit 1; }
grep -aq 'boot store cannot be mounted' "$OUT" || { echo "FAILED: log lacks the reason: see $OUT" >&2; exit 1; }
# The log says what the firmware showed: the machine, the controllers and the disks.
for pat in 'diag: firmware ' 'diag: system ' 'diag: pci ' 'diag: [0-9]* block device handle' 'diag:   disk, block 512'; do
    grep -aq "$pat" "$OUT" || { echo "FAILED: log lacks '$pat': see $OUT" >&2; exit 1; }
done
# Lines end with CR LF in the file.
[ "$(grep -ac "$(printf '\r')\$" "$OUT")" -gt 0 ] || { echo "FAILED: log lines do not end with CR LF" >&2; exit 1; }
rm -f "$IMG" "$OUT"
echo ">>> SUCCESS: debug loader left RUNWAY.LOG on the ESP"
