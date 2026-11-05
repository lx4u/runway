#!/bin/sh
# Rotation soak: many update cycles over two slots of a sparse 512 MiB store, with large random
# payloads. Each slot is read back and compared by sha256 after every cycle. Some cycles share a
# payload through BLOBS/, some lose the power in the middle of the update. Host only.
# SPDX-License-Identifier: Apache-2.0
#   SOAK_CYCLES=300 SOAK_SEED=7 sh tests/host/test_rotation.sh   (a long run)
# With SOAK_QUOTA=PERCENT the store has QUOTA and no SLOTS: slots go by eviction, and a full store may refuse.
set -u
BUILD=${BUILD:-build}
CTL=$BUILD/rwyctl
HFS=$BUILD/host_fs
CYCLES=${SOAK_CYCLES:-20}
SEED=${SOAK_SEED:-1}
QUOTA=${SOAK_QUOTA:-}
RND=$SEED
T=$(mktemp -d /var/tmp/rwy-rotation.XXXXXX)
trap 'rm -rf "$T"' EXIT
S=$T/soak.store
n=0
fail() { echo "ROTATION FAIL (seed $SEED, cycle $n): $*" >&2; exit 1; }
pick() { RND=$(( (RND * 1103515245 + 12345) & 2147483647 )); R=$(( (RND >> 8) % $1 )); }   # R in 0..$1-1
ctl() { "$CTL" --store "$S" "$@"; }
slots() { ctl status | awk '$1 ~ /^CONFIG[0-9]+$/ {print $1}'; }
readback() { "$HFS" "$S" cat "EFI/RUNWAY/$1/KERNEL" | sha256sum | cut -d' ' -f1; }
pending() {
    for s in $(slots); do
        case $(ctl get "$s" STATE) in INSTALLED|TESTING) return 0 ;; esac
    done
    return 1
}

# A slot whose REVISION changed (or is unknown) is the new one and must hold $1; every other slot
# must still hold the payload recorded for it. Sets NEWCOUNT and NEWNAME.
settle() {
    NEWCOUNT=0 NEWNAME=
    ctl status | grep -q '^INCOMPLETE=0$' || fail "an incomplete slot is visible"
    for s in $(slots); do
        h=$(readback "$s"); r=$(ctl get "$s" REVISION)
        if [ -f "$T/r.$s" ] && [ "$r" = "$(cat "$T/r.$s")" ]; then
            [ "$h" = "$(cat "$T/h.$s")" ] || fail "$s changed its payload without a new revision"
            continue
        fi
        [ "$h" = "$1" ] || fail "$s has a new revision but not the new payload"
        echo "$h" >"$T/h.$s"; echo "$r" >"$T/r.$s"; NEWCOUNT=$((NEWCOUNT + 1)); NEWNAME=$s
    done
    for f in "$T"/h.*; do
        [ -f "$f" ] || continue
        slots | grep -qx "${f##*/h.}" && continue
        [ -n "$QUOTA" ] || fail "slot ${f##*/h.} vanished"
        rm -f "$f" "$T/r.${f##*/h.}"
    done
}

if [ -n "$QUOTA" ]; then
    "$BUILD/mkstore" --size 512 "$S" >/dev/null || fail mkstore
    ctl init && ctl set QUOTA "$QUOTA" >/dev/null || fail "set QUOTA"
else
    "$BUILD/mkstore" --size 512 --slots 2 "$S" >/dev/null || fail mkstore
fi
cuts=0 refused=0 shared=0

while [ "$n" -lt "$CYCLES" ]; do
    n=$((n + 1))
    pick 91; MB=$((30 + R))
    head -c $((MB * 1048576)) /dev/urandom >"$T/k"
    want=$(sha256sum "$T/k" | cut -d' ' -f1)
    src=$T/k blob=
    # Every fourth cycle after the slots are filled: reuse the payload of an existing slot
    pick 4
    if [ "$R" = 0 ] && [ "$n" -gt 2 ]; then
        old=$(slots | head -1)
        src=blob:$(cat "$T/h.$old") blob=1 want=$(cat "$T/h.$old"); shared=$((shared + 1))
    fi
    # A power cut in some cycles (never while the slots are filled): early or deep in the copy
    cut=
    pick 5
    if [ "$R" = 0 ] && [ "$n" -gt 2 ]; then
        pick 5; if [ "$R" -lt 2 ]; then pick 4; else pick $((MB * 2)); fi
        cut=$R
    fi

    gen=$(ctl stores | awk 'NR==2 {print $3}')
    before=$(ctl status; ctl stores)
    if [ -n "$cut" ]; then
        env LD_PRELOAD="$BUILD/faultio.so" FAULTIO_CUT=.store:$cut "$CTL" --store "$S" update --dedup --kernel "$src" >"$T/out" 2>"$T/err"
    else
        ctl update --dedup --kernel "$src" >"$T/out" 2>"$T/err"
    fi
    rc=$?
    case $rc in
    0)   settle "$want"; [ "$NEWCOUNT" = 1 ] || fail "update succeeded but $NEWCOUNT slots are new" ;;
    137) cuts=$((cuts + 1)); settle "$want"; [ "$NEWCOUNT" -le 1 ] || fail "$NEWCOUNT new slots after a power cut" ;;
    7)   [ -n "$QUOTA" ] || fail "update ran out of space without QUOTA"
         refused=$((refused + 1)); settle "$want"; [ "$NEWCOUNT" = 0 ] || fail "refused update changed a slot"
         [ "$(ctl status; ctl stores)" = "$before" ] || fail "refused update changed the store" ;;
    6)   pending || { cat "$T/err" >&2; fail "update refused with no pending slot"; }
         refused=$((refused + 1)); settle "$want"; [ "$NEWCOUNT" = 0 ] || fail "refused update changed a slot"
         [ "$(ctl status; ctl stores)" = "$before" ] || fail "refused update changed the store" ;;
    *)   cat "$T/err" >&2; "$BUILD/fsck.runway" "$S" >&2; ctl status >&2; ls -l "$T/k" >&2; fail "update exited $rc" ;;
    esac
    "$BUILD/fsck.runway" "$S" >"$T/check" 2>&1 || { cat "$T/check" >&2; fail "fsck.runway after the update"; }

    # Fate of the new slot, or the way out of a refusal
    if [ "$rc" = 6 ]; then
        for s in $(slots); do
            case $(ctl get "$s" STATE) in INSTALLED|TESTING) ctl confirm "$s" >/dev/null || fail "confirm $s" ;; esac
        done
    elif [ -n "$NEWNAME" ]; then
        pick 10
        case $R in
        [0-6]) ctl confirm "$NEWNAME" >/dev/null || fail "confirm $NEWNAME" ;;
        7)     ctl fail "$NEWNAME" >/dev/null || fail "fail $NEWNAME" ;;
        8)     ;;
        9)     HOST_FS_SYSTEM=1 "$HFS" "$S" put "EFI/RUNWAY/$NEWNAME/STATE" "TESTING
" || fail "TESTING $NEWNAME" ;;
        esac
    elif pending; then
        # a cut left the old slots as they were: clear any pending one so the next cycle can run
        for s in $(slots); do
            case $(ctl get "$s" STATE) in INSTALLED|TESTING) ctl confirm "$s" >/dev/null || fail "confirm $s" ;; esac
        done
    fi
    # The slot that survived a cycle keeps its payload; the generation never goes back
    now=$(ctl stores | awk 'NR==2 {print $3}')
    [ "$now" -ge "$gen" ] || fail "generation went back ($gen -> $now)"
done

"$BUILD/fsck.runway" "$S" 2>&1 | grep -E 'frag|OK|CORRUPT'
"$BUILD/fsck.runway" "$S" >/dev/null || fail "final fsck.runway"
[ -z "$QUOTA" ] || ctl log show | grep -q " EVICT " || fail "QUOTA never evicted a slot"
echo "ROTATION: PASS ($CYCLES cycles, seed $SEED, $shared shared payloads, $cuts power cuts, $refused refused)"
