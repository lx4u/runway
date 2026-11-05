#!/bin/sh
# test_stub_audit.sh — audit the RUNWAY.EFI stub size and symbols
# SPDX-License-Identifier: Apache-2.0
set -eu

BUILD="${BUILD:-build}"
if [ -d "$BUILD/efi" ]; then
    EFI_DIR="$BUILD/efi"
elif [ -d "$(dirname "$BUILD")/../../efi" ]; then
    EFI_DIR="$(dirname "$BUILD")/../../efi"
elif [ -d "build/efi" ]; then
    EFI_DIR="build/efi"
else
    EFI_DIR="efi"
fi
STUB_EFI="$EFI_DIR/RUNWAY.EFI"
STUB_SO="$EFI_DIR/RUNWAY.so"
LOADER_EFI="$EFI_DIR/rwy-loader.efi"

fail() { echo "FAIL: $1" >&2; exit 1; }

[ -f "$STUB_EFI" ] || fail "$STUB_EFI missing"
[ -f "$STUB_SO" ] || fail "$STUB_SO missing"
[ -f "$LOADER_EFI" ] || fail "$LOADER_EFI missing"

# Check size limit: < 150 KB (153600 bytes)
MAX_SIZE=153600
size=$(stat -c %s "$STUB_EFI")
if [ "$size" -ge "$MAX_SIZE" ]; then
    fail "RUNWAY.EFI size ($size bytes) exceeds limit ($MAX_SIZE bytes)"
fi
echo "RUNWAY.EFI size is $size bytes (< $MAX_SIZE bytes)"

# Symbol audit: verify no AES, TPM, or filesystem symbols
forbidden="rwy_aes_ rwy_xts_ rwy_tpm_ tpm2_ rwy_inode_ rwy_alloc_ rwy_mount rwy_dir_ rwy_file_ rwy_fs_mount rwy_fs_path rwy_fs_info"
for sym in $forbidden; do
    if nm -D "$STUB_SO" 2>/dev/null | grep -E "[[:space:]][A-Za-z][[:space:]].*${sym}"; then
        fail "forbidden symbol pattern '$sym' found in stub"
    fi
done

echo "test_stub_audit: all checks passed"
exit 0
