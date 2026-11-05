#!/bin/sh
# tests/fuse/test_fuse_rwyctl_cases.sh — rwyctl on a mounted store: encrypted store, lagging mirror member, anchor on
# swtpm, umount while rwyctl holds the freeze, and several rwyctl at once. Skips without FUSE or python3.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
D=$(mktemp -d /var/tmp/rwy-fuse-rwyctl-cases.XXXXXX)
M=$D/mnt
export RWY_RUN_DIR=$D/run
CTL=$(cd "$BUILD" && pwd)/rwyctl
fail() { echo "FUSE-RWYCTL-CASES FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }
mounted=
BGPIDS=
cleanup() {
    cd /
    [ -n "$mounted" ] && fusermount3 -u "$M" 2>/dev/null
    [ -n "$BGPIDS" ] && kill -9 $BGPIDS 2>/dev/null
    [ -n "${TPID:-}" ] && kill "$TPID" 2>/dev/null
    rm -rf "$D"
}
trap cleanup EXIT

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-RWYCTL-CASES: SKIP (no FUSE)"; exit 0; }
command -v python3 >/dev/null || { echo "TEST-FUSE-RWYCTL-CASES: SKIP (no python3)"; exit 0; }
mkdir -p "$M" "$RWY_RUN_DIR"

waitfor() {
    i=0
    while ! "$@"; do
        i=$((i + 1)); [ $i -gt 100 ] && return 1
        sleep 0.1
    done
}
sock() { ls "$RWY_RUN_DIR"/*.sock 2>/dev/null | head -n 1; }
havesock() { [ -n "$(sock)" ]; }
nodaemon() { ! pgrep -f "rwy-fuse.* $M\$" >/dev/null; }
code() { "$@" >"$D/out" 2>"$D/err"; echo $?; }
# Mount with the given rwy-fuse options and store paths.
start() {
    "$BUILD/rwy-fuse" -s "$@" "$M" 2>"$D/err" || fail "mount: $(cat "$D/err")"
    mounted=1
    waitfor havesock || fail "no freeze socket"
}
stop() {
    cd /
    fusermount3 -u "$M" || fail "unmount"
    mounted=
    waitfor nodaemon || fail "daemon still running after unmount"
    waitfor test -z "$(sock)" || fail "socket left after unmount"
}
# The slot names and labels of a store, without the size lines.
slotlist() { "$CTL" "$@" status 2>&1 | grep -v '^USED='; }

head -c 200000 /dev/urandom >"$D/kernel"
head -c 100000 /dev/urandom >"$D/initrd"

# The same commands on a store that is mounted and on a copy that is not.
cmds() {
    "$CTL" "$@" install --kernel "$D/kernel" --initrd "$D/initrd" --args "root=/dev/vda" --label one --tries 1 &&
    "$CTL" "$@" confirm CONFIG1 &&
    "$CTL" "$@" install --kernel "$D/kernel" --label two &&
    "$CTL" "$@" set SLOTS 6 &&
    "$CTL" "$@" remove CONFIG2
}

# Encrypted store: the key comes from a data key file, then from RWY_KEY
"$BUILD/mkstore" --size 64 --encrypt --passphrase hunter2 --dek-out "$D/dek" "$D/enc" >/dev/null || fail "mkstore enc"
"$CTL" --store "$D/enc" --dek-file "$D/dek" init >/dev/null || fail "init enc"
cp "$D/enc" "$D/enc.off"
cmds --store "$D/enc.off" --dek-file "$D/dek" >"$D/run.out" 2>&1 || { cat "$D/run.out" >&2; fail "offline encrypted"; }
start --key hunter2 "$D/enc"
cmds --store "$D/enc" --dek-file "$D/dek" >"$D/run.out" 2>&1 || { cat "$D/run.out" >&2; fail "encrypted, key file"; }
want "$(cat "$M/EFI/RUNWAY/CONFIG1/LABEL")" one "label seen through the mount"
want "$("$CTL" --store "$D/enc" --dek-file "$D/dek" slots --label one)" CONFIG1 "slot found by label on the mounted store"
[ -e "$M/EFI/RUNWAY/CONFIG2" ] && fail "removed slot still visible"
RWY_KEY=hunter2 "$CTL" --store "$D/enc" confirm CONFIG1 >/dev/null 2>&1 || fail "encrypted, RWY_KEY"
want "$(slotlist --store "$D/enc" --dek-file "$D/dek")" "$(slotlist --store "$D/enc.off" --dek-file "$D/dek")" \
     "status differs from the unmounted store"
want "$(code "$CTL" --store "$D/enc" set SLOTS 7)" 4 "no key"
[ -n "$(sock)" ] && timeout -s KILL 2 cat "$M/EFI/RUNWAY/CONFIG1/LABEL" >/dev/null || fail "mount frozen after a refused key"
stop
want "$("$CTL" --store "$D/enc" --dek-file "$D/dek" get CONFIG1 LABEL)" one "label after unmount"

# Mirror with a lagging member: the same result as without the mount
"$BUILD/mkstore" --size 64 "$D/m0" >/dev/null || fail "mkstore mirror"
"$CTL" --store "$D/m0" init >/dev/null || fail "init mirror"
cp "$D/m0" "$D/m1"
"$BUILD/host_fs" "$D/m0" mkdir AHEAD
cp "$D/m0" "$D/o0"; cp "$D/m1" "$D/o1"
LAGSUM=$(sha256sum <"$D/m1")
U1=$("$BUILD/fsck.runway" "$D/m1" | sed -n 's/^slot [AB]: member uuid //p' | sort -u)
[ -n "$U1" ] || fail "second member not named"
cmds --store "$D/o0" --store "$D/o1" >"$D/run.out" 2>&1 || { cat "$D/run.out" >&2; fail "offline mirror"; }
start "$D/m0" "$D/m1"
cmds --store "$D/m0" --store "$D/m1" >"$D/run.out" 2>&1 || { cat "$D/run.out" >&2; fail "mirror, lagging member"; }
stop
# The member paths differ in the status text
mstatus() { "$CTL" --store "$1" --store "$2" status 2>&1 | grep -v '^USED=\|^MIRROR-MEMBER=\|warning: member'; }
want "$(mstatus "$D/m0" "$D/m1")" "$(mstatus "$D/o0" "$D/o1")" "mirror status"
for p in m o; do
    "$CTL" --store "$D/${p}0" --store "$D/${p}1" status 2>&1 | grep -q "warning: member .* lagging" || fail "${p}1 not seen as lagging"
done
# The mount counts the lagging member once; RPC commands use the mounted store without reopening it
nlag() { "$BUILD/host_fs" "$D/$1" health "$U1" | sed -n 's/^lagging=\([0-9]*\) .*/\1/p'; }
want "$(nlag m0)" 1 "lagging is counted once for the mount"
want "$(sha256sum <"$D/m1")" "$LAGSUM" "lagging member touched on the mounted store"
want "$(sha256sum <"$D/o1")" "$LAGSUM" "lagging member touched on the unmounted store"
for f in m0 m1; do "$BUILD/fsck.runway" "$D/$f" >/dev/null 2>&1 || fail "fsck of mirror member $f"; done
want "$("$CTL" --store "$D/m0" get CONFIG1 LABEL)" one "label on the first member"

# umount while rwyctl holds the freeze: the daemon ends and rwyctl does not hang
"$BUILD/mkstore" --size 64 "$D/u" >/dev/null || fail "mkstore u"
"$CTL" --store "$D/u" init >/dev/null || fail "init u"
mkfifo "$D/pipe"
start "$D/u"
"$CTL" --store "$D/u" put NOTE "$D/pipe" >"$D/p.out" 2>&1 &
P=$!
BGPIDS=$P
waitfor sh -c "! timeout -s KILL 1 cat '$M/NOTE' >/dev/null 2>&1" || fail "no freeze: $(cat "$D/p.out")"
stop
echo data >"$D/pipe"
i=0
while kill -0 $P 2>/dev/null; do
    i=$((i + 1)); [ $i -gt 100 ] && fail "rwyctl hangs after the umount"
    sleep 0.1
done
wait $P; rc=$?
BGPIDS=
[ $rc -le 5 ] || fail "rwyctl after the umount: exit $rc, $(cat "$D/p.out")"
grep -qi "segmentation\|abort" "$D/p.out" && fail "rwyctl crashed: $(cat "$D/p.out")"
"$BUILD/fsck.runway" "$D/u" >/dev/null 2>&1 || fail "store not clean after the umount"
if [ $rc -eq 0 ]; then
    want "$("$CTL" --store "$D/u" cat NOTE)" data "put that ended after the umount"
fi
start "$D/u"
echo again >"$M/AGAIN" || fail "remount after the umount"
stop

# Several rwyctl at once: they take turns and all apply
"$BUILD/mkstore" --size 64 "$D/c" >/dev/null || fail "mkstore c"
"$CTL" --store "$D/c" init >/dev/null || fail "init c"
"$CTL" --store "$D/c" set SLOTS 12 >/dev/null || fail "set SLOTS"
start "$D/c"
PIDS=
for n in 1 2 3 4 5 6; do
    "$CTL" --store "$D/c" install --kernel "$D/kernel" --label "par$n" >"$D/par$n.out" 2>&1 &
    PIDS="$PIDS $!"
done
BGPIDS=$PIDS
for p in $PIDS; do wait $p || fail "parallel rwyctl $p failed: $(cat "$D"/par*.out)"; done
BGPIDS=
for n in 1 2 3 4 5 6; do
    "$CTL" --store "$D/c" status 2>&1 | grep -q "par$n" || fail "slot par$n missing"
done
timeout -s KILL 2 cat "$M/EFI/RUNWAY/CONFIG1/LABEL" >/dev/null || fail "mount not thawed"
stop
"$BUILD/fsck.runway" "$D/c" >/dev/null 2>&1 || fail "store not clean after the parallel calls"

echo "FUSE-RWYCTL-CASES: PASS"
