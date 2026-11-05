#!/bin/sh
# Check size limit (< 150 KB) and symbols of RUNWAY.EFI stub.
set -eu

efi="$1"
so="$2"
stamp="${3:-}"

MAX_BYTES=153600
size=$(stat -c %s "$efi")
if [ "$size" -ge "$MAX_BYTES" ]; then
    echo "error: stub $efi size ($size bytes) exceeds 150 KB limit ($MAX_BYTES bytes)" >&2
    exit 1
fi

# Symbol audit: verify no AES, TPM, or filesystem modules are linked
forbidden="rwy_aes_ rwy_xts_ rwy_tpm_ tpm2_ rwy_inode_ rwy_alloc_ rwy_mount rwy_dir_ rwy_file_ rwy_fs_mount rwy_fs_path rwy_fs_info"
for sym in $forbidden; do
    if nm -D "$so" 2>/dev/null | grep -E "[[:space:]][A-Za-z][[:space:]].*${sym}"; then
        echo "error: forbidden symbol '$sym' found in stub $so" >&2
        exit 1
    fi
done

[ -n "$stamp" ] && : > "$stamp"
exit 0
