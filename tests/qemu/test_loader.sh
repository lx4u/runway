#!/bin/sh
# tests/qemu/test_loader.sh — loader stub and loader area integration tests.
# SPDX-License-Identifier: Apache-2.0
#   1) normal boot through stub (copy A, confirmed good)
#   2) trial boot lifecycle: stage copy B with rwyctl, boot on trial, try decrement,
#      confirm by loader, second boot without trial
#   3) error return fallback: trial loader returning error immediately falls back
#      to good copy in the same boot session, marking trial copy bad with START_FAILED
#   4) reset loop and try exhaustion: trial loader that resets the system decrements
#      tries to 0, then stub falls back to good copy with TRIES_EXHAUSTED
#   5) watchdog hang recovery: trial loader that busy-loops is reset by hardware watchdog
#      (i6300esb), decrements tries, and falls back to good copy
#   6) mirror member reconciliation: trial boot on one mirror member reconciles state
#      record across all mirror members on confirm
#   7) Secure Boot snakeoil:
#      - unsigned trial child rejected with EFI_ACCESS_DENIED, falls back to signed good copy
#      - signed trial child succeeds and confirms
set -e
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"

IMG=$BUILD/loader.img
LOG=$BUILD/loader.log
ST=$BUILD/loader.store
VARS=$BUILD/loader_vars.fd
T=EFI/RUNWAY

fail() { echo "FAIL: $1" >&2; [ -n "${2:-}" ] && tail -n 60 "$2" >&2; exit 1; }

part_off() { img_part_off "$IMG" 1; }
fs() { HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST" "$@"; }
expect() { grep -qF -- "$1" "$LOG" || fail "missing expected: $1" "$LOG"; }
refuse() { grep -qF -- "$1" "$LOG" && fail "unexpected: $1" "$LOG"; return 0; }

newdisk() {
    tests/qemu/mkdisk_multivol.sh "$IMG" 0 0 >/dev/null
    store_get "$IMG" 0 "$ST"
    fs mkdir "$T"
    fs mkdir "$T/BLOBS"
    fs put "$T/BLOBS/uki.efi" @"$BUILD/NATIVE_PAYLOAD.EFI"
    fs mkdir "$T/CONFIG1"
    fs link "$T/BLOBS/uki.efi" "$T/CONFIG1/KERNEL"
    fs put "$T/CONFIG1/ARGS" confirm
    fs put "$T/CONFIG1/STATE" "INSTALLED
"
    fs put "$T/CONFIG1/STATE" "OK
"
    store_put "$IMG" 0 "$ST"
}

run() {
    qemu_vars_copy "$VARS"
    timeout "${TMO:-60}" "$QEMU_BIN" $(qemu_args "$VARS") \
        -drive format=raw,file="$IMG" -display none -serial stdio "$@" > "$LOG" 2>&1 || true
    store_get "$IMG" 0 "$ST"
}

echo "--- 1: normal boot through stub ---"
newdisk
run
expect "RUNWAY STUB"
expect "RUNWAY: booting CONFIG1"
expect "[NATIVE] started args=[confirm]"
"$BUILD/rwyctl" --store "$ST" loader show > "$BUILD/loader_show1.out"
grep -q "good A, trial none" "$BUILD/loader_show1.out" || fail "expected good A, trial none"
echo "1: PASS"

echo "--- 2: trial boot lifecycle (stage, decrement, confirm, second boot) ---"
"$BUILD/rwyctl" --store "$ST" loader set "$BUILD/rwy-loader.efi" --timeout 300 --tries 3 || fail "loader set"
"$BUILD/rwyctl" --store "$ST" loader show > "$BUILD/loader_show2.out"
grep -q "good A, trial B, tries 3" "$BUILD/loader_show2.out" || fail "trial B not staged"
store_put "$IMG" 0 "$ST"

run
expect "RUNWAY STUB"
expect "RUNWAY: booting CONFIG1"
expect "[NATIVE] started args=[confirm]"
"$BUILD/rwyctl" --store "$ST" loader show > "$BUILD/loader_show2_post.out"
grep -q "good B, trial none" "$BUILD/loader_show2_post.out" || fail "copy B not promoted to good"

run
expect "RUNWAY STUB"
expect "RUNWAY: booting CONFIG1"
expect "[NATIVE] started args=[confirm]"
"$BUILD/rwyctl" --store "$ST" loader show > "$BUILD/loader_show2_boot2.out"
grep -q "good B, trial none" "$BUILD/loader_show2_boot2.out" || fail "good is not B on reboot"
echo "2: PASS"

echo "--- 3: error return fallback (same-boot fallback to good) ---"
"$BUILD/rwyctl" --store "$ST" loader set "$BUILD/LOADER_FAIL.EFI" --tries 3 || fail "loader set fail"
store_put "$IMG" 0 "$ST"

run
expect "RUNWAY STUB"
expect "[TEST_LOADER] failing on purpose"
expect "RUNWAY: booting CONFIG1"
expect "[NATIVE] started args=[confirm]"
"$BUILD/rwyctl" --store "$ST" loader show > "$BUILD/loader_show3.out"
grep -q "good B, trial none" "$BUILD/loader_show3.out" || fail "good not B after fallback"
grep -q "bad A, reason start failed" "$BUILD/loader_show3.out" || fail "expected bad A, reason start failed"
echo "3: PASS"

echo "--- 4: reset loop and try exhaustion ---"
"$BUILD/rwyctl" --store "$ST" loader set "$BUILD/LOADER_RESET.EFI" --tries 2 || fail "loader set reset"
store_put "$IMG" 0 "$ST"

run
[ "$(grep -c '\[TEST_LOADER\] resetting on purpose' "$LOG")" -eq 2 ] || fail "expected 2 resets" "$LOG"
expect "RUNWAY: booting CONFIG1"
expect "[NATIVE] started args=[confirm]"
"$BUILD/rwyctl" --store "$ST" loader show > "$BUILD/loader_show4.out"
grep -q "good B, trial none" "$BUILD/loader_show4.out" || fail "good not B after try exhaustion"
grep -q "bad A, reason tries exhausted" "$BUILD/loader_show4.out" || fail "expected bad A, reason tries exhausted"
echo "4: PASS"

echo "--- 5: watchdog hang recovery ---"
WDT_DIR="$BUILD/loader_wdt_dir"
mkdir -p "$WDT_DIR"
cp -f "$BUILD/I6300ESB.EFI" "$WDT_DIR/"
"$BUILD/rwyctl" --store "$ST" loader set "$BUILD/LOADER_HANG.EFI" --wdt "$WDT_DIR" --timeout 4 --tries 1 || fail "loader set hang"
store_put "$IMG" 0 "$ST"

TMO=25 run -device i6300esb -watchdog-action reset
expect "[TEST_LOADER] hanging on purpose"
expect "RUNWAY: booting CONFIG1"
expect "[NATIVE] started args=[confirm]"
"$BUILD/rwyctl" --store "$ST" loader show > "$BUILD/loader_show5.out"
grep -q "good B, trial none" "$BUILD/loader_show5.out" || fail "good not B after watchdog hang"
grep -q "bad A" "$BUILD/loader_show5.out" || fail "copy A not marked bad"
rm -rf "$WDT_DIR"
echo "5: PASS"

echo "--- 6: mirror member reconciliation ---"
IMG0="$BUILD/loader_m0.img"
IMG1="$BUILD/loader_m1.img"
ST0="$BUILD/loader_m0.store"
ST1="$BUILD/loader_m1.store"
U0="00000000-0000-4000-8000-000000000001"
U1="00000000-0000-4000-8000-000000000002"
MIRROR_UUID="11112222-3333-4444-5555-666677778888"

"$BUILD/mkstore" --uuid "$MIRROR_UUID" --loader "$BUILD/rwy-loader.efi" "$ST0" >/dev/null
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST0" mkdir "$T"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST0" mkdir "$T/BLOBS"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST0" put "$T/BLOBS/uki.efi" @"$BUILD/NATIVE_PAYLOAD.EFI"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST0" mkdir "$T/CONFIG1"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST0" link "$T/BLOBS/uki.efi" "$T/CONFIG1/KERNEL"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST0" put "$T/CONFIG1/ARGS" confirm
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST0" put "$T/CONFIG1/STATE" "INSTALLED
"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST0" put "$T/CONFIG1/STATE" "OK
"
cp -f "$ST0" "$ST1"

"$BUILD/rwyctl" --store "$ST0" loader set "$BUILD/rwy-loader.efi" --timeout 300 --tries 3 >/dev/null
tests/qemu/mkesp_mirror.sh "$IMG0" "$IMG1" "$ST0" "$ST1" "$U0" "$U1" >/dev/null

qemu_vars_copy "$VARS"
timeout 60 "$QEMU_BIN" $(qemu_args "$VARS") \
    -drive format=raw,file="$IMG0" -drive format=raw,file="$IMG1" \
    -display none -serial stdio > "$LOG" 2>&1 || true

expect "RUNWAY STUB"
expect "RUNWAY: booting CONFIG1"
expect "[NATIVE] started args=[confirm]"

store_get "$IMG0" 0 "$ST0"
store_get "$IMG1" 0 "$ST1"
"$BUILD/rwyctl" --store "$ST0" loader show > "$BUILD/show_m0.out"
"$BUILD/rwyctl" --store "$ST1" loader show > "$BUILD/show_m1.out"
grep -q "good B, trial none" "$BUILD/show_m0.out" || fail "m0 good not B"
grep -q "good B, trial none" "$BUILD/show_m1.out" || fail "m1 not reconciled to good B"
rm -f "$IMG0" "$IMG1" "$ST0" "$ST1"
echo "6: PASS"

echo "--- 7: Secure Boot snakeoil tests ---"
SNAKE=/usr/share/ovmf/PkKek-1-snakeoil
if [ -f "$SNAKE.key" ] && command -v sbsign >/dev/null 2>&1; then
    SKEY=$(mktemp /var/tmp/rwy-sbkey-XXXXXX)
    trap 'rm -f "$SKEY" "$BUILD"/sb_*.efi "$BUILD"/loader_sb.*' EXIT
    openssl pkey -in "$SNAKE.key" -passin pass:snakeoil -out "$SKEY" >/dev/null 2>&1
    sbsign --key "$SKEY" --cert "$SNAKE.pem" --output "$BUILD/sb_stub.efi" "$BUILD/RUNWAY.EFI" >/dev/null
    sbsign --key "$SKEY" --cert "$SNAKE.pem" --output "$BUILD/sb_uki.efi" "$BUILD/NATIVE_PAYLOAD.EFI" >/dev/null
    sbsign --key "$SKEY" --cert "$SNAKE.pem" --output "$BUILD/sb_loader_good.efi" "$BUILD/rwy-loader.efi" >/dev/null

    IMG_SB="$BUILD/loader_sb.img"
    ST_SB="$BUILD/loader_sb.store"

    tests/qemu/mkdisk_multivol.sh "$IMG_SB" 0 0 >/dev/null
    mcopy -o -i "$IMG_SB@@$(img_part_off "$IMG_SB" 1)" "$BUILD/sb_stub.efi" "::EFI/BOOT/$QEMU_BOOT_FILE"
    mcopy -o -i "$IMG_SB@@$(img_part_off "$IMG_SB" 1)" "$BUILD/sb_stub.efi" ::EFI/RUNWAY/RUNWAY.EFI

    "$BUILD/mkstore" --loader "$BUILD/sb_loader_good.efi" "$ST_SB" >/dev/null
    HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST_SB" mkdir "$T"
    HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST_SB" mkdir "$T/BLOBS"
    HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST_SB" put "$T/BLOBS/uki.efi" @"$BUILD/sb_uki.efi"
    HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST_SB" mkdir "$T/CONFIG1"
    HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST_SB" link "$T/BLOBS/uki.efi" "$T/CONFIG1/KERNEL"
    HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST_SB" put "$T/CONFIG1/ARGS" confirm
    HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST_SB" put "$T/CONFIG1/STATE" "INSTALLED
"
    HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST_SB" put "$T/CONFIG1/STATE" "OK
"

    # 7a: unsigned trial child rejected with EFI_ACCESS_DENIED, falls back to signed good copy
    "$BUILD/rwyctl" --store "$ST_SB" loader set "$BUILD/rwy-loader.efi" --tries 3 >/dev/null
    store_put "$IMG_SB" 0 "$ST_SB"

    SAVED_AAVMF_CODE=${AAVMF_CODE:-}
    SAVED_AAVMF_VARS=${AAVMF_VARS_TEMPLATE:-}
    SAVED_OVMF_CODE=${OVMF_CODE:-}
    SAVED_OVMF_VARS=${OVMF_VARS_TEMPLATE:-}
    if [ "$ARCH" = "aarch64" ]; then
        AAVMF_CODE=/usr/share/AAVMF/AAVMF_CODE.snakeoil.fd
        AAVMF_VARS_TEMPLATE=/usr/share/AAVMF/AAVMF_VARS.snakeoil.fd
    else
        OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.snakeoil.fd
        OVMF_VARS_TEMPLATE=/usr/share/OVMF/OVMF_VARS_4M.snakeoil.fd
    fi

    qemu_vars_copy "$VARS"
    timeout 60 "$QEMU_BIN" $(qemu_args "$VARS") \
        -drive format=raw,file="$IMG_SB" -display none -serial stdio > "$LOG" 2>&1 || true

    expect "RUNWAY STUB"
    expect "RUNWAY: booting CONFIG1"
    expect "[NATIVE] started args=[confirm]"
    store_get "$IMG_SB" 0 "$ST_SB"
    "$BUILD/rwyctl" --store "$ST_SB" loader show > "$BUILD/show_sb_7a.out"
    grep -q "bad B, reason start failed" "$BUILD/show_sb_7a.out" || fail "unsigned copy B not marked bad"

    # 7b: signed trial child succeeds and confirms
    sbsign --key "$SKEY" --cert "$SNAKE.pem" --output "$BUILD/sb_loader_trial.efi" "$BUILD/rwy-loader.efi" >/dev/null
    "$BUILD/rwyctl" --store "$ST_SB" loader set "$BUILD/sb_loader_trial.efi" --tries 3 >/dev/null
    store_put "$IMG_SB" 0 "$ST_SB"

    qemu_vars_copy "$VARS"
    timeout 60 "$QEMU_BIN" $(qemu_args "$VARS") \
        -drive format=raw,file="$IMG_SB" -display none -serial stdio > "$LOG" 2>&1 || true

    expect "RUNWAY STUB"
    expect "RUNWAY: booting CONFIG1"
    expect "[NATIVE] started args=[confirm]"
    store_get "$IMG_SB" 0 "$ST_SB"
    "$BUILD/rwyctl" --store "$ST_SB" loader show > "$BUILD/show_sb_7b.out"
    grep -q "good B, trial none" "$BUILD/show_sb_7b.out" || fail "signed copy B not confirmed"

    AAVMF_CODE=$SAVED_AAVMF_CODE
    AAVMF_VARS_TEMPLATE=$SAVED_AAVMF_VARS
    OVMF_CODE=$SAVED_OVMF_CODE
    OVMF_VARS_TEMPLATE=$SAVED_OVMF_VARS
    rm -f "$SKEY" "$BUILD"/sb_*.efi "$BUILD"/loader_sb.*
    echo "7: PASS"
else
    echo "7: SKIP (sbsign or snakeoil keys absent)"
fi

echo "--- 8: Multi-disk stub scanning ---"
IMG0=$BUILD/loader_md0.img
IMG1=$BUILD/loader_md1.img
ST0=$BUILD/loader_md0.store
ST1=$BUILD/loader_md1.store
U0="00000000-0000-4000-8000-000000000003"
U1="00000000-0000-4000-8000-000000000004"
MIRROR_UUID="33334444-5555-6666-7777-88889999aaaa"

"$BUILD/mkstore" --uuid "$MIRROR_UUID" --loader "$BUILD/rwy-loader.efi" "$ST1" >/dev/null
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST1" mkdir "$T"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST1" mkdir "$T/BLOBS"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST1" put "$T/BLOBS/uki.efi" @"$BUILD/NATIVE_PAYLOAD.EFI"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST1" mkdir "$T/CONFIG1"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST1" link "$T/BLOBS/uki.efi" "$T/CONFIG1/KERNEL"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST1" put "$T/CONFIG1/ARGS" confirm
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST1" put "$T/CONFIG1/STATE" "INSTALLED
"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST1" put "$T/CONFIG1/STATE" "OK
"
cp -f "$ST1" "$ST0"

tests/qemu/mkesp_mirror.sh "$IMG0" "$IMG1" "$ST0" "$ST1" "$U0" "$U1" >/dev/null

# 8a: Corrupt store on primary disk (IMG0); stub should find valid store on IMG1
STORE_OFF=$(img_part_off "$IMG0" 2)
dd if=/dev/zero of="$IMG0" bs=1M count=2 seek=$((STORE_OFF / 1048576)) conv=notrunc status=none

qemu_vars_copy "$VARS"
timeout 60 "$QEMU_BIN" $(qemu_args "$VARS") \
    -drive format=raw,file="$IMG0" -drive format=raw,file="$IMG1" \
    -display none -serial stdio > "$LOG" 2>&1 || true

expect "RUNWAY STUB"
expect "RUNWAY: booting CONFIG1"
expect "[NATIVE] started args=[confirm]"

# 8b: Higher generation on secondary disk is preferred
cp -f "$ST1" "$ST0"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST1" mkdir "$T/CONFIG2"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST1" link "$T/BLOBS/uki.efi" "$T/CONFIG2/KERNEL"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST1" put "$T/CONFIG2/ARGS" gen_pref
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST1" put "$T/CONFIG2/STATE" "INSTALLED
"
HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST1" put "$T/CONFIG2/STATE" "OK
"
tests/qemu/mkesp_mirror.sh "$IMG0" "$IMG1" "$ST0" "$ST1" "$U0" "$U1" >/dev/null

qemu_vars_copy "$VARS"
timeout 60 "$QEMU_BIN" $(qemu_args "$VARS") \
    -drive format=raw,file="$IMG0" -drive format=raw,file="$IMG1" \
    -display none -serial stdio > "$LOG" 2>&1 || true

expect "RUNWAY STUB"
expect "RUNWAY: booting CONFIG2"
expect "[NATIVE] started args=[gen_pref]"

rm -f "$IMG0" "$IMG1" "$ST0" "$ST1"
echo "8: PASS"

rm -f "$IMG" "$LOG" "$ST" "$VARS" "$BUILD"/loader_show*.out
echo "ALL LOADER TESTS PASSED"
