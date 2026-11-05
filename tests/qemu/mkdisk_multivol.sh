# Build a GPT disk: an ESP and one store for the boot group, plus N auxiliary stores with distinct UUIDs
# (optional XBOOTLDR as last partition). With STORE=file (see tools/imglib.sh) every store is a
# RUNWAY.DAT on its own FAT partition; with STORE=block each store is a raw XBOOTLDR partition.
# SPDX-License-Identifier: Apache-2.0
# Usage: tests/qemu/mkdisk_multivol.sh [out.img] [aux_count] [xbootldr:0|1]
# Store n has UUID 00000000-0000-4000-8000-<n>. With MIRROR_AUX=1, aux
# store 2 reuses aux 1's UUID (a 2-member mirror of one logical volume).
# ESP_ADD=DEST=SRC adds one extra file to the boot store (see mkstore --add).
# SAME_AS_n=m gives aux n the UUID of aux m (generic mirror member).
# STORE_n env (n>=1) overrides the store image placed as aux store n.
# BOOT_EFI env overrides the app seeded at EFI/BOOT/$QEMU_BOOT_FILE in the boot store.
# ENCRYPT=1 makes every store AES-256-XTS encrypted (factory keyslot 0);
# each store gets its own DEK (seed "store-<n>", boot store = 0) unless MIRROR_AUX
# makes two stores members of one volume (they then share the DEK).
# ENCRYPT_SKIP="0 2" leaves those store indexes (0 = boot store) plaintext.
# Requires: sfdisk, dosfstools, mtools, python3. No root.
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
OUT="${1:-$BUILD/multivol.img}"
AUX="${2:-2}"
XBL="${3:-0}"
ESP_MB=330      # STORE=file: holds the 256 MiB boot store + binaries
AUX_MB=80       # STORE=file: holds a 64 MiB store
START=2048

# enc N -> mkstore encryption flags for store N (empty if plaintext)
enc() { [ -n "${ENCRYPT:-}" ] || return 0; case " ${ENCRYPT_SKIP:-} " in *" $1 "*) return 0;; esac; echo "--encrypt"; }

store_uuid() { printf '00000000-0000-4000-8000-%012d' "$1"; }

total_aux=$((AUX + XBL))

loader_arg() {
    [ -n "${NO_LOADER:-}" ] && return 0
    [ -f "${LOADER_EFI:-$BUILD/rwy-loader.efi}" ] && echo "--loader ${LOADER_EFI:-$BUILD/rwy-loader.efi}"
}

# The stores: 0 is the boot store (full default size), 1.. are the auxiliary ones
$BUILD/mkstore $(enc 0) --arch "$ARCH" --uuid "$(store_uuid 0)" --efi-boot "${BOOT_EFI:-$BUILD/CHAIN.EFI}" ${ESP_ADD:+--add "$ESP_ADD"} $(loader_arg) ${BOOT_STORE_EXTRA:-} $BUILD/mv_store0.store >/dev/null
n=1
while [ "$n" -le "$total_aux" ]; do
    u=$n; [ "${MIRROR_AUX:-0}" = 1 ] && [ "$n" = 2 ] && u=1
    $BUILD/mkstore $(enc "$n") --size 64 --uuid "$(store_uuid "$u")" $([ "$u" = 0 ] && loader_arg) $BUILD/mv_store$n.store >/dev/null
    eval "sa=\${SAME_AS_$n:-}"; [ -n "$sa" ] && u=$sa
    eval "ov=\${STORE_$n:-}"; [ -n "$ov" ] && cp -f "$ov" $BUILD/mv_store$n.store
    n=$((n+1))
done

# store_mb N -> MiB a partition needs for store N
store_mb() { echo $((($(stat -c %s $BUILD/mv_store$1.store) + 1048575) / 1048576)); }

script="label: gpt
"
if [ "$STORE" = block ]; then
    total=$((1 + BLOCK_ESP_MB + 1))
    script="${script}start=$START, size=${BLOCK_ESP_MB}MiB, type=$ESP_TYPE, name=\"ESP\"
"
    n=0
    while [ "$n" -le "$total_aux" ]; do
        script="${script}size=$(store_mb $n)MiB, type=$XBL_TYPE, name=\"STORE$n\"
"; total=$((total + $(store_mb $n))); n=$((n+1))
    done
else
    total=$((1 + ESP_MB + AUX * AUX_MB + XBL * AUX_MB + 2))
    script="${script}start=$START, size=${ESP_MB}MiB, type=$ESP_TYPE, name=\"ESP\"
"
    i=1
    while [ "$i" -le "$AUX" ]; do
        script="${script}size=${AUX_MB}MiB, type=$DATA_TYPE, name=\"CONFIG$i\"
"; i=$((i+1))
    done
    [ "$XBL" = 1 ] && script="${script}size=${AUX_MB}MiB, type=$XBL_TYPE, name=\"XBOOTLDR\"
"
fi
rm -f "$OUT"; truncate -s "${total}M" "$OUT"
printf '%s' "$script" | sfdisk "$OUT" >/dev/null

# fmt_aux IDX LABEL: format partition IDX as a FAT16 volume (STORE=file)
fmt_aux() {
    mkfs.vfat --offset "$(img_part_field "$OUT" "$1" 4)" -F 16 -n "$2" "$OUT" $(($(img_part_field "$OUT" "$1" 6) / 2)) >/dev/null 2>&1
}

mkfs_esp "$OUT" "$(img_part_field "$OUT" 1 4)" $(($(img_part_field "$OUT" 1 6) / 2))
b=$(img_part_off "$OUT" 1)
mmd -i "$OUT@@$b" ::EFI ::EFI/BOOT ::EFI/RUNWAY
mcopy -i "$OUT@@$b" $BUILD/RUNWAY.EFI ::EFI/RUNWAY/RUNWAY.EFI
mcopy -i "$OUT@@$b" $BUILD/RUNWAY.EFI ::EFI/BOOT/$QEMU_BOOT_FILE
n=0
while [ "$n" -le "$total_aux" ]; do
    if [ "$STORE" = file ] && [ "$n" -gt 0 ]; then
        if [ "$n" -gt "$AUX" ]; then lbl=XBOOTLDR; else lbl=CONFIG$n; fi
        fmt_aux $((n + 1)) "$lbl"
    fi
    store_put "$OUT" "$n" $BUILD/mv_store$n.store
    n=$((n+1))
done
rm -f $BUILD/mv_store*.store
echo "image: $OUT (1 ESP + $AUX aux + $XBL xbootldr, STORE=$STORE)"
