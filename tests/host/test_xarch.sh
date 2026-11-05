#!/bin/sh
# On-disk format across CPU architectures: the host tools for x86_64 ($BUILD) and for aarch64
# ($A64_BUILD, run by qemu-aarch64-static) make the same store and read each other's writes.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
[ -n "${A64_BUILD:-}" ] || { echo "SKIP: A64_BUILD is not set"; exit 77; }
command -v qemu-aarch64-static >/dev/null || { echo "SKIP: no qemu-aarch64-static"; exit 77; }
QEMU_LD_PREFIX=${QEMU_LD_PREFIX:-/usr/aarch64-linux-gnu}
export QEMU_LD_PREFIX
T=$(mktemp -d /var/tmp/rwy-xarch.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail() { echo "XARCH FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }   # got want what
A64() { tool=$1; shift; qemu-aarch64-static "$A64_BUILD/$tool" "$@"; }

U=11111111-2222-4333-8444-555555555555
for mode in plain encrypted; do
    opts="--size 64 --random-seed s1 --uuid $U --efi-boot $BUILD/CHAIN.EFI"   # same payload: each build has its own CHAIN.EFI
    key=; unset HOST_FS_PASSPHRASE
    [ $mode = encrypted ] && { opts="$opts --encrypt"; key="--key runway-test-passphrase"; export HOST_FS_PASSPHRASE=runway-test-passphrase; }

    $BUILD/mkstore $opts "$T/x.store" >/dev/null 2>&1 || fail "$mode: x86_64 mkstore"
    A64 mkstore $opts "$T/a.store" >/dev/null 2>&1 || fail "$mode: aarch64 mkstore"
    cmp -s "$T/x.store" "$T/a.store" || fail "$mode: the two architectures made different images"

    A64 host_fs "$T/x.store" put TEST/FROM_A64.TXT "written on aarch64" || fail "$mode: aarch64 write"
    $BUILD/host_fs "$T/a.store" put TEST/FROM_X64.TXT "written on x86_64" || fail "$mode: x86_64 write"
    want "$($BUILD/host_fs "$T/x.store" cat TEST/FROM_A64.TXT)" "written on aarch64" "$mode: x86_64 reads aarch64 data"
    want "$(A64 host_fs "$T/a.store" cat TEST/FROM_X64.TXT)" "written on x86_64" "$mode: aarch64 reads x86_64 data"
    $BUILD/fsck.runway $key "$T/x.store" >/dev/null 2>&1 || fail "$mode: x86_64 check of an aarch64 write"
    A64 fsck.runway $key "$T/x.store" >/dev/null 2>&1 || fail "$mode: aarch64 check of an aarch64 write"
    $BUILD/fsck.runway $key "$T/a.store" >/dev/null 2>&1 || fail "$mode: x86_64 check of an x86_64 write"
    A64 fsck.runway $key "$T/a.store" >/dev/null 2>&1 || fail "$mode: aarch64 check of an x86_64 write"
done
echo "XARCH OK"
