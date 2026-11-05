#!/bin/sh
# Power cut at every write of an install that evicts a slot and defragments the store (suite soak).
# SPDX-License-Identifier: Apache-2.0
# After each cut the checker is clean, the newest known good slot still holds its payload, and a new slot, when
# there is one, is whole. EVICT_CUT_STEP=N tries every Nth write only. Host only.
set -u
BUILD=${BUILD:-build}
STEP=${EVICT_CUT_STEP:-1}
T=$(mktemp -d /var/tmp/rwy-evict-cut.XXXXXX)
trap 'rm -rf "$T"' EXIT
# The QUOTA percentages below count on a store that is already about a quarter full.
head -c 17825792 /dev/zero >"$T/ballast"
S=$T/base.store
W=$T/work.store
fail() { echo "EVICT-CUT FAIL: $*" >&2; exit 1; }
ctl() { _st=$1; shift; "$BUILD/rwyctl" --store "$_st" "$@"; }
hash_of() { "$BUILD/host_fs" "$1" cat "EFI/RUNWAY/$2/KERNEL" 2>/dev/null | sha256sum | cut -d' ' -f1; }

"$BUILD/mkstore" --size 64 --add BALLAST="$T/ballast" "$S" >/dev/null || fail mkstore
ctl "$S" init || fail init
head -c 3670016 /dev/urandom >"$T/new"
new=$(sha256sum "$T/new" | cut -d' ' -f1)
ctl "$S" set QUOTA 95 >/dev/null
for i in $(seq 1 18); do
    head -c 3145728 /dev/urandom >"$T/old"
    ctl "$S" update --dedup --kernel "$T/old" >/dev/null 2>&1 || fail "update $i"
    ctl "$S" confirm "$(ctl "$S" status | awk '/^CONFIG/ {n = $1} END {print n}')" >/dev/null || fail "confirm $i"
done
for s in $(ctl "$S" status | awk '/^CONFIG/ {print $1}' | awk 'NR % 2 == 0'); do ctl "$S" remove "$s" >/dev/null || fail "remove $s"; done
newest=$(ctl "$S" status | awk '/^CONFIG/ {n = $1} END {print n}')
head -c 3670016 /dev/urandom >"$T/pre"
ctl "$S" install --dedup --kernel "$T/pre" >/dev/null || fail "split install"
ctl "$S" set QUOTA 66 >/dev/null
old=$(hash_of "$S" "$newest")
known=$(for s in $(ctl "$S" status | awk '/^CONFIG/ {print $1}'); do echo "$s $(hash_of "$S" "$s")"; done)
"$BUILD/fsck.runway" "$S" | grep -q "frag: 1 multi-extent objects" || fail "the store is not fragmented"

n=0 cuts=0
while :; do
    cp --sparse=always "$S" "$W"
    env LD_PRELOAD="$BUILD/faultio.so" FAULTIO_CUT=.store:$n "$BUILD/rwyctl" --store "$W" install --dedup --kernel "$T/new" >/dev/null 2>"$T/err"
    rc=$?
    case $rc in
    137) cuts=$((cuts + 1)) ;;
    0)   break ;;
    *)   cat "$T/err" >&2; fail "install exited $rc at write $n" ;;
    esac
    "$BUILD/fsck.runway" "$W" >"$T/check" 2>&1 || { cat "$T/check" >&2; fail "fsck.runway after a cut at write $n"; }
    ctl "$W" status | grep -q '^INCOMPLETE=0$' || fail "an incomplete slot is visible after a cut at write $n"
    [ "$(hash_of "$W" "$newest")" = "$old" ] || fail "the newest known good slot changed after a cut at write $n"
    for s in $(ctl "$W" status | awk '/^CONFIG/ {print $1}'); do
        h=$(hash_of "$W" "$s")
        echo "$known" | grep -qx "$s $h" || [ "$h" = "$new" ] || fail "$s holds neither its payload nor the new one after a cut at write $n"
    done
    n=$((n + STEP))
done
[ "$cuts" -gt 0 ] || fail "no cut happened"
[ "$(ctl "$W" log show | grep -c ' EVICT ')" -gt 0 ] || fail "the install did not evict"
"$BUILD/fsck.runway" "$W" >"$T/check" 2>&1 || { cat "$T/check" >&2; fail "final fsck.runway"; }
echo "EVICT-CUT: PASS ($cuts cuts, step $STEP)"
