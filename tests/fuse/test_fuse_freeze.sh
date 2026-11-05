#!/bin/sh
# tests/fuse/test_fuse_freeze.sh — rwy-fuse runs rwyctl commands over its RPC socket
# without dropping store locks or stalling the mount.
# SPDX-License-Identifier: Apache-2.0
# Skips when FUSE cannot be mounted or python3 is missing.
set -u
BUILD=${BUILD:-build}
D=$(mktemp -d /var/tmp/rwy-fuse-rpc.XXXXXX)
M=$D/mnt
RUN=$D/run
export RWY_RUN_DIR=$RUN
fail() { echo "FUSE-RPC FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }
mounted=
cleanup() {
    [ -n "$mounted" ] && fusermount3 -u "$M" 2>/dev/null
    pkill -f "$D/" 2>/dev/null
    rm -rf "$D"
}
trap cleanup EXIT

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-FREEZE: SKIP (no FUSE)"; exit 0; }
command -v python3 >/dev/null || { echo "TEST-FUSE-FREEZE: SKIP (no python3)"; exit 0; }
mkdir -p "$M" "$RUN"

# lock_check FILE...: exit 0 when none is locked, 1 when one is.
cat >"$D/lock.py" <<'PY'
import fcntl, sys
for name in sys.argv[1:]:
    f = open(name, "r+b")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        sys.exit(1)
PY
lockfree() { python3 -I "$D/lock.py" "$@"; }
lockheld() { ! lockfree "$@"; }

waitfor() {
    i=0
    while ! "$@"; do
        i=$((i + 1)); [ $i -gt 100 ] && return 1
        sleep 0.1
    done
}
sock() { ls "$RUN"/*.sock 2>/dev/null | head -n 1; }
havesock() { [ -n "$(sock)" ]; }
nodaemon() { ! pgrep -f "rwy-fuse.* $M\$" >/dev/null; }

start() {
    "$BUILD/rwy-fuse" "$@" "$M" 2>"$D/err" || fail "mount: $(cat "$D/err")"
    mounted=1
    waitfor havesock || fail "no RPC socket"
}
stop() {
    fusermount3 -u "$M" || fail "unmount"
    mounted=
    waitfor nodaemon || fail "daemon still running after unmount"
}

"$BUILD/mkstore" --size 64 "$D/base" >/dev/null || fail mkstore
"$BUILD/rwyctl" --store "$D/base" init >/dev/null || fail init
head -c 2097152 /dev/urandom >"$D/k1"
head -c 2097152 /dev/urandom >"$D/k2"
cp "$D/base" "$D/s"
ST=$D/s

# Start rwy-fuse and check socket permissions and store lock
start "$ST"
want "$(stat -c %a "$(sock)")" 600 "socket mode"
lockheld "$ST" || fail "store not locked by the mount"
echo hello >"$M/HELLO"

# Execute rwyctl commands over RPC
"$BUILD/rwyctl" --store "$ST" status >"$D/status" || fail "status over RPC"
grep -q '^USED=' "$D/status" || fail "status missing USED"
lockheld "$ST" || fail "store lock dropped after status"

# Install kernel over RPC while reading/writing /boot
"$BUILD/rwyctl" --store "$ST" install --kernel "$D/k1" --label one >/dev/null || fail "install over RPC"
cmp "$D/k1" "$M/EFI/RUNWAY/CONFIG1/KERNEL" || fail "kernel mismatch after install"
want "$(cat "$M/HELLO")" hello "mount read works"
echo world >"$M/WORLD"
want "$(cat "$M/WORLD")" world "mount write works"
lockheld "$ST" || fail "store lock dropped after install"

# Disruptive commands are refused over RPC
"$BUILD/rwyctl" --store "$ST" init >"$D/init.out" 2>&1
want "$?" 5 "init refused over RPC"
grep -q "store busy" "$D/init.out" || fail "wrong message for init: $(cat "$D/init.out")"

"$BUILD/rwyctl" --store "$ST" check --repair >"$D/repair.out" 2>&1
want "$?" 5 "check --repair refused over RPC"
grep -q "store busy" "$D/repair.out" || fail "wrong message for repair: $(cat "$D/repair.out")"

# Concurrent FUSE I/O while running rwyctl commands
for i in 1 2 3 4 5; do
    echo "data $i" >"$M/DATA_$i"
    "$BUILD/rwyctl" --store "$ST" set BOOT-DELAY $i >/dev/null || fail "set BOOT-DELAY $i"
    want "$(cat "$M/DATA_$i")" "data $i" "concurrent mount data $i"
done
lockheld "$ST" || fail "store lock dropped during concurrent I/O"

# Unmount: daemon ends and socket is cleaned up
stop
[ -z "$(sock)" ] || fail "socket left after stop"
lockfree "$ST" || fail "store locked after unmount"

# Stale socket replacement
start "$ST"
S=$(sock)
pkill -9 -f "rwy-fuse.* $M\$"
waitfor nodaemon || fail "daemon not killed"
fusermount3 -u "$M" 2>/dev/null
mounted=
[ -S "$S" ] || fail "no stale socket to test with"
start "$ST"
"$BUILD/rwyctl" --store "$ST" status >/dev/null || fail "status on replaced socket"
stop
[ -z "$(sock)" ] || fail "socket left after stop"

# Encrypted store over RPC
"$BUILD/mkstore" --size 64 --encrypt --passphrase hunter2 "$D/enc" >/dev/null || fail "mkstore enc"
echo hunter2 >"$D/pass"
"$BUILD/rwyctl" --store "$D/enc" --passphrase-file "$D/pass" init >/dev/null || fail "init enc"
start --key hunter2 "$D/enc"
lockheld "$D/enc" || fail "encrypted store not locked"
"$BUILD/rwyctl" --store "$D/enc" --passphrase-file "$D/pass" install --kernel "$D/k1" --label enc1 >/dev/null || fail "enc install"
cmp "$D/k1" "$M/EFI/RUNWAY/CONFIG1/KERNEL" || fail "enc kernel mismatch"
lockheld "$D/enc" || fail "encrypted store lock dropped"
stop

# Two-member mirror over RPC
cp "$D/base" "$D/m1"
cp "$D/base" "$D/m2"
start "$D/m1" "$D/m2"
lockheld "$D/m1" || fail "m1 not locked"
lockheld "$D/m2" || fail "m2 not locked"
"$BUILD/rwyctl" --store "$D/m1" install --kernel "$D/k1" --label mir1 >/dev/null || fail "mirror install"
cmp "$D/k1" "$M/EFI/RUNWAY/CONFIG1/KERNEL" || fail "mirror kernel mismatch"
lockheld "$D/m1" || fail "m1 lock dropped"
lockheld "$D/m2" || fail "m2 lock dropped"
stop

"$BUILD/fsck.runway" "$D/s" >"$D/check" 2>&1 || { cat "$D/check" >&2; fail "fsck s"; }
"$BUILD/fsck.runway" "$D/m1" >"$D/check" 2>&1 || { cat "$D/check" >&2; fail "fsck m1"; }
"$BUILD/fsck.runway" "$D/m2" >"$D/check" 2>&1 || { cat "$D/check" >&2; fail "fsck m2"; }

echo "FUSE-FREEZE: PASS"
