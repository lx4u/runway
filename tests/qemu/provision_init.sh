#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export TERMINFO=/usr/share/terminfo

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /run /dev/pts /sys/firmware/efi/efivars

# Under QEMU the tests read the serial port; on hardware the console is the screen.
# A port that does not exist still opens, but every write fails, so look for a UART first.
if grep -qs QEMU /sys/class/dmi/id/sys_vendor; then
    for tty in ttyS0 ttyAMA0; do
        type=$(cat "/sys/class/tty/$tty/type" 2>/dev/null || true)
        if [ -n "$type" ] && [ "$type" != 0 ] && [ -c "/dev/$tty" ]; then
            exec <"/dev/$tty" >"/dev/$tty" 2>&1
        fi
    done
fi
mount -t devpts devpts /dev/pts 2>/dev/null || true

for m in efivarfs ahci ata_piix virtio_blk virtio_scsi sd_mod nvme vmd md_mod raid0 raid1 raid10 raid456 xhci_pci ehci_pci ohci_pci uhci_hcd usb_storage uas usbhid hid_generic vfat nls_cp437 nls_ascii nls_iso8859_1; do
    modprobe "$m" 2>/dev/null || true
done
mount -t efivarfs efivarfs /sys/firmware/efi/efivars 2>/dev/null || true
mdev -s 2>/dev/null || true
# Start the IMSM (Intel RST) volumes so that they are listed as disks.
mkdir -p /run/mdadm
mdadm --assemble --scan --run || true
mdev -s 2>/dev/null || true

if /payload/provision.sh; then
    exit 0
fi
echo "PROVISION: failed, dropping to a shell" >&2
exec sh
