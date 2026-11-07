#!/bin/sh
# tests/fuse/test_fuse_bls.sh — Synthetic BLS interface read-only tree tests.
# SPDX-License-Identifier: Apache-2.0
# Skips when FUSE cannot be mounted.
set -e
BUILD=${BUILD:-build}
TDIR=$(mktemp -d /var/tmp/rwy-fuse-bls.XXXXXX)
ST=$TDIR/test.store
M=$TDIR/mnt
T=EFI/RUNWAY
fail() { echo "FAIL: $1"; exit 1; }

[ -e /dev/fuse ] && command -v fusermount3 >/dev/null || { echo "TEST-FUSE-BLS: SKIP (no FUSE)"; exit 0; }
mkdir -p "$M"
cp "$BUILD/RUNWAY.DAT" "$ST"
fs() { HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST" "$@"; }

fs mkdir $T

# Seed Type 1 slot
fs mkdir $T/CONFIG1
fs put $T/CONFIG1/KERNEL "kernel-bytes-1"
fs put $T/CONFIG1/INITRD "initrd-bytes-1"
fs put $T/CONFIG1/ENTRY "slot1.conf"
fs put $T/CONFIG1/LABEL "Slot One"
fs put $T/CONFIG1/TRIES "3"
fs put $T/CONFIG1/STATE "INSTALLED"

# Seed Type 2 UKI slot
fs mkdir $T/CONFIG2
STUB=$(find "$BUILD/../.." -name "kernel-stub*.efi" 2>/dev/null | head -n 1 || true)
if [ -n "$STUB" ] && [ -f "$STUB" ]; then
    fs put $T/CONFIG2/KERNEL "@$STUB"
else
    fs put $T/CONFIG2/KERNEL "MZfakeukiimagecontent"
fi
fs put $T/CONFIG2/ENTRY "slot2.efi"
fs put $T/CONFIG2/STATE "INSTALLED"
fs put $T/CONFIG2/STATE "OK"

"$BUILD/rwy-fuse" -s "$ST" "$M"
trap 'fusermount3 -u "$M" 2>/dev/null || true; [ -n "$SWTPM_PID" ] && kill -9 "$SWTPM_PID" 2>/dev/null || true; rm -rf "$TDIR"' EXIT
sleep 1

# Root and /EFI list each name once
[ "$(ls "$M" | grep -cx EFI)" = 1 ] || fail "EFI listed more than once in /"
[ "$(ls "$M/EFI" | grep -cx Linux)" = 1 ] || fail "Linux listed more than once in /EFI"

# Check /loader/entries.srel
[ -f "$M/loader/entries.srel" ] || fail "missing loader/entries.srel"
[ "$(cat "$M/loader/entries.srel")" = "type1" ] || fail "invalid entries.srel content"

# Check Type 1 entry in /loader/entries
ls "$M/loader/entries" | grep -qx "slot1+3.conf" || fail "slot1+3.conf not listed in loader/entries"
cat "$M/loader/entries/slot1+3.conf" | grep -qx "title Slot One" || fail "title line missing in slot1+3.conf"
cat "$M/loader/entries/slot1+3.conf" | grep -q "linux /slot1/linux" || fail "linux line missing in slot1+3.conf"
cat "$M/loader/entries/slot1+3.conf" | grep -q "initrd /slot1/initrd" || fail "initrd line missing in slot1+3.conf"

# Check Type 1 payload files
[ -f "$M/slot1/linux" ] || fail "missing /slot1/linux"
[ "$(cat "$M/slot1/linux")" = "kernel-bytes-1" ] || fail "invalid payload linux content"
[ -f "$M/slot1/initrd" ] || fail "missing /slot1/initrd"
[ "$(cat "$M/slot1/initrd")" = "initrd-bytes-1" ] || fail "invalid payload initrd content"

# Check Type 2 entry in /EFI/Linux
ls "$M/EFI/Linux" | grep -qx "slot2.efi" || fail "slot2.efi not listed in EFI/Linux"
[ "$(head -c 2 "$M/EFI/Linux/slot2.efi")" = "MZ" ] || fail "slot2.efi does not start with MZ"

# Check bootctl list if bootctl is present
if command -v bootctl >/dev/null 2>&1; then
    SYSTEMD_RELAX_XBOOTLDR_CHECKS=1 SYSTEMD_RELAX_ESP_CHECKS=1 bootctl --esp-path="$M" --boot-path="$M" list >"$TDIR/bootctl.out" 2>&1 || true
    grep -qi "slot1" "$TDIR/bootctl.out" || fail "bootctl list did not display slot1"
fi

# Writer matrix & staging tests

# 1. Definite faults rejected at flush
echo "Testing definite fault rejection at flush..."
if python3 -c "
with open('$M/loader/entries/bad1.conf', 'w') as f:
    f.write('linux /tok/../escape\n')
" 2>/dev/null; then
    fail "expected flush failure on path with dotdot"
fi
if python3 -c "
with open('$M/loader/entries/bad2.conf', 'w') as f:
    f.write('linux /tok/v1/linux\nlinux /tok/v1/linux\n')
" 2>/dev/null; then
    fail "expected flush failure on duplicate linux"
fi

# Prepare payload directory
mkdir -p "$M/testtok/v1"
echo "kernel-tok-v1" > "$M/testtok/v1/linux"
echo "initrd-tok-v1" > "$M/testtok/v1/initrd"

# 2. cp
printf "title CP Test\nversion 1.0\nlinux /testtok/v1/linux\ninitrd /testtok/v1/initrd\n" > "$TDIR/entry_cp.conf"
cp "$TDIR/entry_cp.conf" "$M/loader/entries/entry-cp.conf"
ls "$M/loader/entries" | grep -qx "entry-cp+3.conf" || fail "entry-cp+3.conf not found after cp"

# 3. install
install -m 0644 "$TDIR/entry_cp.conf" "$M/loader/entries/entry-inst+3.conf"
ls "$M/loader/entries" | grep -qx "entry-inst+3.conf" || fail "entry-inst+3.conf not found after install"

# 4. tee
cat "$TDIR/entry_cp.conf" | tee "$M/loader/entries/entry-tee.conf" >/dev/null
ls "$M/loader/entries" | grep -qx "entry-tee+3.conf" || fail "entry-tee+3.conf not found after tee"

# 5. dd bs=1
dd if="$TDIR/entry_cp.conf" of="$M/loader/entries/entry-dd.conf" bs=1 2>/dev/null
ls "$M/loader/entries" | grep -qx "entry-dd+3.conf" || fail "entry-dd+3.conf not found after dd"

# 6. python3 write and reopen
if command -v python3 >/dev/null 2>&1; then
    python3 -c "
with open('$M/loader/entries/entry-py.conf', 'w') as f:
    f.write('''title Python Test
version 1.0
linux /testtok/v1/linux
initrd /testtok/v1/initrd
''')
with open('$M/loader/entries/entry-py+3.conf', 'r') as f:
    data = f.read()
assert 'title Python Test' in data
" || fail "python write/reopen test failed"
fi

# 7. Group redirect with subshell
{
    echo "title Group Subshell"
    (echo "version 1.0")
    echo "linux /testtok/v1/linux"
    echo "initrd /testtok/v1/initrd"
} > "$M/loader/entries/entry-group.conf"
ls "$M/loader/entries" | grep -qx "entry-group+3.conf" || fail "entry-group+3.conf not found"

# 8. Group redirect with pipe
{
    echo "title Group Pipe"
    echo "version 1.0" | cat
    echo "linux /testtok/v1/linux"
    echo "initrd /testtok/v1/initrd"
} > "$M/loader/entries/entry-pipe.conf"
ls "$M/loader/entries" | grep -qx "entry-pipe+3.conf" || fail "entry-pipe+3.conf not found"

# 9. Child that outlives parent holding fd
sh -c '
    exec 3>"$1"
    printf "title Outlive\nversion 1.0\n" >&3
    ( sleep 0.3; printf "linux /testtok/v1/linux\ninitrd /testtok/v1/initrd\n" >&3; exec 3>&- ) &
' sh "$M/loader/entries/entry-outlive.conf"
sleep 0.6
ls "$M/loader/entries" | grep -qx "entry-outlive+3.conf" || fail "entry-outlive+3.conf not found after child exit"

# 10. Temporary file renamed into place
printf "title Renamed\nversion 1.0\nlinux /testtok/v1/linux\n" > "$M/loader/entries/.tmp-entry.conf"
mv "$M/loader/entries/.tmp-entry.conf" "$M/loader/entries/entry-renamed.conf"
ls "$M/loader/entries" | grep -qx "entry-renamed+3.conf" || fail "entry-renamed+3.conf not found after rename"

# 11. Type 2 staged UKI write
printf "MZfakeukiimagecontent" > "$M/EFI/Linux/entry-uki+2.efi"
ls "$M/EFI/Linux" | grep -qx "entry-uki+2.efi" || fail "entry-uki+2.efi not found after staged write"

# Preallocate / truncate then write and rename (systemd-sysupdate flow)
python3 -c "
import os
partial = '$M/EFI/Linux/.sysupdate.partial.entry-sysup+2.efi'
fd = os.open(partial, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o600)
os.ftruncate(fd, 1024)
os.write(fd, b'MZfakeukisysupdate')
fd2 = os.open(partial, os.O_RDONLY)
data = os.read(fd2, 18)
assert data == b'MZfakeukisysupdate'
os.close(fd2)
os.close(fd)
os.rename(partial, '$M/EFI/Linux/entry-sysup+2.efi')
" || fail "failed to truncate and write staged UKI"
ls "$M/EFI/Linux" | grep -qx "entry-sysup+2.efi" || fail "entry-sysup+2.efi not found after truncate write"
[ "$(stat -c %s "$M/EFI/Linux/entry-sysup+2.efi")" = 1024 ] || fail "entry-sysup+2.efi has wrong size after truncate"
[ "$(head -c 18 "$M/EFI/Linux/entry-sysup+2.efi")" = "MZfakeukisysupdate" ] || fail "entry-sysup+2.efi has wrong content after truncate"

# Truncate a payload blob
printf "0123456789" > "$M/testtok/v1/trunc"
truncate -s 4 "$M/testtok/v1/trunc" || fail "failed to truncate payload blob"
[ "$(cat "$M/testtok/v1/trunc")" = "0123" ] || fail "invalid payload content after truncate"

# 12. Bless rename good (Type 1)
mv "$M/loader/entries/entry-cp+3.conf" "$M/loader/entries/entry-cp.conf"
ls "$M/loader/entries" | grep -qx "entry-cp.conf" || fail "entry-cp.conf not found after good bless"
ls "$M/loader/entries" | grep -qx "entry-cp+3.conf" && fail "entry-cp+3.conf still listed after good bless"

# 13. Bless rename bad (Type 1)
mv "$M/loader/entries/entry-inst+3.conf" "$M/loader/entries/entry-inst+0.conf"
ls "$M/loader/entries" | grep -qx "entry-inst+0.conf" || fail "entry-inst+0.conf not found after bad bless"
ls "$M/loader/entries" | grep -qx "entry-inst+3.conf" && fail "entry-inst+3.conf still listed after bad bless"

# 14. Invalid renames refused with EPERM
mv "$M/loader/entries/entry-tee+3.conf" "$M/loader/entries/other.conf" 2>/dev/null && fail "rename with different stem must fail"
mv "$M/loader/entries/entry-tee+3.conf" "$M/loader/entries/entry-tee+1.conf" 2>/dev/null && fail "rename with indeterminate counter must fail"
mv "$M/loader/entries/entry-tee+3.conf" "$M/testtok/entry-tee.conf" 2>/dev/null && fail "rename out of entries dir must fail"

# 15. Type 2 bless rename good
mv "$M/EFI/Linux/entry-uki+2.efi" "$M/EFI/Linux/entry-uki.efi"
ls "$M/EFI/Linux" | grep -qx "entry-uki.efi" || fail "entry-uki.efi not found after good bless"
ls "$M/EFI/Linux" | grep -qx "entry-uki+2.efi" && fail "entry-uki+2.efi still listed after good bless"

# 16. Remove slot with unlink
rm "$M/loader/entries/entry-dd+3.conf"
ls "$M/loader/entries" | grep -qx "entry-dd+3.conf" && fail "entry-dd+3.conf still exists after rm"

# 17. systemd-bless-boot in private mount namespace with fake efivars
BLESS_BIN=""
if [ -x "/usr/lib/systemd/systemd-bless-boot" ]; then
    BLESS_BIN="/usr/lib/systemd/systemd-bless-boot"
elif command -v systemd-bless-boot >/dev/null 2>&1; then
    BLESS_BIN="$(command -v systemd-bless-boot)"
fi

if [ -n "$BLESS_BIN" ] && command -v unshare >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
    printf "title Bless Test\nversion 1.0\nlinux /testtok/v1/linux\n" > "$M/loader/entries/entry-bless+2.conf"
    ls "$M/loader/entries" | grep -qx "entry-bless+2.conf" || fail "entry-bless+2.conf not found before bless-boot"

    unshare -m -r python3 -c "
import os, subprocess
os.system('mount -t tmpfs none /sys/firmware/efi/efivars')
path = '/loader/entries/entry-bless+2.conf\0'
data = b'\x06\x00\x00\x00' + path.encode('utf-16le')
with open('/sys/firmware/efi/efivars/LoaderBootCountPath-4a67b082-0a4c-41cf-b6c7-440b29bb8c4f', 'wb') as f:
    f.write(data)
res = subprocess.run(['$BLESS_BIN', '--path=$M', 'good'], capture_output=True, text=True)
assert res.returncode == 0, f'bless good failed: {res.stderr}'
" || fail "systemd-bless-boot good test failed"

    ls "$M/loader/entries" | grep -qx "entry-bless.conf" || fail "entry-bless.conf not found after systemd-bless-boot good"
    ls "$M/loader/entries" | grep -qx "entry-bless+2.conf" && fail "entry-bless+2.conf still listed after systemd-bless-boot good"
fi

# 18. Unlink refused for last OK slot
# Remove other OK slots first (entry-uki.efi, slot2.efi, and entry-bless.conf)
rm -f "$M/EFI/Linux/entry-uki.efi"
rm -f "$M/EFI/Linux/slot2.efi"
rm -f "$M/loader/entries/entry-bless.conf"
# Now entry-cp.conf is the last OK slot: removing it must fail with EBUSY
rm "$M/loader/entries/entry-cp.conf" 2>/dev/null && fail "removing last OK slot must fail with EBUSY"
ls "$M/loader/entries" | grep -qx "entry-cp.conf" || fail "entry-cp.conf missing after refused removal"

fusermount3 -u "$M"
sleep 0.5

# 19. Encrypted stores: --key, RWY_KEY, credentials, key-file, and TPM unseal
ENC_STORE="$TDIR/enc.store"
DEK_FILE="$TDIR/testdek"
$BUILD/mkstore --encrypt --passphrase "secretpass" --dek-out "$DEK_FILE" "$ENC_STORE"
HOST_FS_SYSTEM=1 HOST_FS_PASSPHRASE="secretpass" "$BUILD/host_fs" "$ENC_STORE" mkdir EFI/RUNWAY
HOST_FS_SYSTEM=1 HOST_FS_PASSPHRASE="secretpass" "$BUILD/host_fs" "$ENC_STORE" mkdir EFI/RUNWAY/CONFIG1
HOST_FS_SYSTEM=1 HOST_FS_PASSPHRASE="secretpass" "$BUILD/host_fs" "$ENC_STORE" put EFI/RUNWAY/CONFIG1/KERNEL "enc-kernel-1"
HOST_FS_SYSTEM=1 HOST_FS_PASSPHRASE="secretpass" "$BUILD/host_fs" "$ENC_STORE" put EFI/RUNWAY/CONFIG1/ENTRY "enc1.conf"
HOST_FS_SYSTEM=1 HOST_FS_PASSPHRASE="secretpass" "$BUILD/host_fs" "$ENC_STORE" put EFI/RUNWAY/CONFIG1/STATE "INSTALLED"
HOST_FS_SYSTEM=1 HOST_FS_PASSPHRASE="secretpass" "$BUILD/host_fs" "$ENC_STORE" put EFI/RUNWAY/CONFIG1/STATE "OK"

# 19a. Mount without key fails
if "$BUILD/rwy-fuse" -s "$ENC_STORE" "$M" 2>/dev/null; then
    fusermount3 -u "$M" 2>/dev/null || true
    fail "expected mount failure for encrypted store without key"
fi

# 19b. Mount with --key
"$BUILD/rwy-fuse" -s --key "secretpass" "$ENC_STORE" "$M"
sleep 0.5
[ -f "$M/loader/entries.srel" ] || fail "missing entries.srel on encrypted store with --key"
ls "$M/loader/entries" | grep -qx "enc1.conf" || fail "enc1.conf missing with --key"
fusermount3 -u "$M"
sleep 0.5

# 19c. Mount with RWY_KEY
RWY_KEY="secretpass" "$BUILD/rwy-fuse" -s "$ENC_STORE" "$M"
sleep 0.5
[ -f "$M/loader/entries.srel" ] || fail "missing entries.srel on encrypted store with RWY_KEY"
ls "$M/loader/entries" | grep -qx "enc1.conf" || fail "enc1.conf missing with RWY_KEY"
fusermount3 -u "$M"
sleep 0.5

# 19d. Mount with systemd credentials (CREDENTIALS_DIRECTORY)
CREDS_DIR="$TDIR/creds"
mkdir -p "$CREDS_DIR"
printf "%s" "secretpass" > "$CREDS_DIR/runway.key"
CREDENTIALS_DIRECTORY="$CREDS_DIR" "$BUILD/rwy-fuse" -s "$ENC_STORE" "$M"
sleep 0.5
[ -f "$M/loader/entries.srel" ] || fail "missing entries.srel on encrypted store with CREDENTIALS_DIRECTORY"
ls "$M/loader/entries" | grep -qx "enc1.conf" || fail "enc1.conf missing with CREDENTIALS_DIRECTORY"
fusermount3 -u "$M"
sleep 0.5
rm -rf "$CREDS_DIR"

# 19e. Mount with --key-file
KEY_FILE="$TDIR/passfile"
printf "%s\n" "secretpass" > "$KEY_FILE"
"$BUILD/rwy-fuse" -s --key-file "$KEY_FILE" "$ENC_STORE" "$M"
sleep 0.5
[ -f "$M/loader/entries.srel" ] || fail "missing entries.srel on encrypted store with --key-file"
ls "$M/loader/entries" | grep -qx "enc1.conf" || fail "enc1.conf missing with --key-file"
fusermount3 -u "$M"
sleep 0.5
rm -f "$KEY_FILE"

# 19f. TPM unseal using swtpm (when swtpm is available)
if command -v swtpm >/dev/null 2>&1; then
    echo "Testing TPM unseal with swtpm..."
    TPM_STATE="$TDIR/swtpm"
    mkdir -p "$TPM_STATE"
    swtpm socket --tpmstate dir="$TPM_STATE" \
          --server type=unixio,path="$TPM_STATE/sock" \
          --ctrl type=unixio,path="$TPM_STATE/ctrl" \
          --tpm2 --flags not-need-init,startup-clear >"$TPM_STATE/log" 2>&1 &
    SWTPM_PID=$!
    for i in $(seq 1 50); do [ -S "$TPM_STATE/sock" ] && break; sleep 0.1; done

    # Enroll store with DEK sealed to PCR 7
    DEK_FILE="$TDIR/testdek"
    $BUILD/tpmtest "$TPM_STATE/sock" enroll "$DEK_FILE" "$ENC_STORE" || fail "tpmtest enroll failed"

    # Mount with rwy-fuse using TPM unseal (no passphrase provided!)
    "$BUILD/rwy-fuse" -s --tpm "$TPM_STATE/sock" "$ENC_STORE" "$M"
    sleep 0.5
    [ -f "$M/loader/entries.srel" ] || fail "missing entries.srel on TPM-unsealed store"
    ls "$M/loader/entries" | grep -qx "enc1.conf" || fail "enc1.conf missing on TPM-unsealed store"
    [ "$(cat "$M/enc1/linux")" = "enc-kernel-1" ] || fail "invalid kernel content from TPM-unsealed store"
    fusermount3 -u "$M"
    sleep 0.5

    # Also test with RWY_TPM environment variable
    RWY_TPM="$TPM_STATE/sock" "$BUILD/rwy-fuse" -s "$ENC_STORE" "$M"
    sleep 0.5
    [ -f "$M/loader/entries.srel" ] || fail "missing entries.srel with RWY_TPM"
    fusermount3 -u "$M"
    sleep 0.5

    # Changed PCR 7 fails hardware unseal and prevents mount
    $BUILD/tpmtest "$TPM_STATE/sock" extend || fail "tpmtest extend failed"
    if "$BUILD/rwy-fuse" -s --tpm "$TPM_STATE/sock" "$ENC_STORE" "$M" 2>/dev/null; then
        fusermount3 -u "$M" 2>/dev/null || true
        fail "expected mount failure after PCR 7 changed"
    fi

    kill "$SWTPM_PID" 2>/dev/null || true
    wait "$SWTPM_PID" 2>/dev/null || true
    SWTPM_PID=""
    rm -rf "$TPM_STATE" "$DEK_FILE"
fi

# 20. mount.runway helper with mirrored stores
echo "Testing mount.runway helper..."
MOUNT_RUNWAY="tools/mount.runway"
[ -x "$MOUNT_RUNWAY" ] || MOUNT_RUNWAY="$BUILD/../tools/mount.runway"
if [ -x "$MOUNT_RUNWAY" ]; then
    M1_STORE="$TDIR/m1.store"
    M2_STORE="$TDIR/m2.store"
    cp -f "$ST" "$M1_STORE"
    cp -f "$ST" "$M2_STORE"
    M_ALONE="$TDIR/mnt_alone"
    mkdir -p "$M_ALONE"

    RUNWAY_FUSE="$BUILD/rwy-fuse" "$MOUNT_RUNWAY" "$M1_STORE,$M2_STORE" "$M"
    sleep 0.5
    [ -f "$M/loader/entries.srel" ] || fail "mount.runway: entries.srel missing"

    # Write through the mirror mount
    mkdir -p "$M/mirtok/v1"
    echo "mirror-kernel" > "$M/mirtok/v1/linux"
    printf "title Mirror Test\nlinux /mirtok/v1/linux\n" > "$M/loader/entries/mirror-test.conf"
    sleep 0.5

    # Mounting one member alone while mirror is mounted is refused
    if RUNWAY_FUSE="$BUILD/rwy-fuse" "$MOUNT_RUNWAY" "$M1_STORE" "$M_ALONE" 2>/dev/null; then
        fusermount3 -u "$M_ALONE" 2>/dev/null || true
        fail "expected mount failure when mounting locked member alone"
    fi

    fusermount3 -u "$M"
    sleep 0.5

    # auto: the members come from rwyctl members
    printf '#!/bin/sh\nexec "%s/rwyctl" --store "%s" --store "%s" "$@"\n' "$BUILD" "$M1_STORE" "$M2_STORE" >"$TDIR/rwyctl-auto"
    printf '#!/bin/sh\nexit 3\n' >"$TDIR/rwyctl-none"
    chmod +x "$TDIR/rwyctl-auto" "$TDIR/rwyctl-none"
    RWYCTL="$TDIR/rwyctl-auto" RUNWAY_FUSE="$BUILD/rwy-fuse" "$MOUNT_RUNWAY" auto "$M"
    sleep 0.5
    [ -f "$M/mirtok/v1/linux" ] || fail "mount.runway auto: mirror content missing"
    fusermount3 -u "$M"
    sleep 0.5
    if RWYCTL="$TDIR/rwyctl-none" RUNWAY_FUSE="$BUILD/rwy-fuse" "$MOUNT_RUNWAY" auto "$M_ALONE" 2>/dev/null; then
        fusermount3 -u "$M_ALONE" 2>/dev/null || true
        fail "mount.runway auto: expected failure with no store"
    fi

    # Verify write landed on both stores
    SLOT=$("$BUILD/rwyctl" --store "$M1_STORE" status | awk '/mirror-test/ {print $1}')
    [ -n "$SLOT" ] || fail "mount.runway: mirror slot not found in store 1"
    [ "$(HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$M1_STORE" cat EFI/RUNWAY/$SLOT/KERNEL)" = "mirror-kernel" ] || fail "mount.runway: store 1 missing write"
    [ "$(HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$M2_STORE" cat EFI/RUNWAY/$SLOT/KERNEL)" = "mirror-kernel" ] || fail "mount.runway: store 2 missing write"


    # Mounting with a busy member is refused
    if command -v flock >/dev/null 2>&1; then
        flock -n "$M2_STORE" sleep 5 &
        FLOCK_PID=$!
        sleep 0.5
        if RUNWAY_FUSE="$BUILD/rwy-fuse" "$MOUNT_RUNWAY" "$M1_STORE,$M2_STORE" "$M" 2>/dev/null; then
            fusermount3 -u "$M" 2>/dev/null || true
            kill "$FLOCK_PID" 2>/dev/null || true
            fail "expected mount.runway failure with busy member"
        fi
        kill "$FLOCK_PID" 2>/dev/null || true
        wait "$FLOCK_PID" 2>/dev/null || true
    fi
    rm -rf "$M1_STORE" "$M2_STORE" "$M_ALONE"
fi

echo "TEST-FUSE-BLS: PASS"

