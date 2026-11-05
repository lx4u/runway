#!/bin/sh
# rwyctl update and install under QUOTA: the slots that make room go in their own commit, a refusal deletes
# nothing, and QUOTA works without SLOTS. Host only.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
CTL=$BUILD/rwyctl
T=$(mktemp -d /var/tmp/rwy-evict.XXXXXX)
trap 'rm -rf "$T"' EXIT
# The QUOTA percentages below count on a store that is already about a quarter full.
head -c 17825792 /dev/zero >"$T/ballast"
S=$T/s
fail() { echo "EVICT FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }
newk() { head -c 2097152 /dev/urandom >"$T/k"; }   # a payload that is not stored yet
ctl() { "$CTL" --store "$S" "$@"; }
code() { ctl "$@" >"$T/out" 2>"$T/err" </dev/null; echo $?; }
slots() { ctl status | awk '$1 ~ /^CONFIG[0-9]+$/ {printf "%s ", $1}'; }

"$BUILD/mkstore" --size 64 --add BALLAST="$T/ballast" "$S" >/dev/null || fail mkstore
ctl init || fail init
head -c 2097152 /dev/urandom >"$T/k"
want "$(newk; code update --kernel "$T/k")" 6 "update with neither SLOTS nor QUOTA"
grep -q "neither SLOTS nor QUOTA" "$T/err" || fail "refusal does not name SLOTS and QUOTA"

# QUOTA alone: seven slots fit, QUOTA 60 leaves room for all of them
ctl set QUOTA 60 >/dev/null || fail "set QUOTA"
for i in 1 2 3 4 5 6 7; do
    want "$(newk; code update --kernel "$T/k")" 0 "update $i"
    ctl confirm CONFIG$i >/dev/null || fail "confirm $i"
done
want "$(slots)" "CONFIG1 CONFIG2 CONFIG3 CONFIG4 CONFIG5 CONFIG6 CONFIG7 " "slots before"

# Too small a quota: the protected slots cannot go, nothing is deleted
ctl set QUOTA 30 >/dev/null
want "$(newk; code update --kernel "$T/k")" 7 "update when nothing fits"
want "$(slots)" "CONFIG1 CONFIG2 CONFIG3 CONFIG4 CONFIG5 CONFIG6 CONFIG7 " "slots after a refusal"
want "$(newk; code install --kernel "$T/k")" 7 "install when nothing fits"

# A source that cannot be read costs no slot
ctl set QUOTA 52 >/dev/null
want "$(code update --kernel "$T/none")" 1 "update with a missing source"
want "$(slots)" "CONFIG1 CONFIG2 CONFIG3 CONFIG4 CONFIG5 CONFIG6 CONFIG7 " "slots after a bad source"

# update evicts the oldest OK slot, the install follows in a new commit
want "$(newk; code update --kernel "$T/k")" 0 "update with eviction"
want "$(slots)" "CONFIG2 CONFIG3 CONFIG4 CONFIG5 CONFIG6 CONFIG7 CONFIG8 " "slots after update"
ctl log show | grep -q "EVICT .*CONFIG1.*reason=quota" || fail "no EVICT record for CONFIG1"
ctl log show | sed -n 2p | grep -q " INSTALL " || fail "the install is not an INSTALL event"

ctl status | grep -q "^LAST-EVICT=CONFIG1 quota$" || fail "status does not show the last eviction"
ctl status | grep -q "^USED=[0-9]*% (" || fail "status does not show USED"

# install evicts too; the pending CONFIG8 is kept
ctl confirm CONFIG8 >/dev/null || fail "confirm 8"
want "$(newk; code install --kernel "$T/k")" 0 "install with eviction"
want "$(slots)" "CONFIG3 CONFIG4 CONFIG5 CONFIG6 CONFIG7 CONFIG8 CONFIG9 " "slots after install"

# SLOTS is a cap: install at it evicts to stay within it
ctl set SLOTS 7 >/dev/null
want "$(newk; code install --kernel "$T/k")" 0 "install at the SLOTS cap"
want "$(slots | wc -w)" 7 "the count stays at SLOTS"
"$BUILD/fsck.runway" "$S" >"$T/check" 2>&1 || { cat "$T/check" >&2; fail "fsck.runway"; }

# Shared blobs: slots that share a payload cost only their own blocks, and evicting them frees little
S=$T/shared
"$BUILD/mkstore" --size 64 --add BALLAST="$T/ballast" "$S" >/dev/null || fail "mkstore shared"
ctl init || fail "init shared"
ctl set QUOTA 40 >/dev/null
want "$(code update --dedup --kernel "$T/k")" 0 "first slot"
ctl confirm CONFIG1 >/dev/null || fail "confirm 1"
shared=$(sha256sum "$T/k" | cut -d' ' -f1)
for i in 2 3 4 5 6 7; do
    want "$(code update --dedup --kernel blob:$shared)" 0 "update $i from the blob"
    ctl confirm CONFIG$i >/dev/null || fail "confirm $i"
done
want "$(code update --dedup --kernel "$T/k")" 0 "update 8 from the same file"
ctl confirm CONFIG8 >/dev/null || fail "confirm 8"
ctl log show | grep -q " EVICT " && fail "slots that share a blob were evicted"
head -c 2097152 /dev/urandom >"$T/k2"
ctl set QUOTA 33 >/dev/null
want "$(code update --kernel "$T/k2")" 7 "a new payload when the old slots free almost nothing"
want "$(slots)" "CONFIG1 CONFIG2 CONFIG3 CONFIG4 CONFIG5 CONFIG6 CONFIG7 CONFIG8 " "slots after the refusal"
"$BUILD/fsck.runway" "$S" >"$T/check" 2>&1 || { cat "$T/check" >&2; fail "fsck.runway after shared"; }

# A store run up to QUOTA 95 with 3 MiB payloads: every update works and the store stays sound
S=$T/frag
"$BUILD/mkstore" --size 64 --add BALLAST="$T/ballast" "$S" >/dev/null || fail "mkstore frag"
ctl init || fail "init frag"
head -c 3145728 /dev/urandom >"$T/k3"
head -c 3670016 /dev/urandom >"$T/mid"
ctl set QUOTA 95 >/dev/null
for i in $(seq 1 18); do
    head -c 3145728 /dev/urandom >"$T/k3"
    want "$(code update --dedup --kernel "$T/k3")" 0 "update $i at QUOTA 95"
    ctl confirm "$(slots | awk '{print $NF}')" >/dev/null || fail "confirm $i"
done
"$BUILD/fsck.runway" "$S" >"$T/check" 2>&1 || { cat "$T/check" >&2; fail "fsck.runway after the fill"; }

# Holes in the store leave no run long enough and a payload lands split; a later install evicts and stays sound
for s in $(slots | tr ' ' '\n' | awk 'NR % 2 == 0'); do ctl remove "$s" >/dev/null || fail "remove $s"; done
run=$("$BUILD/fsck.runway" "$S" | sed -n 's/.*largest free run=\([0-9]*\) blocks.*/\1/p')
[ "$run" -lt 896 ] || fail "a free run of $run blocks holds the payload: nothing to defragment"
want "$(code install --dedup --kernel "$T/mid")" 0 "install that only fits split"
"$BUILD/fsck.runway" "$S" | grep -q "frag: 1 multi-extent objects" || fail "no split object to defragment"
cp --sparse=always "$S" "$T/holes"
ctl set QUOTA 66 >/dev/null
head -c 3670016 /dev/urandom >"$T/mid2"
want "$(code install --dedup --kernel "$T/mid2")" 0 "install that needs eviction and defragmentation"
ctl status | grep -q "^LAST-EVICT=CONFIG[0-9]* quota$" || fail "no eviction before the defragmentation"
"$BUILD/fsck.runway" "$S" >"$T/check" 2>&1 || { cat "$T/check" >&2; fail "fsck.runway after defrag"; }

# No run holds the payload even after the defragmentation: the install is split, not refused
S=$T/holes
head -c 10485760 /dev/urandom >"$T/big"
ctl set QUOTA 70 >/dev/null
want "$(code install --dedup --kernel "$T/big")" 0 "install that only fits split"
"$BUILD/fsck.runway" "$S" >"$T/check" 2>&1 || { cat "$T/check" >&2; fail "fsck.runway after a split install"; }
# The eviction frees the only blob a new slot would link: the plan is made again with the blob counted as new
S=$T/lost
"$BUILD/mkstore" --size 64 --add BALLAST="$T/ballast" "$S" >/dev/null || fail "mkstore lost"
ctl init || fail "init lost"
ctl set QUOTA 60 >/dev/null
for i in 1 2 3 4 5 6 7; do
    head -c 2097152 /dev/urandom >"$T/l$i"
    want "$(code update --dedup --kernel "$T/l$i")" 0 "fill $i"
    ctl confirm CONFIG$i >/dev/null || fail "confirm $i"
done
ctl set QUOTA 49 >/dev/null
want "$(code update --dedup --kernel "$T/l1")" 0 "update with the content of the slot that goes"
want "$(slots)" "CONFIG3 CONFIG4 CONFIG5 CONFIG6 CONFIG7 CONFIG8 " "slots after the blob was lost to the eviction"
want "$(ctl status | sed -n 's/^USED=\([0-9]*\)%.*/\1/p' | awk '{print ($1 <= 49)}')" 1 "USED stays within QUOTA"
want "$("$BUILD/host_fs" "$S" cat EFI/RUNWAY/CONFIG8/KERNEL | sha256sum)" "$(sha256sum <"$T/l1")" "the new slot payload"
"$BUILD/fsck.runway" "$S" >"$T/check" 2>&1 || { cat "$T/check" >&2; fail "fsck.runway after the lost blob"; }
echo "EVICT: PASS"
