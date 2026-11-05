#!/bin/sh
# Build two ESP images (esp0.img and esp1.img) for Runway multi-disk mirroring.
# SPDX-License-Identifier: Apache-2.0
# Usage: tests/qemu/mkesp_mirror.sh [esp0.img] [esp1.img] [store0] [store1] [uuid0] [uuid1]
# The UUIDs are the PARTUUIDs of the partitions that hold the stores (the ESPs with STORE=file).
set -eu
BUILD=${BUILD:-build}

ESP0="${1:-esp0.img}"
ESP1="${2:-esp1.img}"
STORE0="${3:-$BUILD/RUNWAY.DAT}"
STORE1="${4:-$STORE0}"
UUID0="${5:-}"
UUID1="${6:-}"

echo "Building mirror member 0: $ESP0 (backing store: $STORE0)"
sh tools/mkesp.sh "$ESP0" "$STORE0" ${UUID0:+"$UUID0"}

echo "Building mirror member 1: $ESP1 (backing store: $STORE1)"
sh tools/mkesp.sh "$ESP1" "$STORE1" ${UUID1:+"$UUID1"}

echo "=== Mirror ESP disks provisioned: $ESP0 and $ESP1 ==="
