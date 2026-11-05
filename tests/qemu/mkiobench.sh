#!/bin/sh
# tests/qemu/mkiobench.sh — make the benchmark images.
# SPDX-License-Identifier: Apache-2.0
# Usage:
#   mkiobench.sh [OUT] [APP]                       combined image (QEMU and USB-only)
#   mkiobench.sh --boot OUT APP RUNWAY KERNEL INITRD [DEK]
#   mkiobench.sh --target OUT [DEK]
# The combined image keeps the benchmark partitions on the boot ESP. The boot image has an
# ESP, a Runway slot store and the results store. The target image has no ESP: a plain store,
# the raw area, an optional encrypted store and the PERF/READY marker, written last.
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/../../tools/imglib.sh"
. "$(dirname "$0")/iobench_lib.sh"
iobench_tools
T=$(mktemp -d /var/tmp/rwy-iobench.XXXXXX)
trap 'rm -rf "$T"' EXIT

case "${1:-}" in
--boot)
    [ $# -ge 6 ] || { echo "usage: $0 --boot OUT APP RUNWAY KERNEL INITRD [DEK]" >&2; exit 2; }
    iobench_boot "$2" "$3" "$4" "$5" "$6" "${7:-}"
    made=$2
    ;;
--target)
    [ $# -ge 2 ] && [ $# -le 3 ] || { echo "usage: $0 --target OUT [DEK]" >&2; exit 2; }
    iobench_target "$2" "${3:-}"
    made=$2
    ;;
*)
    OUT="${1:-$BUILD/iobench.img}"
    APP="${2:-$BUILD/IOBENCH.EFI}"
    iobench_combined "$OUT" "$APP" "${DEK:-}"
    made=$OUT
    ;;
esac
echo "image created: $made"
