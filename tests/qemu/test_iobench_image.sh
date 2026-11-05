#!/bin/sh
# tests/qemu/test_iobench_image.sh — check the boot, target and combined benchmark images.
# SPDX-License-Identifier: Apache-2.0
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/../../tools/imglib.sh"
. "$(dirname "$0")/iobench_lib.sh"
iobench_tools
T=$(mktemp -d /var/tmp/rwy-iobench-image.XXXXXX)
trap 'rm -rf "$T"' EXIT

printf 'kernel\n' >"$T/kernel"
printf 'initrd\n' >"$T/initrd"
dd if=/dev/urandom of="$T/dek" bs=32 count=1 status=none
part_type() { sfdisk -d "$1" | awk -v n="$2" '/start=/ { c++; if (c == n) for (i = 1; i <= NF; i++) if ($i ~ /^type=/) { sub(/^type=/, "", $i); print $i } }'; }

IOBENCH_STORE_MB=16 IOBENCH_RAW_MB=8 IOBENCH_RESULT_MB=8
IOBENCH_BOOTSTORE_MB=32 IOBENCH_ESP_MB=300
export IOBENCH_STORE_MB IOBENCH_RAW_MB IOBENCH_RESULT_MB IOBENCH_BOOTSTORE_MB IOBENCH_ESP_MB

sh tests/qemu/mkiobench.sh --target "$T/target.img" "$T/dek"
[ "$(sfdisk --part-uuid "$T/target.img" 1 | tr a-f A-F)" = "$IOBENCH_PART_GUID" ]
[ "$(sfdisk --part-uuid "$T/target.img" 2 | tr a-f A-F)" = "$IOBENCH_ENC_GUID" ]
[ "$(part_type "$T/target.img" 3 | tr a-f A-F | tr -d ,)" = "$ESP_TYPE" ]
mdir -i "$T/target.img@@$(img_part_off "$T/target.img" 3)" :: | grep -q RWYBENCH
IOBENCH_NO_ESP=1 sh tests/qemu/mkiobench.sh --target "$T/noesp.img" "$T/dek" >/dev/null
[ "$(sfdisk -d "$T/noesp.img" | grep -c "$ESP_TYPE")" -eq 0 ]
off=$(img_part_off "$T/target.img" 1)
len=$(img_part_bytes "$T/target.img" 1)
"$RWYCTL" --store "$T/target.img@$off+$len" cat PERF/READY | grep -q "^layout=$IOBENCH_LAYOUT$"
"$RWYCTL" --store "$T/target.img@$off+$len" cat PERF/READY | grep -q "^disk_guid=$(sfdisk --disk-id "$T/target.img" | tr a-f A-F)$"

sh tests/qemu/mkiobench.sh --boot "$T/boot.img" "$BUILD/IOBENCH.EFI" "$BUILD/RUNWAY.EFI" "$T/kernel" "$T/initrd" "$T/dek"
[ "$(part_type "$T/boot.img" 1 | tr a-f A-F | tr -d ,)" = "$ESP_TYPE" ]
[ "$(sfdisk --part-uuid "$T/boot.img" 3 | tr a-f A-F)" = "$IOBENCH_RESULT_GUID" ]
esp=$((2048 * 512))
mdir -i "$T/boot.img@@$esp" ::/EFI/BOOT | grep -q BOOTX64
mdir -i "$T/boot.img@@$esp" :: | grep -q IOBENCH
mdir -i "$T/boot.img@@$esp" :: | grep -q RUNWAY
mtype -i "$T/boot.img@@$esp" ::/IOBENCH.CFG | grep -q '^rounds=5$'
mtype -i "$T/boot.img@@$esp" ::/IOBENCH.CFG | grep -q '^warmup=1$'
mtype -i "$T/boot.img@@$esp" ::/IOBENCH.CFG | grep -q '^devices=all$'
mtype -i "$T/boot.img@@$esp" ::/IOBENCH.CFG | grep -q '^disks=internal$'
mtype -i "$T/boot.img@@$esp" ::/IOBENCH.CFG | grep -q '^include_boot=0$'
mtype -i "$T/boot.img@@$esp" ::/IOBENCH.CFG | grep -q '^result=auto$'
bootstore="$T/boot.img@$(img_part_off "$T/boot.img" 2)+$(img_part_bytes "$T/boot.img" 2)"
"$RWYCTL" --store "$bootstore" status | grep -q CONFIG0
result="$T/boot.img@$(img_part_off "$T/boot.img" 3)+$(img_part_bytes "$T/boot.img" 3)"
printf 'schema\n' >"$T/row"
"$RWYCTL" --store "$result" put PERF/RUNS.CSV "$T/row"
"$RWYCTL" --store "$result" cat PERF/RUNS.CSV | grep -q schema

sh tests/qemu/mkiobench.sh "$T/combined.img" "$BUILD/IOBENCH.EFI"
[ "$(sfdisk --part-uuid "$T/combined.img" 2 | tr a-f A-F)" = "$IOBENCH_PART_GUID" ]
[ "$(sfdisk --part-uuid "$T/combined.img" 4 | tr a-f A-F)" = "$IOBENCH_RESULT_GUID" ]
esp=$((2048 * 512))
mtype -i "$T/combined.img@@$esp" ::/IOBENCH.CFG | grep -q '^rounds=1$'
mtype -i "$T/combined.img@@$esp" ::/IOBENCH.CFG | grep -q '^warmup=0$'
mtype -i "$T/combined.img@@$esp" ::/IOBENCH.CFG | grep -q '^disks=all$'
mtype -i "$T/combined.img@@$esp" ::/IOBENCH.CFG | grep -q '^include_boot=1$'
result="$T/combined.img@$(img_part_off "$T/combined.img" 4)+$(img_part_bytes "$T/combined.img" 4)"
printf 'combined\n' >"$T/row"
"$RWYCTL" --store "$result" put PERF/SAMPLES.CSV "$T/row"
"$RWYCTL" --store "$result" cat PERF/SAMPLES.CSV | grep -q combined

echo ">>> SUCCESS: boot, target and combined benchmark images were checked"
