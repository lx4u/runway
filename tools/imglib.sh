# tools/imglib.sh — disk image layout shared by the image scripts and the QEMU suites. Source it.
#
# STORE says where a test image keeps its store:
#   block (default)  an XBOOTLDR partition of its own, no file system, read through EFI_BLOCK_IO
#   file             RUNWAY.DAT on a FAT partition
# Store K of a disk (0 is the boot store) sits on partition K+2 with STORE=block, as the ESP is partition 1,
# and on partition K+1 with STORE=file, where every store has a FAT partition.
#   mkfs_esp IMG START SIZE_KIB  format the ESP partition
#   img_part_off IMG N       byte offset of partition N
#   img_part_bytes IMG N     size of partition N in bytes
#   img_store_part K         partition number of store K
#   store_get IMG K OUT      copy store K out of the image
#   store_put IMG K IN       copy a store file into store K of the image
#   store_partuuid IMG K     PARTUUID of the partition of store K, upper case
# STORE_LOG and STORE_LOG_OTHER are the loader log text of the backend that runs and of the other one.
export PATH="$PATH:/sbin:/usr/sbin"
STORE=${STORE:-block}
case $STORE in
block | file) ;;
*) echo "STORE must be block or file, not '$STORE'" >&2; exit 1 ;;
esac
export STORE
[ "$STORE" = block ] && ESP_CLUSTER=1
# The loader says which backend runs; the suites check the line (STORE_LOG) and the absence of the other.
if [ "$STORE" = block ]; then
    STORE_LOG="store: partition, block backend" STORE_LOG_OTHER="file backend"
else
    STORE_LOG="store: RUNWAY.DAT, file backend" STORE_LOG_OTHER="block backend"
fi

ESP_TYPE=C12A7328-F81F-11D2-BA4B-00A0C93EC93B
XBL_TYPE=BC13C2FF-59E6-4262-A352-B275FD6F7172
DATA_TYPE=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
# The ESP of a block image holds RUNWAY.EFI twice and nothing else. Its size comes from FAT32: EDK2 only accepts
# a FAT32 volume with 65525 clusters or more, which takes 512-byte clusters and about 33 MiB.
BLOCK_ESP_MB=36

# mkfs_esp IMG START_SECTORS SIZE_KIB: format a partition as the FAT32 ESP
mkfs_esp() {
    mkfs.vfat --offset "$2" -F 32 ${ESP_CLUSTER:+-s $ESP_CLUSTER} -n ESP "$1" "$3" >/dev/null 2>&1
}

img_part_field() { sfdisk -d "$1" | awk -v n="$2" -v f="$3" '/start=/ {c++; if (c==n) {gsub(/[^0-9]/,"",$f); print $f}}'; }
img_part_off() { echo $(($(img_part_field "$1" "$2" 4) * 512)); }
img_part_bytes() { echo $(($(img_part_field "$1" "$2" 6) * 512)); }
img_store_part() { if [ "$STORE" = block ]; then echo $(($1 + 2)); else echo $(($1 + 1)); fi; }

store_get() {
    p=$(img_store_part "$2")
    if [ "$STORE" = block ]; then
        dd if="$1" of="$3" bs=1M iflag=skip_bytes,count_bytes skip="$(img_part_off "$1" "$p")" \
            count="$(img_part_bytes "$1" "$p")" status=none
    else
        mcopy -o -n -i "$1@@$(img_part_off "$1" "$p")" ::RUNWAY.DAT "$3"
    fi
}

store_put() {
    p=$(img_store_part "$2")
    if [ "$STORE" = block ]; then
        [ "$(stat -c %s "$3")" -le "$(img_part_bytes "$1" "$p")" ] || { echo "store_put: $3 does not fit" >&2; return 1; }
        dd if="$3" of="$1" bs=1M oflag=seek_bytes seek="$(img_part_off "$1" "$p")" conv=notrunc status=none
    else
        mcopy -o -i "$1@@$(img_part_off "$1" "$p")" "$3" ::RUNWAY.DAT
    fi
}

store_partuuid() { sfdisk --part-uuid "$1" "$(img_store_part "$2")" | tr a-f A-F; }
