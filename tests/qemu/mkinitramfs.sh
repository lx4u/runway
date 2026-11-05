#!/bin/sh
# tests/qemu/mkinitramfs.sh — build the minimal Linux initramfs used by the provisioning slot.
# SPDX-License-Identifier: Apache-2.0
# Usage: mkinitramfs.sh INITRD KERNEL_OUT
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
OUT=${1:?usage: $0 INITRD KERNEL_OUT}
KERNEL_OUT=${2:?usage: $0 INITRD KERNEL_OUT}
SRC_DIR=$(cd "$(dirname "$0")/../.." && pwd)

ROOTFS=${DEBIAN_ROOTFS:-}
if [ -z "$ROOTFS" ]; then
    for f in "/var/tmp/rwy-debian-cache-$QEMU_DEB_ARCH.tar" "$BUILD/debian/rootfs.tar"; do
        if [ -f "$f" ]; then ROOTFS=$f; break; fi
    done
fi
[ -n "$ROOTFS" ] || { echo "mkinitramfs: no Debian rootfs tar (set DEBIAN_ROOTFS)" >&2; exit 77; }

for t in tar cpio gzip ldd; do
    command -v "$t" >/dev/null 2>&1 || { echo "mkinitramfs: missing $t" >&2; exit 1; }
done
MKSTORE=$BUILD/mkstore
[ -x "$MKSTORE" ] || MKSTORE=$BUILD/tools/mkstore
RWYCTL=$BUILD/rwyctl
[ -x "$RWYCTL" ] || RWYCTL=$BUILD/tools/rwyctl
IOBENCH_LINUX=$BUILD/iobench_linux
[ -x "$IOBENCH_LINUX" ] || IOBENCH_LINUX=$BUILD/tools/iobench_linux
for f in "$BUILD/IOBENCH.EFI" "$MKSTORE" "$RWYCTL" "$IOBENCH_LINUX"; do
    [ -f "$f" ] || { echo "mkinitramfs: missing $f" >&2; exit 1; }
done

T=$(mktemp -d /var/tmp/rwy-initramfs.XXXXXX)
trap 'rm -rf "$T"' EXIT
R="$T/root"
mkdir -p "$R/bin" "$R/sbin" "$R/usr/bin" "$R/usr/sbin" "$R/lib" "$R/lib64" \
    "$R/proc" "$R/sys" "$R/dev" "$R/run" "$R/mnt/esp" "$R/payload" \
    "$R/usr/share"

KREL=$(tar -tf "$ROOTFS" | grep -E '^(\./)?boot/vmlinuz-' | head -n 1 || true)
[ -n "$KREL" ] || { echo "mkinitramfs: no kernel in $ROOTFS" >&2; exit 1; }
KVER=${KREL##*/vmlinuz-}
MODDIR="usr/lib/modules/$KVER"
tar -xf "$ROOTFS" -C "$T" "$KREL"
mkdir -p "$(dirname "$KERNEL_OUT")"
cp "$T/$KREL" "$KERNEL_OUT"

# What the provisioning slot needs to see a SATA, NVMe (also behind VMD), RAID (IMSM) or USB disk and a USB keyboard, by file name.
# Their dependencies come from modules.dep.
WANT="scsi_common scsi_mod sd_mod virtio_scsi libata libahci ahci ata_piix virtio_blk
efivarfs fat vfat nls_cp437 nls_ascii nls_iso8859-1
nvme nvme-core vmd md-mod raid0 raid1 raid10 raid456 xhci-pci xhci-hcd ehci-pci ehci-hcd ohci-pci ohci-hcd uhci-hcd
usbcore usb-common usb-storage uas hid usbhid hid-generic"
mkdir -p "$R/lib/modules/$KVER"
tar -xf "$ROOTFS" -C "$T" "./$MODDIR/modules.dep" "./$MODDIR/modules.builtin"
cp "$T/$MODDIR/modules.dep" "$T/$MODDIR/modules.builtin" "$R/lib/modules/$KVER/"
MODULES=$(awk -v want="$WANT" '
    BEGIN { n = split(want, w, " "); for (i = 1; i <= n; i++) { k = w[i]; gsub(/_/, "-", k); need[k] = 1 } }
    {
        path = substr($0, 1, index($0, ":") - 1)
        name = path; sub(/.*\//, "", name); sub(/\.ko.*/, "", name); gsub(/_/, "-", name)
        line[name] = $0
    }
    END {
        for (k in need) {
            if (!(k in line)) continue
            m = split(substr(line[k], index(line[k], ":") + 1), d, " ")
            p = substr(line[k], 1, index(line[k], ":") - 1)
            out[p] = 1
            for (i = 1; i <= m; i++) out[d[i]] = 1
        }
        for (p in out) print p
    }' "$T/$MODDIR/modules.dep" | sort)
for m in $MODULES; do
    tar -xf "$ROOTFS" -C "$T" "./$MODDIR/$m"
    dest="$R/lib/modules/$KVER/$m"
    mkdir -p "$(dirname "$dest")"
    cp "$T/$MODDIR/$m" "$dest"
done

cp /usr/bin/busybox "$R/bin/busybox"
for app in $(/usr/bin/busybox --list); do
    [ -e "$R/bin/$app" ] || ln -s busybox "$R/bin/$app"
done
cp /usr/sbin/sfdisk "$R/sbin/sfdisk"
cp /usr/sbin/wipefs "$R/sbin/wipefs"
cp /usr/sbin/mdadm /usr/sbin/mdmon /usr/sbin/mkfs.fat "$R/sbin/"
mkdir -p "$R/etc"
echo "AUTO +imsm +all" > "$R/etc/mdadm.conf"
cp /usr/bin/whiptail "$R/usr/bin/whiptail"
cp "$MKSTORE" "$R/usr/bin/mkstore"
cp "$RWYCTL" "$R/usr/bin/rwyctl"
cp "$IOBENCH_LINUX" "$R/usr/bin/iobench_linux"
cp "$BUILD/IOBENCH.EFI" "$R/payload/IOBENCH.EFI"
cp "$SRC_DIR/tests/qemu/provision.sh" "$R/payload/provision.sh"
cp "$SRC_DIR/tests/qemu/provision_hw.sh" "$R/payload/provision_hw.sh"
cp "$SRC_DIR/tests/qemu/provision_init.sh" "$R/init"
chmod 755 "$R/init" "$R/payload/provision.sh" "$R/payload/provision_hw.sh" "$R/usr/bin/mkstore" "$R/usr/bin/rwyctl" "$R/usr/bin/iobench_linux"

copy_libs() {
    ldd "$1" 2>/dev/null | awk '/=> \// {print $3} /^[[:space:]]*\// {print $1}' | while read -r lib; do
        [ -f "$lib" ] || continue
        dest="$R$lib"
        mkdir -p "$(dirname "$dest")"
        cp -L "$lib" "$dest"
    done
}
copy_libs /usr/bin/busybox
copy_libs /usr/sbin/sfdisk
copy_libs /usr/sbin/wipefs
copy_libs /usr/sbin/mdadm
copy_libs /usr/sbin/mdmon
copy_libs /usr/sbin/mkfs.fat
copy_libs /usr/bin/whiptail
copy_libs "$MKSTORE"
copy_libs "$RWYCTL"
copy_libs "$IOBENCH_LINUX"
cp -a /usr/share/terminfo "$R/usr/share/terminfo"

mkdir -p "$(dirname "$OUT")"
(cd "$R" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9) > "$OUT"
echo "initramfs: $OUT ($(du -h "$OUT" | cut -f1))"
