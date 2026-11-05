#!/bin/sh
# tests/fuse/test_fuse_rwyctl_anchor.sh — rwyctl changes an encrypted store with an enrolled anchor while rwy-fuse has
# it mounted. The daemon's anchor must agree with the counter after the thaw. With RWY_TEST_TPMRM=1 and a usable
# /dev/tpmrm0 it runs on the real TPM instead of swtpm (it enrolls a counter there). Skips without FUSE, swtpm or python3.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
D=$(mktemp -d /var/tmp/rwy-fuse-rwyctl-anchor.XXXXXX)
ST=$D/s
M=$D/mnt
export RWY_RUN_DIR=$D/run
CTL=$(cd "$BUILD" && pwd)/rwyctl
fail() { echo "FUSE-RWYCTL-ANCHOR FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }
mounted=
cleanup() {
    cd /
    [ -n "$mounted" ] && fusermount3 -u "$M" 2>/dev/null
    [ -n "${TPID:-}" ] && kill "$TPID" 2>/dev/null
    rm -rf "$D"
}
trap cleanup EXIT
# The QUOTA percentages below count on a store that is already about a quarter full.
head -c 17825792 /dev/zero >"$D/ballast"

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-RWYCTL-ANCHOR: SKIP (no FUSE)"; exit 0; }
command -v python3 >/dev/null || { echo "TEST-FUSE-RWYCTL-ANCHOR: SKIP (no python3)"; exit 0; }
if [ "${RWY_TEST_TPMRM:-}" = 1 ]; then
    REAL=1
    TPM=${RWY_TEST_TPM:-/dev/tpmrm0}
    [ -r "$TPM" ] && [ -w "$TPM" ] || { echo "TEST-FUSE-RWYCTL-ANCHOR: SKIP ($TPM not usable)"; exit 0; }
else
    REAL=
    command -v swtpm >/dev/null || { echo "TEST-FUSE-RWYCTL-ANCHOR: SKIP (no swtpm)"; exit 0; }
    TPM=$D/sock
fi
ctl() { "$CTL" --store "$ST" --passphrase-file "$D/pass" --tpm "$TPM" "$@"; }
anchor() { ctl anchor status 2>/dev/null | sed -n "s/^$1=//p"; }
mkdir -p "$M" "$D/tpm" "$RWY_RUN_DIR"
if [ -z "$REAL" ]; then
    swtpm socket --tpmstate dir="$D/tpm" --server type=unixio,path="$TPM" --ctrl type=unixio,path="$D/ctrl" \
          --tpm2 --flags not-need-init,startup-clear >"$D/tpm.log" 2>&1 &
    TPID=$!
    for i in $(seq 1 50); do [ -S "$TPM" ] && break; sleep 0.1; done
fi
echo hunter2 >"$D/pass"
"$BUILD/mkstore" --size 64 --add BALLAST="$D/ballast" --encrypt --passphrase hunter2 "$ST" >/dev/null || fail mkstore
ctl init || fail init
ctl anchor enroll >/dev/null || fail enroll
head -c 2097152 /dev/urandom >"$D/k"
for i in 1 2 3 4 5 6 7; do
    head -c 2097152 /dev/urandom >"$D/k"
    ctl install --kernel "$D/k" >/dev/null || fail "install $i"
    ctl confirm CONFIG$i >/dev/null || fail "confirm $i"
done
ctl set QUOTA 52 >/dev/null || fail "set QUOTA"
E0=$(anchor EPOCH)

waitfor() {
    i=0
    while ! "$@"; do
        i=$((i + 1)); [ $i -gt 100 ] && return 1
        sleep 0.1
    done
}
"$BUILD/rwy-fuse" -s --key hunter2 --tpm "$TPM" "$ST" "$M" 2>"$D/err" || fail "mount: $(cat "$D/err")"
mounted=1
waitfor test -n "$(ls "$RWY_RUN_DIR"/*.sock 2>/dev/null)" || fail "no freeze socket"
# With swtpm the daemon holds the only connection, so the anchor is read while mounted only on a real TPM
[ -z "$REAL" ] || want "$(anchor ANCHOR)" ok "anchor while mounted"

# rwyctl evicts an old slot through the freeze and raises the counter
head -c 2097152 /dev/urandom >"$D/k"
timeout 30 "$CTL" --store "$ST" --passphrase-file "$D/pass" --tpm "$TPM" install --kernel "$D/k" >"$D/out" 2>&1 \
    || { cat "$D/out" >&2; fail "install on the mounted store"; }
ls "$M/EFI/RUNWAY" | grep -q CONFIG1 && fail "evicted slot still visible"

# The daemon evicts too, with the counter that rwyctl raised
mkdir -p "$M/tok/x" "$M/loader/entries"
cp "$D/k" "$M/tok/x/linux" || fail "copy through the mount"
printf 'title x\nlinux /tok/x/linux\n' >"$M/loader/entries/x.conf" || fail "entry through the mount"
fusermount3 -u "$M" || fail "unmount"
mounted=
while pgrep -f "rwy-fuse.* $ST " >/dev/null; do sleep 0.2; done
[ "$(ctl log show | grep -c 'EVICT .*reason=quota')" -ge 2 ] || fail "want an eviction by rwyctl and one by the daemon"
[ "$(anchor EPOCH)" -ge $((E0 + 2)) ] || fail "the epoch did not rise twice"
want "$(anchor ANCHOR)" ok "counter and epoch agree"
echo "FUSE-RWYCTL-ANCHOR: PASS"
