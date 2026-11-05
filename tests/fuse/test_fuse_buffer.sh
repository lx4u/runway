#!/bin/sh
# tests/fuse/test_fuse_buffer.sh — write buffering in rwy-fuse:
# writes stay in the active transaction until fsync, close, or dirty threshold.
# SPDX-License-Identifier: Apache-2.0
# Skips when FUSE cannot be mounted or python3 is missing.
set -u
BUILD=${BUILD:-build}
D=$(mktemp -d /var/tmp/rwy-fuse-buffer.XXXXXX)
ST=$D/f.store
M=$D/mnt
fail() { echo "FUSE-BUFFER FAIL: $1" >&2; exit 1; }
trap 'fusermount3 -u $M 2>/dev/null; rm -rf $D' EXIT

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-BUFFER: SKIP (no FUSE)"; exit 0; }
command -v python3 >/dev/null || { echo "TEST-FUSE-BUFFER: SKIP (no python3)"; exit 0; }
mkdir -p "$M"

# Use RUNWAY.DAT as initial store template
if [ -f "$BUILD/tests/host/RUNWAY.DAT" ]; then
    cp "$BUILD/tests/host/RUNWAY.DAT" "$ST"
elif [ -f "$BUILD/RUNWAY.DAT" ]; then
    cp "$BUILD/RUNWAY.DAT" "$ST"
else
    fail "RUNWAY.DAT not found in build tree"
fi

RWYCTL_ROOT=$D $BUILD/rwy-fuse -s "$ST" "$M" || fail "mount"
sleep 1

cat <<'PY' > "$D/test_buffer.py"
import os, sys, struct

store_path = sys.argv[1]
mount_file = sys.argv[2]

def get_gen():
    with open(store_path, "rb") as f:
        data = f.read(8192)
    g1 = struct.unpack("<Q", data[32:40])[0]
    g2 = struct.unpack("<Q", data[4096+32:4096+40])[0]
    return max(g1, g2)

g_init = get_gen()

fd = os.open(mount_file, os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
# File creation commits the new inode
g_created = get_gen()

# Write several 64 KiB chunks without fsync
os.write(fd, b"A" * 65536)
os.write(fd, b"B" * 65536)
os.write(fd, b"C" * 65536)

# Writes should stay buffered in the open transaction
g_write = get_gen()
if g_write != g_created:
    print(f"FAIL: writes committed prematurely (g_write={g_write}, g_created={g_created})", file=sys.stderr)
    sys.exit(1)

# Calling fsync must commit the transaction
os.fsync(fd)
g_sync = get_gen()
if g_sync <= g_write:
    print(f"FAIL: fsync did not commit (g_sync={g_sync}, g_write={g_write})", file=sys.stderr)
    sys.exit(1)

# Write another chunk without fsync
os.write(fd, b"D" * 65536)
g_write2 = get_gen()
if g_write2 != g_sync:
    print(f"FAIL: write after fsync committed prematurely (g_write2={g_write2}, g_sync={g_sync})", file=sys.stderr)
    sys.exit(1)

# Closing the file must commit the remaining writes
os.close(fd)
g_close = get_gen()
if g_close <= g_write2:
    print(f"FAIL: close did not commit (g_close={g_close}, g_write2={g_write2})", file=sys.stderr)
    sys.exit(1)

# Verify entire file content is correct
with open(mount_file, "rb") as f:
    content = f.read()
expected = b"A" * 65536 + b"B" * 65536 + b"C" * 65536 + b"D" * 65536
if content != expected:
    print("FAIL: file content corrupted", file=sys.stderr)
    sys.exit(1)

# Read-after-write consistency while uncommitted
fd2 = os.open(mount_file + "_raw", os.O_CREAT | os.O_RDWR, 0o644)
os.write(fd2, b"UNCOMMITTED_DATA_CONSISTENCY")
os.lseek(fd2, 0, os.SEEK_SET)
readback = os.read(fd2, 28)
if readback != b"UNCOMMITTED_DATA_CONSISTENCY":
    print(f"FAIL: uncommitted readback mismatch: {readback}", file=sys.stderr)
    sys.exit(1)
os.close(fd2)

print("PASS: write buffering verified")
PY

python3 "$D/test_buffer.py" "$ST" "$M/BUFFER_TEST" || fail "python test failed"

echo "TEST-FUSE-BUFFER: PASS"
