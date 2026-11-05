#!/bin/sh
# tests/fuse/test_fuse_freeze_anchor.sh — rwyctl commands over RPC use the TPM anchor held by rwy-fuse.
# SPDX-License-Identifier: Apache-2.0
# swtpm serves one client at a time. Skips when FUSE cannot be mounted, swtpm or python3 is missing.
set -u
BUILD=${BUILD:-build}
TDIR=$(mktemp -d /var/tmp/rwy-fuse-freeze-anchor.XXXXXX)
ST=$TDIR/s
M=$TDIR/mnt
SOCK=$TDIR/sock
export RWY_RUN_DIR=$TDIR/run
fail() { echo "FUSE-FREEZE-ANCHOR FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }
ctl() { "$BUILD/rwyctl" --store "$ST" --passphrase-file "$TDIR/pass" --tpm "$SOCK" "$@"; }
slots() { ctl status 2>/dev/null | awk '$1 ~ /^CONFIG[0-9]+$/ {printf "%s ", $1}'; }
anchor() { ctl anchor status 2>/dev/null | sed -n "s/^$1=//p"; }
mounted=
cleanup() {
    [ -n "$mounted" ] && fusermount3 -u "$M" 2>/dev/null
    kill "${TPID:-}" "${LPID:-}" 2>/dev/null
    rm -rf "$TDIR"
}
trap cleanup EXIT
# The QUOTA percentages below count on a store that is already about a quarter full.
head -c 17825792 /dev/zero >"$TDIR/ballast"

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-FREEZE-ANCHOR: SKIP (no FUSE)"; exit 0; }
command -v swtpm >/dev/null || { echo "TEST-FUSE-FREEZE-ANCHOR: SKIP (no swtpm)"; exit 0; }
command -v python3 >/dev/null || { echo "TEST-FUSE-FREEZE-ANCHOR: SKIP (no python3)"; exit 0; }
mkdir -p "$M" "$TDIR/tpm" "$RWY_RUN_DIR"
swtpm socket --tpmstate dir="$TDIR/tpm" --server type=unixio,path="$SOCK" --ctrl type=unixio,path="$TDIR/ctrl" \
      --tpm2 --flags not-need-init,startup-clear >"$TDIR/tpm.log" 2>&1 &
TPID=$!
for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.1; done
echo hunter2 >"$TDIR/pass"
"$BUILD/mkstore" --size 64 --add BALLAST="$TDIR/ballast" --encrypt --passphrase hunter2 "$ST" >/dev/null || fail mkstore
ctl init || fail init
ctl anchor enroll >/dev/null || fail enroll
head -c 2097152 /dev/urandom >"$TDIR/k"
for i in 1 2 3 4 5 6 7; do
    head -c 2097152 /dev/urandom >"$TDIR/k"
    ctl install --kernel "$TDIR/k" >/dev/null || fail "install $i"
    ctl confirm CONFIG$i >/dev/null || fail "confirm $i"
done
ctl set QUOTA 52 >/dev/null || fail "set QUOTA"

write_entry() {
    mkdir -p "$M/tok/$1"
    cp "$TDIR/k" "$M/tok/$1/linux" 2>/dev/null || return 1
    printf 'title %s\nlinux /tok/%s/linux\n' "$1" "$1" >"$M/loader/entries/$1.conf" 2>/dev/null
}
waitfor() {
    i=0
    while ! "$@"; do
        i=$((i + 1)); [ $i -gt 100 ] && return 1
        sleep 0.1
    done
}
daemon_locked() { ! python3 -I -c 'import fcntl,sys; fcntl.flock(open(sys.argv[1], "r+b"), fcntl.LOCK_EX | fcntl.LOCK_NB)' "$ST" 2>/dev/null; }

E0=$(anchor EPOCH)
"$BUILD/rwy-fuse" -s --key hunter2 --tpm "$SOCK" "$ST" "$M" 2>"$TDIR/err" || fail "mount"
mounted=1
waitfor test -n "$(ls "$RWY_RUN_DIR"/*.sock 2>/dev/null)" || fail "no RPC socket"

# The daemon serves anchor commands over RPC without freezing
timeout 20 "$BUILD/rwyctl" --store "$ST" --passphrase-file "$TDIR/pass" --tpm "$SOCK" anchor status >"$TDIR/status" 2>&1 \
    || { cat "$TDIR/status" >&2; fail "anchor status over RPC"; }
grep -q '^EPOCH=' "$TDIR/status" || fail "no epoch in the status over RPC"
daemon_locked || fail "daemon dropped the store lock"

# The daemon keeps the anchor and an eviction raises the epoch
write_entry first || fail "first entry"
fusermount3 -u "$M" || fail "unmount"
mounted=
while pgrep -f "rwy-fuse.* $ST " >/dev/null; do sleep 0.2; done
ctl log show | grep -q "EVICT .*reason=quota" || fail "no EVICT record"
case "$(slots)" in *CONFIG1\ *) fail "CONFIG1 was not evicted" ;; esac
[ "$(anchor EPOCH)" -gt "$E0" ] || fail "the epoch did not rise"
want "$(anchor ANCHOR)" ok "counter and epoch agree"
echo "FUSE-FREEZE-ANCHOR: PASS"
