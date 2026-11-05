#!/bin/sh
# tests/qemu/test_luksroot.sh — test Debian on a LUKS2 root with Runway
# SPDX-License-Identifier: Apache-2.0
#
# Verifies:
#   P1: First boot of a fresh image (unattended, root unlocked with TPM)
#   P2: Guest verification (LUKS2 with systemd-tpm2 token, Secure Boot enabled)
#   P3: Host inspection of raw disk (only BOOTX64.EFI on ESP, store is ciphertext)
#   P4: Reboot twice (TPM state and policy hold across boots)
#   P5: Second UKI slot with new cmdline boots and unlocks without LUKS header change
#   N1: Unsigned boot binary rejected by firmware under Secure Boot
#   N2: UKI signed by untrusted key rejected by firmware, falls back to good slot
#   N3: UKI with mismatched PCR 11 signature fails to unlock root
#   N4: Reset firmware variables cause PCR 7 mismatch and prevent store unseal
#   N5: Root keyslot unseal fails in running system after initrd PCR phase transition
#   N6: Missing/wiped TPM state prevents store unseal
set -e
export PATH="$PATH:/sbin:/usr/sbin"
BUILD=${BUILD:-build/tests/qemu/bin}
. "$(dirname "$0")/qemu_lib.sh"

fail() {
    echo "FAIL: $1"
    [ -n "${2:-}" ] && [ -f "${2:-}" ] && tail -n 60 "$2" 2>/dev/null
    exit 1
}

[ "$ARCH" = x86_64 ] || { echo "test-luksroot: SKIP (AAVMF has no TPM2 stack)"; exit 0; }

for tool in cryptsetup resize2fs systemd-cryptenroll dracut ukify sbsign openssl swtpm sfdisk mcopy mdir strings sha256sum; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "test-luksroot: SKIP (missing $tool)"
        exit 0
    fi
done

SNAKE=/usr/share/ovmf/PkKek-1-snakeoil
if [ ! -f "$SNAKE.key" ] || [ ! -f "$SNAKE.pem" ]; then
    echo "test-luksroot: SKIP (missing snakeoil keys)"
    exit 0
fi

OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.snakeoil.fd
OVMF_VARS_TEMPLATE=/usr/share/OVMF/OVMF_VARS_4M.snakeoil.fd
export OVMF_CODE OVMF_VARS_TEMPLATE
if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_TEMPLATE" ]; then
    echo "test-luksroot: SKIP (missing OVMF snakeoil firmware)"
    exit 0
fi

DEBIAN_DIR=${DEBIAN_DIR:-build/debian-$ARCH-luks}
IMG=$DEBIAN_DIR/debian.img
TPM_DIR=$DEBIAN_DIR/tpmstate
LOADER_EFI=$DEBIAN_DIR/rwy-loader.signed.efi

# Build guest disk image if needed
if [ ! -f "$IMG" ] || [ ! -d "$TPM_DIR" ]; then
    ROOT=luks DEBIAN_DIR="$DEBIAN_DIR" BUILD="$BUILD" sh tests/qemu/mkdebian.sh "$IMG" || fail "mkdebian.sh failed"
fi

[ -f "$IMG" ] || { echo "test-luksroot: SKIP (image not built)"; exit 0; }

SCRATCH=$(mktemp -d /var/tmp/rwy-luksroot-XXXXXX)
QPID=""
cleanup() {
    if [ -n "$QPID" ]; then
        kill -9 "$QPID" 2>/dev/null || true
    fi
    pkill -9 -f "swtpm.*$SCRATCH" 2>/dev/null || true
    rm -rf "$SCRATCH"
    leftovers=$(ls -d /var/tmp/rwy-luksroot-* 2>/dev/null || true)
    if [ -n "$leftovers" ]; then
        echo "WARNING: Leftovers detected in /var/tmp: $leftovers"
    fi
}
trap cleanup EXIT INT TERM

STORE_PASSPHRASE="${STORE_PASSPHRASE:-runway-test-passphrase}"
TEST_IMG=$SCRATCH/test.img
TEST_VARS=$SCRATCH/test_vars.fd
TEST_TPM_DIR=$SCRATCH/tpmstate

cp -f "$IMG" "$TEST_IMG"
cp -a "$TPM_DIR" "$TEST_TPM_DIR"
cp -f "$OVMF_VARS_TEMPLATE" "$TEST_VARS"

run_qemu() {
    label="$1"
    img="${2:-$TEST_IMG}"
    vars="${3:-$TEST_VARS}"
    tpm_dir="${4:-$TEST_TPM_DIR}"
    timeout="${5:-300}"
    log="$SCRATCH/boot_${label}.log"
    sock="$SCRATCH/swtpm.sock"
    ctrl="$SCRATCH/swtpm.ctrl"
    rm -f "$sock" "$ctrl" "$log"

    swtpm socket \
        --tpmstate dir="$tpm_dir" \
        --ctrl type=unixio,path="$sock" \
        --tpm2 \
        --flags not-need-init >/dev/null 2>&1 &
    sw_pid=$!

    for i in $(seq 1 50); do
        [ -S "$sock" ] && break
        sleep 0.1
    done
    [ -S "$sock" ] || fail "swtpm socket failed to start"

    tpm_args="-chardev socket,id=chrtpm,path=$sock -tpmdev emulator,id=tpm0,chardev=chrtpm -device $QEMU_TPM_DEVICE,tpmdev=tpm0"

    "$QEMU_BIN" $(qemu_args "$vars") \
        -drive format=raw,file="$img" \
        $tpm_args \
        -display none -serial file:"$log" &
    QPID=$!

    elapsed=0
    while kill -0 "$QPID" 2>/dev/null; do
        if [ "${fail_on_guest_fail:-1}" = 1 ] && grep -q "FAIL:" "$log" 2>/dev/null; then
            kill -9 "$QPID" 2>/dev/null || true
            kill -9 "$sw_pid" 2>/dev/null || true
            fail "guest failure in $label" "$log"
        fi
        sleep 1
        elapsed=$((elapsed + 1))
        if [ $elapsed -ge $timeout ]; then
            kill -9 "$QPID" 2>/dev/null || true
            break
        fi
    done
    wait "$QPID" 2>/dev/null || true
    QPID=""

    kill -9 "$sw_pid" 2>/dev/null || true
    wait "$sw_pid" 2>/dev/null || true
    rm -f "$sock" "$ctrl"
}

boot_guest() {
    label="$1"
    expected_slot="$2"
    img="${3:-$TEST_IMG}"
    fail_on_guest_fail=1 run_qemu "$label" "$img" "$TEST_VARS" "$TEST_TPM_DIR" 300
    log="$SCRATCH/boot_${label}.log"

    grep -q "RUNWAY: booting $expected_slot" "$log" || fail "did not boot $expected_slot in $label" "$log"
    grep -q "RUNWAY-GUEST: ALL TESTS PASSED" "$log" || fail "guest tests did not pass in $label" "$log"
    ! grep -i "passphrase" "$log" || fail "unexpected passphrase prompt seen in $label" "$log"
}

# --- Case P1: First boot of a fresh image ---
echo "--- Case P1: First boot of fresh image ---"
boot_guest "p1" "CONFIG1"
echo "Case P1: PASS"

# --- Case P2: Guest verification ---
echo "--- Case P2: Guest verification ---"
LOG_P1="$SCRATCH/boot_p1.log"
grep -q "RUNWAY-GUEST: root is verified LUKS2 volume" "$LOG_P1" || fail "root was not verified LUKS2" "$LOG_P1"
grep -q "RUNWAY-GUEST: LUKS2 token and tpm2-public-key verified" "$LOG_P1" || fail "token/key check missing" "$LOG_P1"
grep -q "RUNWAY-GUEST: Secure Boot verified enabled" "$LOG_P1" || fail "Secure Boot not confirmed" "$LOG_P1"
grep -q "RUNWAY-GUEST: guest systemctl clean" "$LOG_P1" || fail "systemctl had failed units" "$LOG_P1"
echo "Case P2: PASS"

# --- Case P3: Read raw disk from host ---
echo "--- Case P3: Host raw disk inspection ---"
ESP_OFF=$(img_part_off "$TEST_IMG" 1)
files=$(mdir -s -b -i "$TEST_IMG@@$ESP_OFF" :: | tr -d '\r' | grep -v '^$' | grep -v '/$' | grep -v 'random-seed' | tr '\\' '/' | sort)
expected="::/EFI/BOOT/BOOTX64.EFI"
[ "$files" = "$expected" ] || fail "ESP has extra or unexpected files: $files"

# Check no kernel, initrd, or UKI found in plaintext on disk
! strings "$TEST_IMG" | grep -F "Linux version" || fail "plaintext Linux version found on disk"
! strings "$TEST_IMG" | grep -F ".linux" || fail "plaintext .linux section found on disk"

# Check root partition has LUKS magic and no ext4 magic at offset 1080
ROOT_OFF=$(img_part_off "$TEST_IMG" 3)
ext4_magic=$(dd if="$TEST_IMG" bs=1 skip=$((ROOT_OFF + 1080)) count=2 status=none | od -An -tx1 | tr -d ' \n')
[ "$ext4_magic" != "53ef" ] || fail "ext4 magic found at root partition start"

luks_magic=$(dd if="$TEST_IMG" bs=1 skip=$ROOT_OFF count=4 status=none)
[ "$luks_magic" = "LUKS" ] || fail "LUKS magic not found at root partition start"

# Check store partition is ciphertext
store_get "$TEST_IMG" 0 "$SCRATCH/store_check.bin"
if "$BUILD/host_fs" "$SCRATCH/store_check.bin" ls EFI/RUNWAY >/dev/null 2>&1; then
    fail "store was readable without key"
fi
! grep -a "Linux version" "$SCRATCH/store_check.bin" >/dev/null || fail "store has plaintext kernel"
! grep -a ".linux" "$SCRATCH/store_check.bin" >/dev/null || fail "store has plaintext UKI"
rm -f "$SCRATCH/store_check.bin"
echo "Case P3: PASS"

# --- Case P4: Reboot twice ---
echo "--- Case P4: Reboot twice ---"
boot_guest "p4_1" "CONFIG1"
boot_guest "p4_2" "CONFIG1"
grep -q "RUNWAY-GUEST: installed CONFIG2 successfully" "$SCRATCH/boot_p4_2.log" || fail "CONFIG2 install failed in p4_2" "$SCRATCH/boot_p4_2.log"
echo "Case P4: PASS"

# --- Case P5: Second UKI slot without LUKS header change ---
echo "--- Case P5: Second UKI slot ---"
ROOT_OFF=$(img_part_off "$TEST_IMG" 3)
luks_hash_before=$(dd if="$TEST_IMG" bs=1M skip=$((ROOT_OFF / 1048576)) count=8 status=none | sha256sum | awk '{print $1}')

boot_guest "p5" "CONFIG2"
LOG_P5="$SCRATCH/boot_p5.log"
grep -q "RUNWAY: booting CONFIG2" "$LOG_P5" || fail "did not boot CONFIG2 in p5" "$LOG_P5"
grep -q "RUNWAY-GUEST: CONFIG2 boot verified" "$LOG_P5" || fail "CONFIG2 boot not verified in guest" "$LOG_P5"
grep -q "RUNWAY-GUEST: ALL TESTS PASSED" "$LOG_P5" || fail "guest tests did not pass in p5" "$LOG_P5"

luks_hash_after=$(dd if="$TEST_IMG" bs=1M skip=$((ROOT_OFF / 1048576)) count=8 status=none | sha256sum | awk '{print $1}')
[ "$luks_hash_before" = "$luks_hash_after" ] || fail "LUKS2 header changed after slot 2 install"
echo "Case P5: PASS"

# --- Case N1: Unsigned BOOTX64.EFI rejected by firmware ---
echo "--- Case N1: Unsigned boot binary rejected under Secure Boot ---"
N1_IMG="$SCRATCH/n1.img"
cp -f "$TEST_IMG" "$N1_IMG"
N1_ESP_OFF=$(img_part_off "$N1_IMG" 1)
mcopy -o -i "$N1_IMG@@$N1_ESP_OFF" "$BUILD/RUNWAY.EFI" "::EFI/BOOT/BOOTX64.EFI"
fail_on_guest_fail=0 run_qemu "n1" "$N1_IMG" "$TEST_VARS" "$TEST_TPM_DIR" 15
LOG_N1="$SCRATCH/boot_n1.log"
grep -q -i -E "Access Denied|Security Violation" "$LOG_N1" || fail "firmware did not reject unsigned binary" "$LOG_N1"
! grep -q "RUNWAY: store" "$LOG_N1" || fail "Runway unexpectedly ran in N1" "$LOG_N1"
! grep -q "RUNWAY-GUEST" "$LOG_N1" || fail "Guest unexpectedly ran in N1" "$LOG_N1"
rm -f "$N1_IMG"
echo "Case N1: PASS"

# --- Case N2: UKI signed by untrusted key falls back to good slot ---
echo "--- Case N2: Untrusted UKI signature falls back to good slot ---"
N2_IMG="$SCRATCH/n2.img"
N2_TPM_DIR="$SCRATCH/tpmstate_n2"
cp -f "$IMG" "$N2_IMG"
cp -a "$TPM_DIR" "$N2_TPM_DIR"
N2_STORE="$SCRATCH/n2_store.img"
"$BUILD/mkstore" --loader "$LOADER_EFI" --size 256 --slots 2 --encrypt "$N2_STORE" >/dev/null
RWY_KEY="$STORE_PASSPHRASE" "$BUILD/rwyctl" --store "$N2_STORE" install --name CONFIG2 \
    --kernel "$DEBIAN_DIR/uki2.efi" --entry "debian-guest2.conf" --label "Debian Trixie Slot 2" --tries 3 >/dev/null
RWY_KEY="$STORE_PASSPHRASE" "$BUILD/rwyctl" --store "$N2_STORE" install --name CONFIG1 \
    --kernel "$DEBIAN_DIR/uki-bad-sb.efi" --entry "debian-bad-sb.conf" --label "Debian Bad SB" --tries 3 >/dev/null
store_put "$N2_IMG" 0 "$N2_STORE"
rm -f "$N2_STORE"

fail_on_guest_fail=1 run_qemu "n2" "$N2_IMG" "$TEST_VARS" "$N2_TPM_DIR" 300
LOG_N2="$SCRATCH/boot_n2.log"
grep -q "CONFIG1 cannot be started" "$LOG_N2" || fail "CONFIG1 not rejected in N2" "$LOG_N2"
grep -q "RUNWAY: booting CONFIG2" "$LOG_N2" || fail "CONFIG2 not booted in N2" "$LOG_N2"
grep -q "RUNWAY-GUEST: ALL TESTS PASSED" "$LOG_N2" || fail "guest tests did not pass in N2" "$LOG_N2"
! grep -i "passphrase" "$LOG_N2" || fail "unexpected passphrase prompt seen in N2" "$LOG_N2"
rm -f "$N2_IMG"
rm -rf "$N2_TPM_DIR"
echo "Case N2: PASS"

# --- Case N3: UKI with bad PCR signature fails to unlock root ---
echo "--- Case N3: UKI with mismatched PCR 11 signature fails root unlock ---"
N3_IMG="$SCRATCH/n3.img"
N3_TPM_DIR="$SCRATCH/tpmstate_n3"
cp -f "$IMG" "$N3_IMG"
cp -a "$TPM_DIR" "$N3_TPM_DIR"
N3_STORE="$SCRATCH/n3_store.img"
"$BUILD/mkstore" --loader "$LOADER_EFI" --size 256 --slots 2 --encrypt "$N3_STORE" >/dev/null
RWY_KEY="$STORE_PASSPHRASE" "$BUILD/rwyctl" --store "$N3_STORE" install --name CONFIG1 \
    --kernel "$DEBIAN_DIR/uki-bad-pcr.efi" --entry "debian-bad-pcr.conf" --label "Debian Bad PCR" --tries 3 >/dev/null
store_put "$N3_IMG" 0 "$N3_STORE"
rm -f "$N3_STORE"

fail_on_guest_fail=0 run_qemu "n3" "$N3_IMG" "$TEST_VARS" "$N3_TPM_DIR" 45
LOG_N3="$SCRATCH/boot_n3.log"
grep -q "RUNWAY: booting CONFIG1" "$LOG_N3" || fail "CONFIG1 did not boot in N3" "$LOG_N3"
! grep -q "RUNWAY-GUEST: ALL TESTS PASSED" "$LOG_N3" || fail "guest tests unexpectedly passed in N3" "$LOG_N3"
! grep -i "passphrase" "$LOG_N3" || fail "unexpected passphrase prompt in N3" "$LOG_N3"
grep -q -i -E "failed to activate|could not boot|timed out|emergency|dracut" "$LOG_N3" || fail "missing expected unlock failure in N3" "$LOG_N3"
rm -f "$N3_IMG"
rm -rf "$N3_TPM_DIR"
echo "Case N3: PASS"

# --- Case N4: Reset firmware variables cause PCR 7 mismatch ---
echo "--- Case N4: Reset firmware variables prevent store unseal ---"
N4_VARS="$SCRATCH/n4_vars.fd"
DEFAULT_VARS=/usr/share/OVMF/OVMF_VARS_4M.fd
[ -f "$DEFAULT_VARS" ] || DEFAULT_VARS=/usr/share/OVMF/OVMF_VARS.fd
cp -f "$DEFAULT_VARS" "$N4_VARS"
fail_on_guest_fail=0 run_qemu "n4" "$TEST_IMG" "$N4_VARS" "$TEST_TPM_DIR" 15
LOG_N4="$SCRATCH/boot_n4.log"
grep -q "SECURITY ALERT: TPM 2.0 unseal failed!" "$LOG_N4" || fail "unseal alert missing in N4" "$LOG_N4"
grep -q "RUNWAY: boot store cannot be mounted" "$LOG_N4" || fail "store mount failure missing in N4" "$LOG_N4"
! grep -q "RUNWAY: booting" "$LOG_N4" || fail "Runway booted a slot in N4" "$LOG_N4"
! grep -q "RUNWAY-GUEST" "$LOG_N4" || fail "Guest ran in N4" "$LOG_N4"
rm -f "$N4_VARS"
echo "Case N4: PASS"

# --- Case N5: Running system cannot unseal root keyslot ---
echo "--- Case N5: Root keyslot unseal fails in running system ---"
LOG_P1="$SCRATCH/boot_p1.log"
grep -q "RUNWAY-GUEST: running system cannot unlock root via TPM (N5 verified)" "$LOG_P1" || fail "N5 check missing from guest log" "$LOG_P1"
echo "Case N5: PASS"

# --- Case N6: Wiped TPM prevents store unseal ---
echo "--- Case N6: Wiped TPM state prevents store unseal ---"
N6_TPM_DIR="$SCRATCH/tpmstate_empty"
rm -rf "$N6_TPM_DIR"
mkdir -p "$N6_TPM_DIR"
fail_on_guest_fail=0 run_qemu "n6" "$TEST_IMG" "$TEST_VARS" "$N6_TPM_DIR" 15
LOG_N6="$SCRATCH/boot_n6.log"
grep -q "SECURITY ALERT: TPM 2.0 unseal failed!" "$LOG_N6" || fail "unseal alert missing in N6" "$LOG_N6"
grep -q "RUNWAY: boot store cannot be mounted" "$LOG_N6" || fail "store mount failure missing in N6" "$LOG_N6"
! grep -q "RUNWAY: booting" "$LOG_N6" || fail "Runway booted a slot in N6" "$LOG_N6"
! grep -q "RUNWAY-GUEST" "$LOG_N6" || fail "Guest ran in N6" "$LOG_N6"
rm -rf "$N6_TPM_DIR"
echo "Case N6: PASS"

echo "test-luksroot: ALL TESTS PASSED"
