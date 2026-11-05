# tests/qemu/qemu_lib.sh — QEMU test configuration and helpers per architecture.
# SPDX-License-Identifier: Apache-2.0
# Sourced by QEMU test scripts. Defines QEMU_BIN, QEMU_MACHINE, QEMU_BOOT_FILE,
# QEMU_CODE, QEMU_VARS_TEMPLATE, QEMU_VARS, QEMU_EXTRA_DEVICES, QEMU_TPM_DEVICE.
# STORE=block|file picks the store layout of the images (tools/imglib.sh, which this file sources:
# store_get, store_put, store_partuuid). Helpers:
#   qemu_vars_copy [target]
#   qemu_fw_args [vars_file]
#   qemu_base_args [mem]
#   qemu_args [vars_file] [mem]

BUILD="${BUILD:-build}"
ARCH="${ARCH:-x86_64}"
. tools/imglib.sh

case "$ARCH" in
    arm64|aarch64) ARCH="aarch64" ;;
    amd64|x86_64)  ARCH="x86_64" ;;
esac

if [ "$ARCH" = "aarch64" ]; then
    QEMU_BIN="${QEMU_BIN:-qemu-system-aarch64}"
    QEMU_MACHINE="${QEMU_MACHINE:-virt,accel=kvm:tcg}"
    QEMU_BOOT_FILE="${QEMU_BOOT_FILE:-BOOTAA64.EFI}"
    QEMU_MULTIARCH=aarch64-linux-gnu QEMU_DEB_ARCH=arm64 QEMU_SDBOOT_NAME=systemd-bootaa64.efi
    QEMU_EXTRA_DEVICES="${QEMU_EXTRA_DEVICES:--device virtio-rng-pci}"
    QEMU_TPM_DEVICE="${QEMU_TPM_DEVICE:-tpm-tis-device}"
    FW="${FW:-aavmf}"

    if [ "$FW" = "uboot" ]; then
        UBOOT_BIN="${UBOOT_BIN:-/usr/lib/u-boot/qemu_arm64/u-boot.bin}"
        QEMU_CODE=""
        QEMU_VARS_TEMPLATE=""
        QEMU_VARS=""
    else
        if [ -z "${AAVMF_CODE:-}" ]; then
            if [ -f /usr/share/AAVMF/AAVMF_CODE.fd ]; then
                AAVMF_CODE=/usr/share/AAVMF/AAVMF_CODE.fd
            elif [ -f /usr/share/qemu-efi-aarch64/QEMU_EFI.fd ]; then
                AAVMF_CODE=/usr/share/qemu-efi-aarch64/QEMU_EFI.fd
            else
                AAVMF_CODE=/usr/share/AAVMF/AAVMF_CODE.fd
            fi
        fi
        if [ -z "${AAVMF_VARS_TEMPLATE:-}" ]; then
            AAVMF_VARS_TEMPLATE=/usr/share/AAVMF/AAVMF_VARS.fd
        fi
        QEMU_CODE="$AAVMF_CODE"
        QEMU_VARS_TEMPLATE="$AAVMF_VARS_TEMPLATE"
        QEMU_VARS="${QEMU_VARS:-$BUILD/AAVMF_VARS.fd}"
    fi
else
    QEMU_BIN="${QEMU_BIN:-qemu-system-x86_64}"
    QEMU_MACHINE="${QEMU_MACHINE:-q35,accel=kvm:tcg}"
    QEMU_BOOT_FILE="${QEMU_BOOT_FILE:-BOOTX64.EFI}"
    QEMU_MULTIARCH=x86_64-linux-gnu QEMU_DEB_ARCH=amd64 QEMU_SDBOOT_NAME=systemd-bootx64.efi
    QEMU_EXTRA_DEVICES="${QEMU_EXTRA_DEVICES:-}"
    QEMU_TPM_DEVICE="${QEMU_TPM_DEVICE:-tpm-tis}"

    if [ -z "${OVMF_CODE:-}" ]; then
        if [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ]; then
            OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
        elif [ -f /usr/share/OVMF/OVMF_CODE.fd ]; then
            OVMF_CODE=/usr/share/OVMF/OVMF_CODE.fd
        else
            OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
        fi
    fi
    if [ -z "${OVMF_VARS_TEMPLATE:-}" ]; then
        if [ -f /usr/share/OVMF/OVMF_VARS_4M.fd ]; then
            OVMF_VARS_TEMPLATE=/usr/share/OVMF/OVMF_VARS_4M.fd
        elif [ -f /usr/share/OVMF/OVMF_VARS.fd ]; then
            OVMF_VARS_TEMPLATE=/usr/share/OVMF/OVMF_VARS.fd
        else
            OVMF_VARS_TEMPLATE=/usr/share/OVMF/OVMF_VARS_4M.fd
        fi
    fi
    QEMU_CODE="$OVMF_CODE"
    QEMU_VARS_TEMPLATE="$OVMF_VARS_TEMPLATE"
    QEMU_VARS="${QEMU_VARS:-$BUILD/OVMF_VARS.fd}"
fi

# systemd-boot binary for $ARCH: system package if present, else unpack the .deb (no install).
sdboot_efi() {
    SDBOOT_EFI="${SDBOOT_EFI:-/usr/lib/systemd/boot/efi/$QEMU_SDBOOT_NAME}"
    [ -f "$SDBOOT_EFI" ] && return 0
    SDBOOT_EFI=$BUILD/sdboot/usr/lib/systemd/boot/efi/$QEMU_SDBOOT_NAME
    [ -f "$SDBOOT_EFI" ] && return 0
    mkdir -p "$BUILD/sdboot"
    (cd "$BUILD/sdboot" && apt-get download "systemd-boot-efi:$QEMU_DEB_ARCH" >/dev/null 2>&1 \
        && dpkg-deb -x systemd-boot-efi_*_"$QEMU_DEB_ARCH".deb .)
}

# Image tools run as child processes
export ARCH QEMU_BOOT_FILE

qemu_vars_copy() {
    dst="${1:-$QEMU_VARS}"
    tmpl="${QEMU_VARS_TEMPLATE:-}"
    [ "$ARCH" = "x86_64" ] && tmpl="${OVMF_VARS_TEMPLATE:-$tmpl}"
    [ "$ARCH" = "aarch64" ] && tmpl="${AAVMF_VARS_TEMPLATE:-$tmpl}"
    if [ -n "$tmpl" ] && [ -n "$dst" ]; then
        cp -f "$tmpl" "$dst"
    fi
}

qemu_fw_args() {
    if [ "${FW:-}" = "uboot" ]; then
        printf -- '-bios %s' "$UBOOT_BIN"
    else
        code="${QEMU_CODE:-}"
        [ "$ARCH" = "x86_64" ] && code="${OVMF_CODE:-$code}"
        [ "$ARCH" = "aarch64" ] && code="${AAVMF_CODE:-$code}"
        vars="${1:-$QEMU_VARS}"
        printf -- '-drive if=pflash,format=raw,readonly=on,file=%s -drive if=pflash,format=raw,file=%s' "$code" "$vars"
    fi
}

qemu_base_args() {
    mem="${1:-${QEMU_MEM:-1024}}"
    printf -- '-nodefaults -machine %s -cpu max -m %s' "$QEMU_MACHINE" "$mem"
}

qemu_args() {
    vars="${1:-$QEMU_VARS}"
    mem="${2:-${QEMU_MEM:-1024}}"
    extra=""
    [ -n "$QEMU_EXTRA_DEVICES" ] && extra=" $QEMU_EXTRA_DEVICES"
    printf -- '%s %s%s' "$(qemu_base_args "$mem")" "$(qemu_fw_args "$vars")" "$extra"
}
