#!/bin/sh
# tests/fuse/test_fuse_bridge_blobs.sh — kernel-install style writes through the mount: the commit of an entry names the
# payloads by their hash. The same kernel written twice is one blob. Skips when FUSE cannot be mounted.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
D=$(mktemp -d /var/tmp/rwy-fuse-bridge-blobs.XXXXXX)
ST=$D/f.store
M=$D/mnt
T=EFI/RUNWAY
B=$M/$T/BLOBS
fail() { echo "FUSE-BRIDGE-BLOBS FAIL: $1" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }
trap 'fusermount3 -u $M 2>/dev/null; rm -rf $D' EXIT

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-BRIDGE-BLOBS: SKIP (no FUSE)"; exit 0; }
mkdir -p $M
cp $BUILD/RUNWAY.DAT $ST
HOST_FS_SYSTEM=1 $BUILD/host_fs $ST mkdir $T
RWYCTL_ROOT=$D $BUILD/rwy-fuse -s $ST $M || fail "mount"
sleep 1

head -c 300000 /dev/urandom >$D/kernel
head -c 100000 /dev/urandom >$D/initrd
KH=$(sha256sum $D/kernel | cut -d' ' -f1)
IH=$(sha256sum $D/initrd | cut -d' ' -f1)

# kernel-install: install the payloads, write the entry under a temporary name, rename it into place
entry() {
    mkdir -p $M/tok/$1
    install -m 0644 $D/kernel $M/tok/$1/linux
    install -m 0644 $D/initrd $M/tok/$1/initrd
    printf 'title Tok %s\nversion %s\nlinux /tok/%s/linux\ninitrd /tok/%s/initrd\n' $1 $1 $1 $1 >$M/loader/entries/.tmp-$1.conf
    mv $M/loader/entries/.tmp-$1.conf $M/loader/entries/tok-$1.conf
}

entry v1
[ -e $B/$KH ] || fail "kernel has no content name"
[ -e $B/$IH ] || fail "initrd has no content name"
# the content name, the slot and the /boot file are three names of one object
want "$(stat -c %h $B/$KH)" 3 "names of the kernel after one entry"
want "$(stat -c %h $M/$T/CONFIG1/KERNEL)" 3 "slot links the kernel blob"
want "$(stat -c %h $M/tok/v1/linux)" 3 "the /boot file is the blob"

entry v2
want "$(ls $B | grep -cE '^[0-9a-f]{64}$')" 2 "one blob per content"
want "$(stat -c %h $B/$KH)" 5 "names of the shared kernel"
want "$(stat -c %h $M/tok/v2/linux)" 5 "the second /boot file is the blob"
cmp $M/tok/v2/linux $D/kernel || fail "kernel content through the mount"

# two initrd lines: the commit joins them and names the result by its hash
head -c 100000 /dev/urandom >$D/initrd2
JH=$(cat $D/initrd $D/initrd2 | sha256sum | cut -d' ' -f1)
mkdir -p $M/tok/j
install -m 0644 $D/kernel $M/tok/j/linux
install -m 0644 $D/initrd $M/tok/j/initrd
install -m 0644 $D/initrd2 $M/tok/j/initrd2
printf 'title Tok j\nversion j\nlinux /tok/j/linux\ninitrd /tok/j/initrd\ninitrd /tok/j/initrd2\n' >$M/loader/entries/tok-j.conf
[ -e $B/$JH ] || fail "joined initrd has no content name"
want "$(stat -c %h $M/$T/CONFIG3/INITRD)" 2 "slot links the joined blob"
want "$(ls $B | grep -cE '^[0-9a-f]{64}$')" 3 "kernel, initrd and joined initrd"
ls $B | grep -q '^stage.rwyctl$' && fail "staging file left by the join"

# a write in place to a frozen /boot file is refused, a rename over it is not
echo x >>$M/tok/v1/linux 2>/dev/null && fail "write in place to the shared kernel"
echo new >$M/tok/v1/linux.new && mv $M/tok/v1/linux.new $M/tok/v1/linux || fail "rename over the shared kernel"
want "$(stat -c %h $B/$KH)" 6 "the replaced /boot file gave up its name"

fusermount3 -u $M
sleep 0.5
$BUILD/fsck.runway $ST >$D/fsck.out 2>&1 || { cat $D/fsck.out; fail "fsck.runway"; }
$BUILD/rwyctl --store $ST check --blobs >$D/check.out 2>&1 || { cat $D/check.out; fail "check --blobs"; }
echo "TEST-FUSE-BRIDGE-BLOBS: PASS"
