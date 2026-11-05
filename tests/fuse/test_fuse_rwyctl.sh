#!/bin/sh
# tests/fuse/test_fuse_rwyctl.sh — rwyctl changes a store that rwy-fuse has mounted: it asks the daemon to freeze,
# runs its offline code and lets go. Skips when FUSE cannot be mounted or python3 is missing.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
D=$(mktemp -d /var/tmp/rwy-fuse-rwyctl.XXXXXX)
M=$D/mnt
RUN=$D/run
export RWY_RUN_DIR=$RUN
CTL=$(cd "$BUILD" && pwd)/rwyctl
fail() { echo "FUSE-RWYCTL FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }
mounted=
BGPIDS=
cleanup() {
    [ -n "$mounted" ] && fusermount3 -u "$M" 2>/dev/null
    [ -n "$BGPIDS" ] && kill -9 $BGPIDS 2>/dev/null
    rm -rf "$D"
}
trap cleanup EXIT

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-RWYCTL: SKIP (no FUSE)"; exit 0; }
command -v python3 >/dev/null || { echo "TEST-FUSE-RWYCTL: SKIP (no python3)"; exit 0; }
mkdir -p "$M" "$RUN"

cat >"$D/lock.py" <<'PY'
import fcntl, sys
for name in sys.argv[1:]:
    f = open(name, "r+b")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        sys.exit(1)
PY
# Hold the lock of a file until the release file exists.
cat >"$D/hold.py" <<'PY'
import fcntl, os, sys, time
f = open(sys.argv[1], "r+b")
fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
open(sys.argv[3], "w").write("held")
while not os.path.exists(sys.argv[2]):
    time.sleep(0.05)
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
# A request on the mount waits while the store is frozen. The killed cat leaves nothing open.
thawed() { timeout -s KILL 1 cat "$M/BEFORE" >/dev/null 2>&1; }
frozen() { ! thawed; }
code() { "$@" >"$D/out" 2>"$D/err"; echo $?; }
has() { grep -q "$2" "$1" || fail "'$2' not in $1: $(cat "$1")"; }

start() {
    "$BUILD/rwy-fuse" "$@" "$M" 2>"$D/err" || fail "mount: $(cat "$D/err")"
    mounted=1
    waitfor havesock || fail "no freeze socket"
}
stop() {
    fusermount3 -u "$M" || fail "unmount"
    mounted=
    waitfor nodaemon || fail "daemon still running after unmount"
}

"$BUILD/mkstore" --size 64 "$D/base" >/dev/null || fail mkstore
"$BUILD/rwyctl" --store "$D/base" init >/dev/null || fail init
head -c 300000 /dev/urandom >"$D/kernel"
head -c 100000 /dev/urandom >"$D/initrd"
echo "root=/dev/vda console=ttyS0" >"$D/args"
echo 8 >"$D/slots"
cp "$D/base" "$D/on"
cp "$D/base" "$D/off"

# The same commands, in a directory with relative names, on a mounted and on an unmounted store.
run_all() {
    store=$1
    (cd "$D" &&
    "$CTL" --store "$store" install --kernel kernel --initrd initrd --args @args --label snap --tries 1 &&
    "$CTL" --store "$store" confirm CONFIG1 &&
    "$CTL" --store "$store" install --kernel kernel --label second &&
    "$CTL" --store "$store" set SLOTS @slots &&
    "$CTL" --store "$store" remove CONFIG2 &&
    "$CTL" --store "$store" put NOTE kernel) >"$D/run.out" 2>&1
}
run_all off || { cat "$D/run.out" >&2; fail "commands on the unmounted store"; }
start "$D/on"
echo seen >"$M/BEFORE"
run_all on || { cat "$D/run.out" >&2; fail "commands on the mounted store"; }
lockheld "$D/on" || fail "store not locked again after the commands"
"$CTL" -v --store "$D/on" set SLOTS @"$D/slots" 2>"$D/err" || fail "verbose set: $(cat "$D/err")"
has "$D/err" "store mounted at $M\$"
grep -q "rwy-fuse:" /proc/self/mountinfo || fail "mount source does not carry the store UUID"
ls "$M/EFI/RUNWAY" | grep -q CONFIG2 && fail "removed slot still visible"
want "$(cat "$M/EFI/RUNWAY/CONFIG1/LABEL")" snap "label seen through the mount"
want "$(cat "$M/EFI/RUNWAY/CONFIG1/STATE")" OK "state seen through the mount"
cmp "$D/kernel" "$M/NOTE" || fail "put through the mount"
want "$(cat "$M/BEFORE")" seen "file written before"
# USED differs: the mounted store also holds the files written through the mount.
"$CTL" --store "$D/on" status 2>&1 | grep -v '^USED=' >"$D/on.status"
"$CTL" --store "$D/off" status 2>&1 | grep -v '^USED=' >"$D/off.status"
cmp "$D/on.status" "$D/off.status" || fail "status differs from the unmounted store"
for f in KERNEL INITRD ARGS; do
    "$CTL" --store "$D/on" cat EFI/RUNWAY/CONFIG1/$f >"$D/on.$f"
    "$CTL" --store "$D/off" cat EFI/RUNWAY/CONFIG1/$f >"$D/off.$f"
    cmp "$D/on.$f" "$D/off.$f" || fail "$f differs from the unmounted store"
done
cmp "$D/kernel" "$D/on.KERNEL" || fail "kernel stored"

# -v says which way was taken
cd "$D" && "$CTL" -v --store "$D/on" set SLOTS 7 >"$D/out" 2>"$D/err"; cd - >/dev/null
has "$D/err" "running command via rwy-fuse RPC"
want "$(cat "$M/BEFORE")" seen "mount serves reads while rwyctl runs"

# Concurrent rwyctl commands run without stalling /boot
"$CTL" --store "$D/on" set SLOTS 6 &
P1=$!
"$CTL" --store "$D/on" set SLOTS 8 &
P2=$!
wait $P1 || fail "first rwyctl failed"
wait $P2 || fail "second rwyctl failed"
want "$(cat "$M/BEFORE")" seen "mount serves reads"
lockheld "$D/on" || fail "store not locked after concurrent rwyctl"

# Long commands are refused and the mount keeps serving
for c in "check --repair" "init" "reencrypt"; do
    want "$(code "$CTL" --store "$D/on" $c)" 5 "$c on a mounted store"
    has "$D/err" "unmount it first"
    lockheld "$D/on" || fail "$c froze the store"
done
want "$(cat "$M/BEFORE")" seen "file after the refusals"

# A missing input ends before the freeze
want "$(code "$CTL" --store "$D/on" install --kernel "$D/nothing")" 1 "missing input"
lockheld "$D/on" || fail "a missing input froze the store"
stop

# Inputs that live on the mount are copied to a scratch file before the freeze
# A request on the mount waits while the store is frozen; the killed cat leaves nothing open.
thawed_ct9() { timeout -s KILL 1 cat "$M/boot-a" >/dev/null 2>&1; }
frozen_ct9() { ! thawed_ct9; }
mkdir "$D/scratch"
cp "$D/base" "$D/ct9"
start --freeze-wait 1 "$D/ct9"
cp "$D/kernel" "$M/boot-k"
cp "$D/initrd" "$M/boot-i"
ln -s "$M/boot-k" "$D/lk"
ln -s "$M/boot-i" "$D/li"
want "$(code env TMPDIR="$D/scratch" "$CTL" --store "$D/ct9" install --kernel "$D/lk" --initrd "$D/li" --label one)" 0 "install from the mount"
cmp "$D/kernel" "$M/EFI/RUNWAY/CONFIG1/KERNEL" || fail "kernel installed from the mount"
cmp "$D/initrd" "$M/EFI/RUNWAY/CONFIG1/INITRD" || fail "initrd installed from the mount"
lockheld "$D/ct9" || fail "store not locked again after the install from the mount"
"$CTL" --store "$D/ct9" set SLOTS 8 || fail "set SLOTS"
want "$(code env TMPDIR="$D/scratch" "$CTL" --store "$D/ct9" update --kernel "$D/lk" --label two)" 0 "update from the mount"
cmp "$D/kernel" "$M/EFI/RUNWAY/CONFIG2/KERNEL" || fail "kernel updated from the mount"
# A relative name and an @file value on the mount work too
echo "from the mount" >"$M/boot-a"
(cd "$M" && "$CTL" --store "$D/ct9" install --kernel boot-k --args @boot-a --label three) >"$D/out" 2>&1 ||
    fail "install with relative names on the mount: $(cat "$D/out")"
want "$(cat "$M/EFI/RUNWAY/CONFIG3/ARGS")" "from the mount" "@file on the mount"
want "$(ls -A "$D/scratch")" "" "scratch directory after the runs"

# The scratch directory is missing: the copy goes to memory
want "$(code env TMPDIR="$D/missing" "$CTL" --store "$D/ct9" update --kernel "$D/lk" --initrd "$D/li" --label four)" 0 "update with a missing TMPDIR"
cmp "$D/initrd" "$M/EFI/RUNWAY/CONFIG4/INITRD" || fail "initrd installed with a missing TMPDIR"

# A scratch write error ends the command before the freeze
want "$( (trap '' XFSZ; ulimit -f 10; code env TMPDIR="$D/scratch" "$CTL" --store "$D/ct9" put NOTE "$D/lk") )" 1 "scratch write error"
has "$D/err" "scratch copy"
lockheld "$D/ct9" || fail "a failed copy released the lock of the daemon"
thawed_ct9 || fail "a failed copy froze the mount"

# A large input, also killed in the middle of the copy: nothing stays in the scratch directory
head -c 20971520 /dev/urandom >"$M/big"
env TMPDIR="$D/scratch" "$CTL" --store "$D/ct9" update --kernel "$M/big" >"$D/p2.out" 2>&1 &
P2=$!
BGPIDS=$P2
sleep 0.05
kill -9 $P2; wait $P2 2>/dev/null
BGPIDS=
want "$(ls -A "$D/scratch")" "" "scratch directory after the kill"
waitfor thawed_ct9 || fail "no thaw after the kill"
want "$(code env TMPDIR="$D/scratch" "$CTL" --store "$D/ct9" update --kernel "$M/big")" 0 "20 MiB input on the mount"
n=$(ls "$M/EFI/RUNWAY" | grep -c CONFIG)
found=
for c in $(ls "$M/EFI/RUNWAY" | grep CONFIG); do cmp -s "$M/big" "$M/EFI/RUNWAY/$c/KERNEL" && found=$c; done
[ -n "$found" ] || fail "20 MiB kernel not found in a slot ($n slots)"

# A copy that takes longer than the wait for the freeze still gets the freeze
DPID=$(pgrep -f "rwy-fuse.* $M\$")
kill -STOP "$DPID"
env TMPDIR="$D/scratch" "$CTL" --store "$D/ct9" update --kernel "$M/boot-k" >"$D/p3.out" 2>&1 &
P3=$!
BGPIDS=$P3
sleep 12
kill -CONT "$DPID"
wait $P3 || fail "slow copy of an input: $(cat "$D/p3.out")"
BGPIDS=
stop

# A stale socket and no socket give "store busy"
start "$D/on"
S=$(sock)
SNAME=$(basename "$S")
stop
python3 -I - "$RUN/$SNAME" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX)
s.bind(sys.argv[1])
PY
[ -S "$RUN/$SNAME" ] || fail "no stale socket to test with"
python3 -I "$D/hold.py" "$D/on" "$D/hold.rel" "$D/hold.mark" &
HPID=$!
BGPIDS=$HPID
waitfor test -e "$D/hold.mark" || fail "no lock holder"
want "$(code "$CTL" --store "$D/on" set SLOTS 9)" 5 "stale socket"
has "$D/err" "busy"
rm -f "$RUN/$SNAME"
want "$(code "$CTL" --store "$D/on" set SLOTS 9)" 5 "no socket"
touch "$D/hold.rel"; wait $HPID
BGPIDS=

echo "FUSE-RWYCTL: PASS"
