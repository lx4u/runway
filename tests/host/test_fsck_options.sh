#!/bin/sh
# fsck.runway: the fsck(8) options and exit codes.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
T=$(mktemp -d /var/tmp/rwy-fsckopt.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail() { echo "FSCKOPT FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }
code() { "$@" >"$T/out" 2>"$T/err" </dev/null; echo $?; }
FSCK=$BUILD/fsck.runway
S=$T/RUNWAY.DAT

"$BUILD/mkstore" --size 64 "$S" >/dev/null || fail mkstore

want "$(code $FSCK "$S")" 0 "plain check"
for o in -n -f -a -p -y --repair --defrag; do
    want "$(code $FSCK $o "$S")" 0 "clean store with $o"
done
want "$(code $FSCK -f -n "$S")" 0 "-f with -n"
want "$(code $FSCK "$S" -n)" 0 "option after the store"

want "$(code $FSCK)" 16 "no store"
want "$(code $FSCK -h)" 16 "-h"
want "$(code $FSCK -z "$S")" 16 "unknown option"
want "$(code $FSCK "$S" "$S")" 16 "two stores"
want "$(code $FSCK -n -a "$S")" 16 "-n with -a"
want "$(code $FSCK -n -y "$S")" 16 "-n with -y"
want "$(code $FSCK --repair --defrag "$S")" 16 "--repair with --defrag"

want "$(code $FSCK "$T/missing")" 8 "missing store"

dd if=/dev/zero of="$S" bs=4096 count=2 conv=notrunc 2>/dev/null
want "$(code $FSCK "$S")" 4 "corrupt store"
want "$(code $FSCK -a "$S")" 4 "corrupt store is not repaired"
echo "FSCKOPT: PASS"
