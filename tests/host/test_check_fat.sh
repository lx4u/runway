#!/bin/sh
# fsck.runway on a store file inside a FAT32 volume: repair needs clusters of 4096 bytes or more.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
PATH=$PATH:/sbin:/usr/sbin
T=$(mktemp -d /var/tmp/rwy-fsckfat.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail() { echo "CHECKFAT FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }
code() { "$@" >"$T/out" 2>"$T/err" </dev/null; echo $?; }

for t in mkfs.vfat mcopy; do
    command -v $t >/dev/null 2>&1 || { echo "CHECKFAT: SKIP ($t is missing)"; exit 0; }
done

$BUILD/mkstore --size 64 "$T/RUNWAY.DAT" >/dev/null || fail mkstore

# One image per cluster size: a FAT32 volume needs at least 65525 clusters
mkimg() {
    truncate -s "$2" "$T/$1.img"
    mkfs.vfat -F 32 -s "$3" "$T/$1.img" >/dev/null 2>&1 || fail "mkfs.vfat $1"
    mcopy -i "$T/$1.img" "$T/RUNWAY.DAT" ::RUNWAY.DAT || fail "mcopy $1"
}
mkimg small 150M 1
mkimg big 300M 8

want "$(code $BUILD/fsck.runway "$T/small.img")" 0 "read-only check with 512-byte clusters"
want "$(code $BUILD/fsck.runway --repair "$T/small.img")" 8 "repair with 512-byte clusters is refused"
grep -q "512-byte clusters" "$T/err" || fail "the refusal does not say why"
want "$(code $BUILD/fsck.runway "$T/big.img")" 0 "check with 4096-byte clusters"
want "$(code $BUILD/fsck.runway --repair "$T/big.img")" 0 "repair with 4096-byte clusters"
echo "CHECKFAT: PASS"
