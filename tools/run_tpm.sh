#!/bin/sh
# tools/run_tpm.sh — Automated QEMU invocation with emulated TPM 2.0 (swtpm).
# NO_TPM=1 boots without any TPM device. QEMU is killed after QEMU_TIMEOUT seconds (default 90):
# RUNWAY.EFI returns to the firmware when it cannot mount the boot store, and nothing then powers off.
set -e

BUILD=${BUILD:-build}
TPM_DIR="${TPM_DIR:-$BUILD/tpmstate}"
. "$(dirname "$0")/../tests/qemu/qemu_lib.sh"
ESP_IMG="${ESP_IMG:-esp.img}"
# Optional extra QEMU args, e.g. "-drive format=raw,file=esp1.img" (mirror members).
EXTRA_DRIVES="${EXTRA_DRIVES:-}"

mkdir -p "$BUILD"
mkdir -p "$TPM_DIR"

SOCK="$TPM_DIR/swtpm-sock"
TPM_ARGS=""
if [ "${NO_TPM:-0}" != 1 ]; then
    rm -f "$SOCK"

    # Start swtpm daemon in background
    swtpm socket \
        --tpmstate dir="$TPM_DIR" \
        --ctrl type=unixio,path="$SOCK" \
        --tpm2 \
        --flags not-need-init &
    SWTPM_PID=$!

    cleanup() {
        kill -9 "$SWTPM_PID" 2>/dev/null || true
    }
    trap cleanup EXIT INT TERM

    # Wait for unix socket to become ready
    for i in $(seq 1 30); do
        if [ -S "$SOCK" ]; then
            break
        fi
        sleep 0.1
    done

    if [ ! -S "$SOCK" ]; then
        echo "ERROR: swtpm failed to start or socket not created" >&2
        exit 1
    fi
    TPM_ARGS="-chardev socket,id=chrtpm,path=$SOCK -tpmdev emulator,id=tpm0,chardev=chrtpm -device $QEMU_TPM_DEVICE,tpmdev=tpm0"
fi

# Fresh vars copy if not present
if [ ! -f "$QEMU_VARS" ] || [ "${RESET_VARS:-0}" = "1" ]; then
    qemu_vars_copy
fi

QEMU_EXTRA=""
if [ "$1" = "--debug" ]; then
    QEMU_EXTRA="-d guest_errors -D $BUILD/qemu.log"
fi

# Launch QEMU with TPM and emulator chardev
timeout "${QEMU_TIMEOUT:-90}" "$QEMU_BIN" \
    -nographic \
    $(qemu_args) \
    -drive format=raw,file="$ESP_IMG" \
    $EXTRA_DRIVES \
    $TPM_ARGS \
    -serial stdio -display none \
    $QEMU_EXTRA
