#!/bin/sh
# A store opened as a file, or as IMAGE@OFFSET[+LENGTH]: host_cat, host_fs, host_mutate,
# fsck.runway, rwyctl and rwy-fuse. Nothing outside the window may change.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
T=$(mktemp -d /var/tmp/rwy-hostdev.XXXXXX)
trap 'fusermount3 -u "$T/mnt" 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "HOSTDEV FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }
code() { "$@" >"$T/out" 2>"$T/err" </dev/null; echo $?; }

OFF=1048576
$BUILD/mkstore --size 64 "$T/plain.store" >/dev/null || fail mkstore
SZ=$(stat -c %s "$T/plain.store")
# Filler, the store, filler: 1 MiB before and 1 MiB after
head -c $OFF /dev/zero | tr '\0' '\377' >"$T/head"
head -c $OFF /dev/zero | tr '\0' '\252' >"$T/tail"
cat "$T/head" "$T/plain.store" "$T/tail" >"$T/disk.img"
W="$T/disk.img@$OFF+$SZ"
outside_ok() { cmp -n $OFF "$T/head" "$T/disk.img" && tail -c $OFF "$T/disk.img" | cmp - "$T/tail"; }

# Reads
$BUILD/host_fs "$T/plain.store" put /HELLO "hello window" || fail "put plain"
cat "$T/head" "$T/plain.store" "$T/tail" >"$T/disk.img"
want "$($BUILD/host_cat "$W" /HELLO)" "hello window" "host_cat window and length"
cat "$T/head" "$T/plain.store" >"$T/last.img"
want "$($BUILD/host_cat "$T/last.img@$OFF" /HELLO)" "hello window" "host_cat window to the end"
want "$(code $BUILD/host_cat "$T/disk.img@$OFF" /HELLO)" 1 "window to the end holds filler too"
want "$($BUILD/host_cat "$T/plain.store" /HELLO)" "hello window" "host_cat plain file"
want "$($BUILD/host_fs "$W" cat /HELLO)" "hello window" "host_fs cat"
want "$(code $BUILD/fsck.runway "$W")" 0 "fsck.runway window"
want "$(code $BUILD/fsck.runway "$T/last.img@$OFF")" 0 "fsck.runway window to the end"

# Writes stay inside the window
$BUILD/host_fs "$W" put /MORE "more data" || fail "host_fs put in window"
want "$($BUILD/host_cat "$W" /MORE)" "more data" "host_fs put read back"
$BUILD/host_mutate "$W" /MUT "mutated" >/dev/null || fail "host_mutate in window"
want "$($BUILD/host_cat "$W" /MUT)" "mutated" "host_mutate read back"
outside_ok || fail "a write left the window"
want "$(code $BUILD/fsck.runway "$W")" 0 "fsck.runway after writes"
want "$(code $BUILD/fsck.runway --repair "$W")" 0 "fsck.runway --repair in window"
outside_ok || fail "repair left the window"

# The same store extracted from the image is the same store
dd if="$T/disk.img" of="$T/cut.store" bs=$OFF skip=1 count=$((SZ / OFF)) 2>/dev/null
want "$($BUILD/host_cat "$T/cut.store" /MORE)" "more data" "extracted copy"

# rwyctl
want "$(code $BUILD/rwyctl --store "$W" status)" 0 "rwyctl status"
$BUILD/rwyctl --store "$W" init >/dev/null 2>&1 || fail "rwyctl init"
outside_ok || fail "rwyctl left the window"
want "$(code $BUILD/fsck.runway "$W")" 0 "fsck.runway after rwyctl init"
want "$($BUILD/host_cat "$W" /EFI/RUNWAY/VERSION)" 1 "rwyctl init wrote inside the window"

# Bad windows
want "$(code $BUILD/host_cat "$T/disk.img@$((OFF * 100))" /HELLO)" 2 "window past the end"
want "$(code $BUILD/host_cat "$T/disk.img@$OFF+$((SZ * 100))" /HELLO)" 2 "length past the end"
want "$(code $BUILD/host_cat "$T/disk.img@0+$SZ" /HELLO)" 1 "window on the filler"
want "$(code $BUILD/host_cat "$T/disk.img@$OFF+0" /HELLO)" 2 "zero length"
want "$(code $BUILD/host_cat "$T/disk.img@12x" /HELLO)" 2 "bad number"
# A short window cuts the store: it does not mount
want "$(code $BUILD/host_cat "$T/disk.img@$OFF+$((SZ / 2))" /HELLO)" 1 "half the store"

# A file name with an @ that exists is a file
cp "$T/plain.store" "$T/odd@name"
want "$($BUILD/host_cat "$T/odd@name" /HELLO)" "hello window" "existing file with @"

# mkstore makes a store inside a window and clears what was there
{ cat "$T/head"; head -c $SZ /dev/zero | tr '\0' '\377'; cat "$T/tail"; } >"$T/mk.img"
MK="$T/mk.img@$OFF+$SZ"
want "$(code $BUILD/mkstore --size 64 "$MK")" 0 "mkstore into a window"
want "$(code $BUILD/fsck.runway "$MK")" 0 "fsck.runway of a store made in a window"
want "$($BUILD/host_cat "$MK" /README.TXT | head -c 8)" "$($BUILD/host_cat "$T/plain.store" /README.TXT | head -c 8)" "mkstore window content"
cmp -n $OFF "$T/head" "$T/mk.img" || fail "mkstore wrote before the window"
tail -c $OFF "$T/mk.img" | cmp - "$T/tail" || fail "mkstore wrote after the window"
want "$(code $BUILD/mkstore --size 64 "$T/mk.img@$OFF+$((SZ / 2))")" 2 "mkstore window smaller than the store"

# rwy-fuse
if [ -e /dev/fuse ] && command -v fusermount3 >/dev/null; then
    mkdir "$T/mnt"
    if $BUILD/rwy-fuse -s -odefault_permissions "$W" "$T/mnt" 2>/dev/null; then
        sleep 1
        want "$(cat "$T/mnt/HELLO")" "hello window" "fuse read"
        if [ "$(id -u)" = 0 ]; then   # the root of a store is owned by root
            printf 'via fuse' >"$T/mnt/FUSED" || fail "fuse write"
            fusermount3 -u "$T/mnt"
            want "$($BUILD/host_cat "$W" /FUSED)" "via fuse" "fuse write read back"
        else
            fusermount3 -u "$T/mnt"
        fi
        outside_ok || fail "rwy-fuse left the window"
    else
        echo "HOSTDEV: fuse part skipped (cannot mount)"
    fi
fi

# A window larger than its store (a partition that was made larger): the store runs, the rest stays unused
EXTRA=$((2 * OFF))
head -c $EXTRA /dev/zero | tr '\0' '\125' >"$T/extra"
cp "$T/plain.store" "$T/big.store"
cat "$T/head" "$T/big.store" "$T/extra" "$T/tail" >"$T/big.img"
BW="$T/big.img@$OFF+$((SZ + EXTRA))"
want "$($BUILD/host_cat "$BW" /HELLO)" "hello window" "host_cat in a larger window"
want "$(code $BUILD/fsck.runway "$BW")" 0 "fsck.runway in a larger window"
has() { grep -qF -- "$2" "$1" || fail "missing '$2' in $1"; }
has "$T/out" "note: $EXTRA bytes unused after the store"
$BUILD/host_fs "$BW" put /BIG "written in a larger window" || fail "put in a larger window"
$BUILD/host_mutate "$BW" /BIGMUT "mutated" >/dev/null || fail "host_mutate in a larger window"
want "$(code $BUILD/fsck.runway --repair "$BW")" 0 "repair in a larger window"
want "$($BUILD/host_cat "$BW" /BIG)" "written in a larger window" "read back in a larger window"
cmp -n $OFF "$T/head" "$T/big.img" || fail "a write went before the larger window"
tail -c $OFF "$T/big.img" | cmp - "$T/tail" || fail "a write went after the larger window"
dd if="$T/big.img" bs=$OFF skip=$((1 + SZ / OFF)) count=2 2>/dev/null | cmp - "$T/extra" || fail "the unused space changed"
want "$(code $BUILD/host_cat "$T/big.img@$OFF+$((SZ - OFF))" /HELLO)" 1 "a window smaller than the store"
echo "HOSTDEV: PASS"
