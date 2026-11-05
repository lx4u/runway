#!/bin/sh
# tests/fuse/test_fuse_blobs_enc.sh — the content name rule, the install, the sweep of an orphan blob and check --blobs
# on a store with encryption that rwy-fuse has mounted with a key. Skips when FUSE cannot be mounted.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
D=$(mktemp -d /var/tmp/rwy-fuse-blobs-enc.XXXXXX)
ST=$D/s
M=$D/mnt
export RWY_RUN_DIR=$D/run
CTL="$(cd "$BUILD" && pwd)/rwyctl --store $ST --passphrase-file $D/pass"
BLOBS=$M/EFI/RUNWAY/BLOBS
fail() { echo "FUSE-BLOBS-ENC FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }
mounted=
cleanup() {
    cd /
    [ -n "$mounted" ] && fusermount3 -u "$M" 2>/dev/null
    rm -rf "$D"
}
trap cleanup EXIT

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-BLOBS-ENC: SKIP (no FUSE)"; exit 0; }
mkdir -p "$M" "$RWY_RUN_DIR"
echo hunter2 >"$D/pass"
code() { $CTL "$@" >"$D/out" 2>"$D/err"; echo $?; }
blobs() { ls "$BLOBS" 2>/dev/null | grep -c '^[0-9a-f]\{64\}$'; }
waitfor() {
    i=0
    while ! "$@"; do
        i=$((i + 1)); [ $i -gt 100 ] && return 1
        sleep 0.1
    done
}
havesock() { ls "$RWY_RUN_DIR"/*.sock >/dev/null 2>&1; }
nodaemon() { ! pgrep -f "rwy-fuse.* $M\$" >/dev/null; }
start() {
    "$BUILD/rwy-fuse" -s --key hunter2 "$ST" "$M" 2>"$D/err" || fail "mount: $(cat "$D/err")"
    mounted=1
    waitfor havesock || fail "no freeze socket"
}
stop() {
    fusermount3 -u "$M" || fail "unmount"
    mounted=
    waitfor nodaemon || fail "daemon still running after unmount"
}

"$BUILD/mkstore" --size 64 --encrypt --passphrase hunter2 "$ST" >/dev/null || fail mkstore
$CTL init >/dev/null || fail init
head -c 2000000 /dev/urandom >"$D/k"
head -c 1000000 /dev/urandom >"$D/i"
hash=$(sha256sum "$D/k" | cut -d' ' -f1)
start

# The content name rule holds through the mount: USER cannot make one, other names work.
mkdir "$BLOBS" || fail "mkdir BLOBS"
cp "$D/k" "$BLOBS/$hash" 2>/dev/null && fail "cp to a content name"
cp "$D/k" "$BLOBS/plain" || fail "cp to a plain name"
ln "$BLOBS/plain" "$BLOBS/$hash" 2>/dev/null && fail "ln to a content name"
mv "$BLOBS/plain" "$BLOBS/$hash" 2>/dev/null && fail "mv to a content name"
want "$(blobs)" 0 "no content name was made"
rm "$BLOBS/plain" || fail "rm of the plain name"

# An install writes the payloads through the mount and the freeze names them.
want "$(code install --dedup --kernel "$D/k" --initrd "$D/i" --label one)" 0 "install on the encrypted mount"
cmp "$D/k" "$M/EFI/RUNWAY/CONFIG1/KERNEL" || fail "kernel after the install"
cmp "$D/k" "$BLOBS/$hash" || fail "blob has the content name"
want "$(blobs)" 2 "two blobs"
want "$(code install --dedup --kernel "$D/k" --label two)" 0 "second install"
want "$(blobs)" 2 "the kernel is stored once"

# A USER rmdir of the last slot that links a blob sweeps the blob; a shared blob stays.
rmdir "$M/EFI/RUNWAY/CONFIG2" || fail "rmdir of the second slot"
want "$(blobs)" 2 "the kernel stays while CONFIG1 links it"
rmdir "$M/EFI/RUNWAY/CONFIG1" || fail "rmdir of the first slot"
want "$(blobs)" 0 "the blobs go with the last slot"

# check --blobs only reads, so it runs on the mount; a repair is refused there. Both pass unmounted.
want "$(code install --dedup --kernel "$D/k" --initrd "$D/i" --label three)" 0 "install again"
want "$(code check --blobs)" 0 "check --blobs on the mount"
grep -q "check: OK" "$D/out" || fail "check --blobs on the mount: $(cat "$D/out")"
want "$(code check --blobs --repair --unlock)" 5 "check --repair on the mount"
stop
want "$(code check --blobs)" 0 "check --blobs unmounted"
grep -q "check: OK" "$D/out" || fail "check --blobs: $(cat "$D/out")"
echo "FUSE-BLOBS-ENC: PASS"
