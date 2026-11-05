#!/bin/sh
# tests/fuse/test_fuse_loop.sh — the rwy-fuse serve loop: daemon mode, foreground mode, unmount and
# SIGTERM end it cleanly, mount options still apply, and the kernel caches no names. Skips when FUSE cannot be mounted.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
D=$(mktemp -d /var/tmp/rwy-fuse-loop.XXXXXX)
ST=$D/f.store
M=$D/mnt
fail() { echo "FUSE-LOOP FAIL: $1" >&2; exit 1; }
PID=
cleanup() {
    [ -n "$PID" ] && kill $PID 2>/dev/null
    fusermount3 -u $M 2>/dev/null
    rm -rf $D
}
trap cleanup EXIT

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-LOOP: SKIP (no FUSE)"; exit 0; }
mkdir -p $M
cp $BUILD/RUNWAY.DAT $ST
$BUILD/host_fs $ST put HELLO hi >/dev/null || fail "seed"

# Wait up to 5 s for a condition given as arguments.
waitfor() {
    i=0
    while ! "$@"; do
        i=$((i + 1)); [ $i -gt 50 ] && return 1
        sleep 0.1
    done
}
mounted() { grep -q " $M fuse" /proc/mounts; }
unmounted() { ! mounted; }
nodaemon() { ! pgrep -f "rwy-fuse.* $M\$" >/dev/null; }

# Daemon mode: the command returns, the mount works, unmount ends the daemon.
$BUILD/rwy-fuse $ST $M || fail "daemon mount"
mounted || fail "daemon mount missing"
[ "$(cat $M/HELLO)" = hi ] || fail "daemon read"
fusermount3 -u $M || fail "daemon unmount"
waitfor nodaemon || fail "daemon still running after unmount"

# Foreground mode: unmount ends the process with 0.
$BUILD/rwy-fuse -f $ST $M &
PID=$!
waitfor mounted || fail "foreground mount missing"
fusermount3 -u $M || fail "foreground unmount"
wait $PID; rc=$?; PID=
[ $rc -eq 0 ] || fail "foreground exit code $rc after unmount"

# Foreground mode: SIGTERM unmounts and ends the process.
$BUILD/rwy-fuse -f $ST $M &
PID=$!
waitfor mounted || fail "second foreground mount missing"
kill -TERM $PID
wait $PID; rc=$?; PID=
[ $rc -eq 0 ] || fail "foreground exit code $rc after SIGTERM"
unmounted || fail "mount left behind after SIGTERM"

# Mount options still reach the kernel.
$BUILD/rwy-fuse -o default_permissions $ST $M || fail "option mount"
grep " $M fuse" /proc/mounts | grep -q default_permissions || fail "default_permissions not applied"
fusermount3 -u $M || fail "option unmount"
waitfor nodaemon || fail "daemon still running after option unmount"

# The kernel keeps no names: every stat of a name is a LOOKUP request to the daemon.
$BUILD/rwy-fuse -f -d $ST $M 2>$D/debug &
PID=$!
waitfor mounted || fail "debug mount missing"
for i in 1 2 3; do stat $M/HELLO >/dev/null || fail "stat"; done
fusermount3 -u $M || fail "debug unmount"
wait $PID; PID=
[ "$(grep -c '^LOOKUP /HELLO' $D/debug)" -eq 3 ] || fail "the kernel cached a name"

echo "TEST-FUSE-LOOP: PASS"
