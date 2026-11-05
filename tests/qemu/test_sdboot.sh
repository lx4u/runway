# tests/qemu/test_sdboot.sh — an unmodified systemd-boot boots a UKI from the Runway store.
# SPDX-License-Identifier: Apache-2.0
#   Disk: an ESP with RUNWAY.EFI and the store partition (XBOOTLDR type). The store holds the real
#   systemd-boot (EFI/BOOT/BOOT*.EFI) and a UKI (the test payload plus .osrel and .linux
#   sections) in EFI/Linux. RUNWAY.EFI chain-loads systemd-boot, which lists the UKI through the
#   Runway file system and starts it; the payload reports itself and powers off.
#   The store stays clean (fsck.runway).
set -e
export PATH="$PATH:/sbin:/usr/sbin"
BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
IMG=$BUILD/sdboot.img
LOG=$BUILD/sdboot.log
UKI=$BUILD/sdboot_uki.efi
fail() { echo "FAIL: $1"; [ -n "$2" ] && tail -n 60 "$2"; exit 1; }

sdboot_efi || fail "cannot obtain systemd-boot (apt-get download systemd-boot-efi:$QEMU_DEB_ARCH)"

# The payload becomes a UKI: the extra sections sit after the last section of the image
OBJCOPY=objcopy; [ "$ARCH" = x86_64 ] || OBJCOPY=$QEMU_MULTIARCH-objcopy
printf 'ID=runway-test\nPRETTY_NAME="Runway test UKI"\nVERSION_ID=1\n' > $BUILD/sdboot_osrel
printf 'MZ' > $BUILD/sdboot_linux
end=$(objdump -h $BUILD/SDBOOT_PAYLOAD.EFI | while read -r _ _ size vma _; do
    case $vma in *[!0-9a-f]*|'') continue;; esac
    echo $((0x$vma + 0x$size))
done | sort -n | tail -1)
vma=$(( (end + 0xfffff) / 0x100000 * 0x100000 ))
$OBJCOPY --add-section .osrel=$BUILD/sdboot_osrel --change-section-vma .osrel=$vma \
    --add-section .linux=$BUILD/sdboot_linux --change-section-vma .linux=$((vma + 0x1000)) \
    $BUILD/SDBOOT_PAYLOAD.EFI $UKI

BOOT_EFI=$SDBOOT_EFI ESP_ADD=EFI/Linux/runway-test.efi=$UKI tests/qemu/mkdisk_multivol.sh $IMG 0 0 >/dev/null

qemu_vars_copy
timeout 180 "$QEMU_BIN" -nographic $(qemu_args) \
    -drive format=raw,file=$IMG -serial stdio -display none > $LOG 2>&1 || true
grep -q "\[SDBOOT-PAYLOAD\] started" $LOG || fail "systemd-boot did not start the UKI" $LOG

store_get $IMG 0 $BUILD/sd_x.store
$BUILD/fsck.runway $BUILD/sd_x.store | grep -q "check: OK (0 errors, 0 warnings)" || fail "store not clean"

rm -f $BUILD/sd_x.store $BUILD/sdboot_osrel $BUILD/sdboot_linux $UKI
echo "TEST-SDBOOT: PASS"
