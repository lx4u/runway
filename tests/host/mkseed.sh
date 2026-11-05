#!/bin/sh
# tests/host/mkseed.sh — Build the fixture store the self-tests read: a few small files and the PERF patterns.
# SPDX-License-Identifier: Apache-2.0
# Usage: mkseed.sh MKSTORE CHAIN_EFI OUTPUT
set -eu
mkstore=$1 chain=$2 out=$3
t=$(mktemp -d /var/tmp/rwy-mkseed.XXXXXX)
trap 'rm -rf "$t"' EXIT

printf 'Runway store -- transactional filesystem seed image\ngeneration: 1\nThis file lives inside RUNWAY.DAT, not on the FAT filesystem.\n' >"$t/README.TXT"
printf 'Hello from Runway!\n' >"$t/HELLO.TXT"
printf 'This is a placeholder for EFI/TEST/SAMPLE.EFI.\nA real UKI or boot loader would live at a path like this.\n' >"$t/SAMPLE.EFI"
# Byte i of a pattern file is (i + seed) mod 256.
for spec in 1:17 16:34 64:51; do
    perl -e '($m,$s)=@ARGV; $p=join "", map chr(($_+$s)&255), 0..255; print $p x ($m*4096)' "${spec%%:*}" "${spec##*:}" >"$t/READ${spec%%:*}M.BIN"
done

"$mkstore" --size 256 --efi-boot "$chain" \
    --add README.TXT="$t/README.TXT" \
    --add TEST/HELLO.TXT="$t/HELLO.TXT" \
    --add EFI/TEST/SAMPLE.EFI="$t/SAMPLE.EFI" \
    --add PERF/READ1M.BIN="$t/READ1M.BIN" \
    --add PERF/READ16M.BIN="$t/READ16M.BIN" \
    --add PERF/READ64M.BIN="$t/READ64M.BIN" \
    "$out"
