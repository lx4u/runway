#!/bin/sh
# tests/qemu/test_wdt.sh — watchdog driver applications under QEMU.
# SPDX-License-Identifier: Apache-2.0
# The runner arms the watchdog and hangs; the VM must reset and run it again.
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
LOG="$BUILD/wdt.log"
IMG="$BUILD/wdt.img"
VARS="$BUILD/wdt_vars.fd"
OFF=1048576

command -v sfdisk >/dev/null 2>&1 || { echo "missing sfdisk" >&2; exit 1; }
command -v mcopy >/dev/null 2>&1 || { echo "missing mtools" >&2; exit 1; }

# Boot the runner on a FAT image, with the driver $2 at \WDT.EFI; other arguments go to QEMU.
run_mode() {
    mode="$1"; drv="$2"; shift 2
    rm -f "$IMG"
    dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
    printf 'label: gpt\nstart=2048, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B\n' | sfdisk "$IMG" >/dev/null
    mkfs.vfat --offset 2048 -F 32 "$IMG" >/dev/null
    mmd -i "$IMG@@$OFF" ::EFI ::EFI/BOOT
    mcopy -i "$IMG@@$OFF" "$BUILD/WDT_RUNNER.EFI" "::EFI/BOOT/$QEMU_BOOT_FILE"
    mcopy -i "$IMG@@$OFF" "$BUILD/$drv.EFI" ::WDT.EFI
    printf "%s\n" "$mode" | mcopy -i "$IMG@@$OFF" - ::MODE.TXT
    qemu_vars_copy "$VARS"
    timeout "${TMO:-40}" "$QEMU_BIN" -nographic \
        $(qemu_args "$VARS" 512) \
        -drive format=raw,file="$IMG" \
        -watchdog-action reset "$@" \
        -serial stdio -display none > "$LOG" 2>&1 || true
}

run() {
    run_mode arm "$@"
}

check_resets() {
    runs=$(grep -c "WDT RUNNER: start" "$LOG")
    grep -q "$1: armed" "$LOG" || { cat "$LOG"; echo "FAILED: $1 not armed" >&2; exit 1; }
    [ "$runs" -ge 2 ] || { cat "$LOG"; echo "FAILED: $1 no reset ($runs run)" >&2; exit 1; }
    echo "[OK] $1 armed, VM reset ($runs runs)"
}

check_stop() {
    runs=$(grep -c "WDT RUNNER: start" "$LOG")
    grep -q "$1: stopped" "$LOG" || { cat "$LOG"; echo "FAILED: $1 not stopped" >&2; exit 1; }
    grep -q "WDT RUNNER: stop verified (no reset)" "$LOG" || { cat "$LOG"; echo "FAILED: $1 reset occurred or stop verification failed" >&2; exit 1; }
    [ "$runs" -eq 1 ] || { cat "$LOG"; echo "FAILED: $1 unexpected reset ($runs runs)" >&2; exit 1; }
    echo "[OK] $1 armed and stopped, no reset"
}

check_stop_direct() {
    runs=$(grep -c "WDT RUNNER: start" "$LOG")
    grep -q "$1: stopped" "$LOG" || { cat "$LOG"; echo "FAILED: $1 not stopped" >&2; exit 1; }
    grep -q "WDT RUNNER: stopped" "$LOG" || { cat "$LOG"; echo "FAILED: $1 stop failed" >&2; exit 1; }
    [ "$runs" -eq 1 ] || { cat "$LOG"; echo "FAILED: $1 unexpected reset ($runs runs)" >&2; exit 1; }
    echo "[OK] $1 direct stop succeeded"
}

check_stop_unsupported() {
    runs=$(grep -c "WDT RUNNER: start" "$LOG")
    grep -q "driver status 0x8000000000000003" "$LOG" || { cat "$LOG"; echo "FAILED: $1 expected EFI_UNSUPPORTED on stop" >&2; exit 1; }
    [ "$runs" -eq 1 ] || { echo "FAILED: $1 unexpected reset" >&2; exit 1; }
    echo "[OK] $1 stop unsupported, no reset"
}

echo "--- i6300esb present: the watchdog resets the hung VM ---"
run I6300ESB -device i6300esb
check_resets I6300ESB

echo "--- i6300esb present: stop prevents VM reset ---"
run_mode stop_arm I6300ESB -device i6300esb
check_stop I6300ESB

echo "--- i6300esb present: direct stop succeeds ---"
run_mode stop I6300ESB -device i6300esb
check_stop_direct I6300ESB

# ITCO and WDAT are x86 only.
if [ "$ARCH" = "x86_64" ]; then
    echo "--- ICH9 TCO (q35 chipset): the watchdog resets the hung VM ---"
    run ITCO
    check_resets ITCO

    echo "--- ICH9 TCO (q35 chipset): stop prevents VM reset ---"
    run_mode stop_arm ITCO
    check_stop ITCO

    echo "--- ICH9 TCO (q35 chipset): direct stop succeeds ---"
    run_mode stop ITCO
    check_stop_direct ITCO

    echo "--- WDAT table describing the TCO timer (reboot enabled by the table): the watchdog resets the hung VM ---"
    python3 tests/qemu/mkwdat.py "$BUILD/wdat.bin"
    run WDAT -acpitable sig=WDAT,data="$BUILD/wdat.bin"
    check_resets WDAT

    echo "--- WDAT table describing the TCO timer: stop prevents VM reset ---"
    run_mode stop_arm WDAT -acpitable sig=WDAT,data="$BUILD/wdat.bin"
    check_stop WDAT

    echo "--- WDAT table describing the TCO timer: direct stop succeeds ---"
    run_mode stop WDAT -acpitable sig=WDAT,data="$BUILD/wdat.bin"
    check_stop_direct WDAT

    echo "--- WDAT table without stop action: stop returns EFI_UNSUPPORTED ---"
    python3 tests/qemu/mkwdat.py --nostop "$BUILD/wdat_nostop.bin"
    run_mode stop WDAT -acpitable sig=WDAT,data="$BUILD/wdat_nostop.bin"
    check_stop_unsupported WDAT
    rm -f "$BUILD/wdat_nostop.bin"
fi

# Hardware or table absent: the driver returns EFI_UNSUPPORTED and nothing resets.
TMO=12
DRIVERS=I6300ESB
[ "$ARCH" = "x86_64" ] && DRIVERS="$DRIVERS ITCO WDAT"
for drv in $DRIVERS; do
    echo "--- $drv absent ---"
    case $drv in
    ITCO) run $drv -machine pc ;;
    *) run $drv ;;
    esac
    grep -q "driver status 0x8000000000000003" "$LOG" || { cat "$LOG"; echo "FAILED: $drv expected EFI_UNSUPPORTED" >&2; exit 1; }
    [ "$(grep -c "WDT RUNNER: start" "$LOG")" -eq 1 ] || { echo "FAILED: $drv unexpected reset" >&2; exit 1; }
    echo "[OK] $drv unsupported, no reset"

    echo "--- $drv absent stop ---"
    case $drv in
    ITCO) run_mode stop $drv -machine pc ;;
    *) run_mode stop $drv ;;
    esac
    check_stop_unsupported $drv
done
rm -f "$BUILD/wdat.bin"
rm -f "$IMG" "$VARS"
