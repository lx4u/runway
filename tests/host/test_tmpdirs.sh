#!/bin/sh
# Host tests use a per-run scratch directory: no file is left behind and two runs can overlap.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
STORE=$BUILD/RUNWAY.DAT
CHECKER=$BUILD/fsck.runway
TESTS=${TMPDIRS_TESTS:-"mirrortest cryptotest xattr_link_test"}
T=$(mktemp -d /var/tmp/rwy-tmpdirs.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail() { echo "TMPDIRS FAIL: $*" >&2; exit 1; }

snap() { ls -d /tmp/rwy* /var/tmp/rwy* 2>/dev/null | grep -v "^$T\$" | sort; }

snap >"$T/before"
for t in $TESTS; do
    "$BUILD/$t" "$STORE" "$CHECKER" >"$T/$t.1" 2>&1 & p1=$!
    "$BUILD/$t" "$STORE" "$CHECKER" >"$T/$t.2" 2>&1 & p2=$!
    wait $p1 || fail "$t, first of two overlapping runs"
    wait $p2 || fail "$t, second of two overlapping runs"
done
snap >"$T/after"
left=$(comm -13 "$T/before" "$T/after")
[ -z "$left" ] || fail "files left behind: $left"
echo "TMPDIRS: PASS"
