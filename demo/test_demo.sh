#!/bin/sh
# demo/test_demo.sh — boot the demo image under QEMU and check the DEMO: lines on the serial console:
# v1 confirms and installs v2, v2 confirms and installs v3, v3 hangs, the watchdog
# (QEMU i6300esb) resets the VM and v2 runs again.
# SPDX-License-Identifier: Apache-2.0
# rwyctl must know the boot ESP (no warning in the log).
# Skips (exit 0) when the Alpine files are not in $BUILD/demo (run the demo-fetch target).
set -e
export PATH="$PATH:/sbin:/usr/sbin"
BUILD=${BUILD:-build}
IMG=$BUILD/demo/demo.img
LOG=$BUILD/demo/test.log
. tests/qemu/qemu_lib.sh
fail() { echo "FAIL: $1"; tail -n 40 "$LOG" 2>/dev/null; exit 1; }

[ -f "$BUILD/demo/pkg/boot/vmlinuz-lts" ] || { echo "test-demo: SKIP (run the demo-fetch target first)"; exit 0; }
sh demo/mkdemo.sh >/dev/null || fail "demo/mkdemo.sh"

# wait_for TEXT [AFTER_LINE]: set N to the line number of the first TEXT after AFTER_LINE (waits up to 120 s)
wait_for() {
    i=0
    while [ $i -lt 240 ]; do
        n=$(awk -v t="$1" -v a="${2:-0}" 'NR > a && index($0, t) { print NR; exit }' "$LOG" 2>/dev/null || true)
        [ -n "$n" ] && { N=$n; return 0; }
        sleep 0.5; i=$((i + 1))
    done
    fail "missing: $1"
}

cp -f "$IMG" "$BUILD/demo/test.img"
qemu_vars_copy "$BUILD/demo/test_vars.fd"
rm -f "$LOG"
"$QEMU_BIN" $(qemu_args "$BUILD/demo/test_vars.fd") \
    -drive format=raw,file="$BUILD/demo/test.img" -display none \
    -serial file:"$LOG" -device i6300esb -watchdog-action reset &
q=$!
trap 'kill $q 2>/dev/null; rm -f "$BUILD/demo/test.img" "$BUILD/demo/test_vars.fd"' EXIT

wait_for "DEMO: v1 booted CONFIG1"; n=$N
wait_for "DEMO: v1 confirmed, installing v2" "$n"; n=$N
wait_for "DEMO: v2 booted CONFIG2" "$n"; n=$N
wait_for "DEMO: v2 confirmed, installing v3" "$n"; n=$N
wait_for "RUNWAY: WATCHDOG 20s armed by I6300ESB.EFI" "$n"; n=$N
wait_for "DEMO: v3 booted CONFIG1" "$n"; n=$N
wait_for "DEMO: v3 will not confirm" "$n"; n=$N
wait_for "DEMO: v2 booted CONFIG2" "$n"; n=$N
wait_for "DEMO: v3 failed, v2 is running, demo complete" "$n"
! grep -q "boot ESP unknown" "$LOG" || fail "rwyctl does not know the boot ESP"
echo "test-demo: PASS"
