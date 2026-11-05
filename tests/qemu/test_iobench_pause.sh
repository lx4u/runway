#!/bin/sh
# tests/qemu/test_iobench_pause.sh — a failed run waits for a key and keeps its console text.
# SPDX-License-Identifier: Apache-2.0
# The only target disk has no PERF/READY marker, so the run fails. With pause=fail the application must wait
# for a key before it powers off, and must have saved its console text in the results store and on the ESP.
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
LOG="$BUILD/iobench-pause.log"
BOOT="$BUILD/iobench-pause-boot.img"
T1="$BUILD/iobench-pause-1.img"
DEK="$BUILD/iobench-pause.dek"
KERNEL="$BUILD/iobench-pause.kernel"
INITRD="$BUILD/iobench-pause.initrd"
RWYCTL="$BUILD/rwyctl"
OUT=$(mktemp -d /var/tmp/rwy-iobench-pause.XXXXXX)
trap 'rm -rf "$OUT"' EXIT

[ -x "$RWYCTL" ] || RWYCTL="$BUILD/tools/rwyctl"
dd if=/dev/urandom of="$DEK" bs=32 count=1 2>/dev/null
printf x >"$KERNEL"
printf x >"$INITRD"
IOBENCH_ROUNDS=1 IOBENCH_WARMUP=0 IOBENCH_DEVICES=block IOBENCH_PAUSE=fail \
    sh tests/qemu/mkiobench.sh --boot "$BOOT" "$BUILD/IOBENCH.EFI" "" "$KERNEL" "$INITRD" "$DEK"
IOBENCH_NO_MARKER=1 sh tests/qemu/mkiobench.sh --target "$T1" "$DEK"

# Press a key once the application asks for it.
press_key() {
    n=0
    until grep -aq 'Press any key' "$LOG" 2>/dev/null; do
        n=$((n + 1))
        [ "$n" -lt 240 ] || return 0
        sleep 1
    done
    sleep 1
    printf 'x'
    sleep 20
}

qemu_vars_copy
: >"$LOG"
# shellcheck disable=SC2046
press_key | timeout 300 "$QEMU_BIN" -nographic $(qemu_args) \
    -drive format=raw,file="$BOOT" \
    -drive format=raw,file="$T1" \
    -serial stdio -display none >>"$LOG" 2>&1 || true

grep -a '^iobench' "$LOG" || true
grep -aq 'Press any key' "$LOG" || { echo "FAILED: the application did not ask for a key" >&2; exit 1; }
grep -aq '^IOBENCH FAIL' "$LOG" || { echo "FAILED: the run did not fail" >&2; exit 1; }

esp_off=$(img_part_off "$BOOT" 1)
mcopy -o -i "$BOOT@@$esp_off" ::PERF/CONSOLE.LOG "$OUT/esp.log" ||
    { echo "FAILED: no CONSOLE.LOG on the ESP" >&2; exit 1; }
grep -q 'no marker' "$OUT/esp.log" || { echo "FAILED: the ESP log lacks the reason" >&2; exit 1; }
grep -q '^IOBENCH FAIL' "$OUT/esp.log" || { echo "FAILED: the ESP log lacks the verdict" >&2; exit 1; }

off=$(img_part_off "$BOOT" 3)
len=$(img_part_bytes "$BOOT" 3)
"$RWYCTL" --store "$BOOT@$off+$len" cat PERF/CONSOLE.LOG >"$OUT/store.log" ||
    { echo "FAILED: no PERF/CONSOLE.LOG in the results store" >&2; exit 1; }
grep -q 'no marker' "$OUT/store.log" || { echo "FAILED: the store log lacks the reason" >&2; exit 1; }

echo ">>> SUCCESS: the failed run waited for a key and saved its console text"
