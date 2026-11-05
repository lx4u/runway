#!/bin/sh
# tests/qemu/test_debian.sh — boot a Debian guest with RUNWAY.EFI as the only loader
# and verify the BLS FUSE projection is mounted at /boot.
# SPDX-License-Identifier: Apache-2.0
set -e
export PATH="$PATH:/sbin:/usr/sbin"
BUILD=${BUILD:-build/tests/qemu/bin}
. "$(dirname "$0")/qemu_lib.sh"
DEBIAN_DIR=${DEBIAN_DIR:-build/debian-$ARCH-$STORE}
IMG=$DEBIAN_DIR/debian.img
LOG=$DEBIAN_DIR/test.log

fail() { echo "FAIL: $1"; tail -n 60 "$LOG" 2>/dev/null; exit 1; }

# Skip if firmware or basic tools are missing
if [ -n "$QEMU_CODE" ] && [ ! -f "$QEMU_CODE" ]; then
    echo "test-debian: SKIP (firmware code not found)"
    exit 0
fi
if [ -n "$QEMU_VARS_TEMPLATE" ] && [ ! -f "$QEMU_VARS_TEMPLATE" ]; then
    echo "test-debian: SKIP (firmware vars not found)"
    exit 0
fi
command -v "$QEMU_BIN" >/dev/null 2>&1 || { echo "test-debian: SKIP ($QEMU_BIN not found)"; exit 0; }

# Build guest disk image if needed
if [ ! -f "$IMG" ]; then
    DEBIAN_DIR="$DEBIAN_DIR" BUILD="$BUILD" sh tests/qemu/mkdebian.sh "$IMG" || fail "mkdebian.sh failed"
fi

[ -f "$IMG" ] || { echo "test-debian: SKIP (debian image not built)"; exit 0; }

# Wait for a line containing text in $LOG
wait_for() {
    i=0
    while [ $i -lt 600 ]; do
        if grep -qF -- "$1" "$LOG" 2>/dev/null; then
            return 0
        fi
        grep -qE 'runway-guest-test.sh.*FAIL:|runway-guest-test.service: Failed' "$LOG" 2>/dev/null && fail "guest test failed while waiting for: $1"
        sleep 1
        i=$((i + 1))
    done
    fail "timeout waiting for: $1"
}

TEST_IMG=$DEBIAN_DIR/test.img
TEST_VARS=$DEBIAN_DIR/test_vars.fd
cp -f "$IMG" "$TEST_IMG"
qemu_vars_copy "$TEST_VARS"
rm -f "$LOG"

"$QEMU_BIN" $(qemu_args "$TEST_VARS") \
    -drive format=raw,file="$TEST_IMG" -display none \
    -serial file:"$LOG" &
QPID=$!

trap 'kill $QPID 2>/dev/null || true; rm -f "$TEST_IMG" "$TEST_VARS"' EXIT

echo "Waiting for guest test suite completion..."
wait_for "RUNWAY: booting CONFIG1"
wait_for "RUNWAY: $STORE_LOG"
! grep -qF -- "$STORE_LOG_OTHER" "$LOG" || fail "loader used the $STORE_LOG_OTHER"
wait_for "RUNWAY-GUEST: mount verified"
wait_for "RUNWAY-GUEST: store partition verified"
wait_for "RUNWAY-GUEST: gpt-auto-generator verified"
wait_for "RUNWAY-GUEST: automount verified"
wait_for "RUNWAY-GUEST: packages verified"
wait_for "RUNWAY-GUEST: bootctl list verified"
wait_for "RUNWAY-GUEST: bootctl status verified"
wait_for "RUNWAY-GUEST: bless good verified"
wait_for "RUNWAY-GUEST: kernel-install add Type 1 verified"
wait_for "RUNWAY-GUEST: bless bad verified"
wait_for "RUNWAY-GUEST: kernel-install remove verified"
wait_for "RUNWAY-GUEST: Type 2 UKI verified"
wait_for "RUNWAY-GUEST: removal refusal verified"
wait_for "RUNWAY-GUEST: bootctl install verified"
wait_for "RUNWAY-GUEST: ALL TESTS PASSED"

wait $QPID 2>/dev/null || true
echo "test-debian: guest tools tests passed"

echo "Testing trial boot fallback..."
store_get "$TEST_IMG" 0 "$DEBIAN_DIR/fallback.store"

# Verify CONFIG1 is OK after bless
"$BUILD/rwyctl" --store "$DEBIAN_DIR/fallback.store" status | grep -E -q "CONFIG1 +[0-9]+ +OK" || fail "CONFIG1 not OK after bless"

# Add unconfirmed trial slot CONFIG2 with higher revision that cannot be started
"$BUILD/host_fs" "$DEBIAN_DIR/fallback.store" mkdir EFI/RUNWAY/CONFIG2
"$BUILD/host_fs" "$DEBIAN_DIR/fallback.store" put EFI/RUNWAY/CONFIG2/KERNEL "invalid-kernel-bytes"
"$BUILD/host_fs" "$DEBIAN_DIR/fallback.store" put EFI/RUNWAY/CONFIG2/ENTRY "trial-fail.conf"
"$BUILD/host_fs" "$DEBIAN_DIR/fallback.store" put EFI/RUNWAY/CONFIG2/STATE "INSTALLED
"

store_put "$TEST_IMG" 0 "$DEBIAN_DIR/fallback.store"
rm -f "$LOG" "$DEBIAN_DIR/fallback.store"

"$QEMU_BIN" $(qemu_args "$TEST_VARS") \
    -drive format=raw,file="$TEST_IMG" -display none \
    -serial file:"$LOG" &
QPID=$!

wait_for "RUNWAY: $STORE_LOG"
wait_for "marked FAILED"
wait_for "RUNWAY: booting CONFIG1 (rev 1, OK)"

kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true
echo "test-debian: trial fallback passed"
echo "test-debian: PASS"

