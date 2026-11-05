#!/bin/sh
# demo/mkdemo.sh — build $BUILD/demo/demo.img: a GPT disk whose ESP boots RUNWAY.EFI and holds a store with
# the Alpine lts kernel and an initramfs that runs demo/init. Slot CONFIG1 is version 1.
# Needs the files of the demo-fetch target, and the host tools linked into $BUILD by the demo target. No root.
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
BUILD=${BUILD:-build}
D=$BUILD/demo
OUT=${1:-$D/demo.img}
STORE=$D/RUNWAY.DAT
ESP_MB=160
STORE_MB=128
START=2048

for f in $D/pkg/boot/vmlinuz-lts $D/pkg/bin/busybox.static $BUILD/RUNWAY.EFI $BUILD/mkstore $BUILD/host_fs $BUILD/rwyctl $BUILD/rwyctl-static $BUILD/I6300ESB.EFI $BUILD/ITCO.EFI $BUILD/WDAT.EFI; do
    [ -f "$f" ] || { echo "mkdemo: missing $f" >&2; exit 1; }
done

# initramfs: static busybox, the storage and FAT modules with their dependencies, a static rwyctl, /init
MODS="efivarfs ahci libahci libata sd_mod nvme virtio_blk virtio_pci virtio_scsi usb_storage uas xhci_pci ehci_pci uhci_hcd ohci_pci vfat nls_utf8 nls_cp437 nls_iso8859_1"
KVER=$(ls $D/pkg/lib/modules)
KDIR=$D/pkg/lib/modules/$KVER
RT=$D/rootfs
rm -rf $RT; mkdir -p $RT/bin $RT/sbin $RT/usr/bin $RT/usr/sbin $RT/lib/modules/$KVER $RT/tmp $RT/dev $RT/proc $RT/sys $RT/etc
cp $D/pkg/bin/busybox.static $RT/bin/busybox
ln -s busybox $RT/bin/sh
cp $BUILD/rwyctl-static $RT/usr/bin/rwyctl
cp "$(dirname "$0")/init" $RT/init
# modules: the closure of $MODS over modules.dep, unpacked, with a matching modules.dep
awk -v want="$MODS" '
    BEGIN { n = split(want, w, " "); for (i = 1; i <= n; i++) need[w[i]] = 1 }
    { split($0, f, ":"); dep[f[1]] = f[2]; path = f[1]; sub(/.*\//, "", path); sub(/\.ko.*/, "", path); gsub(/-/, "_", path); name[path] = f[1] }
    function add(m,    d, k, c) { if (m in keep) return; keep[m] = 1; c = split(dep[m], d, " "); for (k = 1; k <= c; k++) add(d[k]) }
    END { for (m in need) if (m in name) add(name[m]); else print "missing module " m > "/dev/stderr"
          for (m in keep) { x = m; print x } }' $KDIR/modules.dep > $D/modlist
while read -r m; do
    mkdir -p "$RT/lib/modules/$KVER/$(dirname $m)"
    gzip -dc "$KDIR/$m" > "$RT/lib/modules/$KVER/${m%.gz}"
done < $D/modlist
awk -F: 'NR==FNR { keep[$1] = 1; next } ($1 in keep) { gsub(/\.gz/, ""); print }' $D/modlist $KDIR/modules.dep > $RT/lib/modules/$KVER/modules.dep
sed 's/\.gz//' $KDIR/modules.builtin > $RT/lib/modules/$KVER/modules.builtin
echo $MODS | tr ' ' '\n' > $RT/etc/modules
(cd $RT && find . | cpio -o -H newc --owner 0:0 --quiet | gzip -9) > $D/initrd
rm -rf $RT $D/modlist
cp $D/pkg/boot/vmlinuz-lts $D/vmlinuz

# store: both payloads and the watchdog drivers, slot CONFIG1 is version 1
rm -f $STORE
$BUILD/mkstore --size $STORE_MB --slots 2 $STORE >/dev/null
$BUILD/host_fs $STORE mkdir EFI/RUNWAY/BLOBS
$BUILD/host_fs $STORE put EFI/RUNWAY/BLOBS/vmlinuz @$D/vmlinuz
$BUILD/host_fs $STORE put EFI/RUNWAY/BLOBS/initrd @$D/initrd
# watchdog drivers: blobs linked into the sealed WDT directory
for w in I6300ESB ITCO WDAT; do
    $BUILD/host_fs $STORE put EFI/RUNWAY/BLOBS/$w.efi @$BUILD/$w.EFI
done
HOST_FS_SYSTEM=1 $BUILD/host_fs $STORE mkdir EFI/RUNWAY/WDT
for w in I6300ESB ITCO WDAT; do
    HOST_FS_SYSTEM=1 $BUILD/host_fs $STORE link EFI/RUNWAY/BLOBS/$w.efi EFI/RUNWAY/WDT/$w.EFI
done
$BUILD/rwyctl --store $STORE install --name CONFIG1 --kernel blob:vmlinuz --initrd blob:initrd \
    --args "console=tty0 console=ttyS0,115200 demo.version=1"
$BUILD/rwyctl --store $STORE set BOOT-DELAY 3

# disk: one ESP with RUNWAY.EFI as the removable-media loader and the store
rm -f $OUT; dd if=/dev/zero of=$OUT bs=1M count=$((ESP_MB + 2)) status=none
printf 'label: gpt\nstart=%s, size=%sMiB, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="ESP"\n' $START $ESP_MB | sfdisk $OUT >/dev/null
# the ESP is made on its own file of the exact partition size, then copied in (mkfs.vfat --offset
# can make a filesystem larger than the partition, which Linux refuses to mount)
ESP=$D/esp.img
rm -f $ESP; dd if=/dev/zero of=$ESP bs=1M count=$ESP_MB status=none
mkfs.vfat -F 32 -n ESP $ESP >/dev/null
mmd -i $ESP ::EFI ::EFI/BOOT
mcopy -i $ESP $BUILD/RUNWAY.EFI ::EFI/BOOT/BOOTX64.EFI
mcopy -i $ESP $STORE ::RUNWAY.DAT
dd if=$ESP of=$OUT bs=512 seek=$START conv=notrunc status=none
rm -f $STORE $ESP
echo "image: $OUT"
