#!/bin/sh
# Boot RUNWAY.EFI under QEMU, with images made in $BUILD.
# Usage: tools/run_qemu.sh run | run-debug | run-mirror | run-tpm | run-tpm-debug
set -eu
cd "$(dirname "$0")/.."
BUILD=${BUILD:-build}
. tests/qemu/qemu_lib.sh
mode=${1:?usage: run_qemu.sh MODE}

export ESP_IMG=$BUILD/esp.img
sh tools/mkesp.sh "$ESP_IMG" >/dev/null
case $mode in
run-tpm) exec sh tools/run_tpm.sh ;;
run-tpm-debug) exec sh tools/run_tpm.sh --debug ;;
esac

drives="-drive format=raw,file=$ESP_IMG"
case $mode in
run-mirror)
	sh tests/qemu/mkesp_mirror.sh "$BUILD/esp0.img" "$BUILD/esp1.img" >/dev/null
	drives="-drive format=raw,file=$BUILD/esp0.img -drive format=raw,file=$BUILD/esp1.img" ;;
esac
extra=-nographic
[ "$mode" = run-debug ] && extra="-d guest_errors -D $BUILD/qemu.log"

qemu_vars_copy
exec "$QEMU_BIN" $(qemu_args) \
	-serial stdio -display none $extra $drives
