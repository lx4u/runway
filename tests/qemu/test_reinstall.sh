#!/bin/sh
# tests/qemu/test_reinstall.sh — Automated test harness for Phase 0 reinstall probe under QEMU.
# SPDX-License-Identifier: Apache-2.0
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
LOG="$BUILD/reinstall.log"
IMG="$BUILD/reinstall_gpt.img"

echo "================================================================="
echo "   Runway Phase 0: Standalone ReinstallProtocolInterface Test      "
echo "================================================================="

mkdir -p "$BUILD"

# 1. Build required binaries

# 2. Assemble GPT disk image
sh tests/qemu/mkgpt_reinstall.sh "$IMG" "$BUILD/REINSTALL_PROBE.EFI" "$BUILD/REINSTALL_CHILD.EFI"

# 3. Clean variable store
qemu_vars_copy

# 4. Boot QEMU
echo "--- Running QEMU under firmware ---"
"$QEMU_BIN" \
    -nographic \
    $(qemu_args) \
    -drive format=raw,file="$IMG" \
    -serial stdio -display none > "$LOG" 2>&1 || true

cat "$LOG"

# 5. Verify assertions
echo "\n--- Verifying Phase 0 Acceptance Criteria ---"

if ! grep -q "TEST 1: ReinstallProtocolInterface on active boot handle: PASS" "$LOG"; then
    echo "FAILED: Assertion 1 (ReinstallProtocolInterface on active boot handle) did not pass" >&2
    exit 1
fi
echo "[OK] 1. gBS->ReinstallProtocolInterface succeeded on active boot handle"

if ! grep -q "TEST 2: Already-opened store_fh remains functional: PASS" "$LOG"; then
    echo "FAILED: Assertion 2 (store_fh read/write after reinstallation) did not pass" >&2
    exit 1
fi
echo "[OK] 2. store_fh continued read/write/flush without error"

if ! grep -q "TEST 3: DevicePath unchanged & authentic GPT node preserved: PASS" "$LOG"; then
    echo "FAILED: Assertion 3 (DevicePath & GPT node integrity) did not pass" >&2
    exit 1
fi
echo "[OK] 3. DevicePath preserved authentic parent bus path & GPT HARDDRIVE_DEVICE_PATH node"

if ! grep -q "TEST 4: Firmware LoadImage resolves via reinstalled protocol: PASS" "$LOG"; then
    echo "FAILED: Assertion 4 (Firmware LoadImage resolution) did not pass" >&2
    exit 1
fi
echo "[OK] 4. Firmware gBS->LoadImage resolved against reinstalled protocol interface"

if ! grep -q "ALL 4 PHASE 0 REINSTALL PROTOCOL PROOFS PASSED" "$LOG"; then
    echo "FAILED: Final summary confirmation missing in log" >&2
    exit 1
fi

echo "\n>>> SUCCESS: All Phase 0 reinstall probe requirements verified! <<<"
