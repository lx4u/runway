#!/bin/sh
# tests/qemu/test_tpm.sh — End-to-end integration test for TPM 2.0 PCR 7 sealing & unsealing.
# SPDX-License-Identifier: Apache-2.0
#
# Verifies:
#  1. Enrollment Phase: factory encrypted store unwraps Slot 0, seals to TPM 2.0 PCR 7
#     into Keyslot 1, clears Slot 0, persists to disk, boots successfully.
#  2. Normal Boot Phase: subsequent boot unseals DEK directly from TPM 2.0 PCR 7
#     without any interactive passphrase, boots successfully.
#  3. Tamper Detection: tampered TPM / PCR 7 state fails hardware unseal and halts boot.
set -e

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
[ "$ARCH" = x86_64 ] || { echo "test-tpm: SKIP (AAVMF has no TPM2 stack)"; exit 0; }
TPM_TEST_DIR="$BUILD/tpmstate_test"
TPM_TAMPER_DIR="$BUILD/tpmstate_tamper"
LOG_ENROLL="$BUILD/tpm_enroll.log"
LOG_NORMAL="$BUILD/tpm_normal.log"
LOG_TAMPER="$BUILD/tpm_tamper.log"

fail() { echo "FAIL: $1"; [ -n "$2" ] && tail -n 60 "$2"; exit 1; }

SCRATCH=$(mktemp -d /var/tmp/rwy-tpm-XXXXXX)
cleanup_scratch() {
    pkill -9 -f "swtpm.*$SCRATCH" 2>/dev/null || true
    rm -rf "$SCRATCH"
    leftovers=$(ls -d /var/tmp/rwy-tpm-* 2>/dev/null || true)
    if [ -n "$leftovers" ]; then
        echo "WARNING: Leftovers detected in /var/tmp: $leftovers"
    fi
}
trap cleanup_scratch EXIT INT TERM

run_tpm_kill_on() {
    trigger="$1"
    log="$2"
    rm -f "$log"
    ./tools/run_tpm.sh > "$log" 2>&1 &
    tpm_pid=$!
    killed=0
    for i in $(seq 1 400); do
        if grep -q "$trigger" "$log" 2>/dev/null; then
            pkill -9 -P $tpm_pid 2>/dev/null || true
            kill -9 $tpm_pid 2>/dev/null || true
            pkill -9 -f "${QEMU_BIN:-qemu-system}.*$ESP_IMG" 2>/dev/null || true
            killed=1
            break
        fi
        if ! kill -0 $tpm_pid 2>/dev/null; then
            break
        fi
        sleep 0.05
    done
    wait $tpm_pid 2>/dev/null || true
    pkill -9 -f "swtpm.*$TPM_DIR" 2>/dev/null || true
    rm -f "$TPM_DIR/swtpm-sock"
    sleep 0.5
    [ "$killed" = 1 ] || fail "trigger '$trigger' not seen before exit" "$log"
}

echo "================================================================="
echo "  Runway TPM 2.0 PCR 7 Hardware Sealing Integration Test"
echo "================================================================="

# Clean up previous state
rm -rf "$TPM_TEST_DIR" "$TPM_TAMPER_DIR"
mkdir -p "$BUILD"

echo "--- 1. Generating Encrypted Backing Store (Factory Slot 0 Active) ---"
$BUILD/mkstore --encrypt "$BUILD/RUNWAY.DAT"
sh tools/mkesp.sh esp.img

echo "--- 2. Factory Enrollment with TPM 2.0 PCR 7 Sealing ---"
export TPM_DIR="$TPM_TEST_DIR"
export RESET_VARS="1"
t0=$(date +%s)
./tools/run_tpm.sh > "$LOG_ENROLL" 2>&1 || true
t1=$(date +%s)
boot_elapsed=$((t1 - t0))

if ! grep -q "crypto: store encrypted with AES-256-XTS" "$LOG_ENROLL"; then
    fail "Expected AES-256-XTS detection in enrollment log" "$LOG_ENROLL"
fi

if ! grep -q "tpm2: enrollment phase: unwrapping factory transport key" "$LOG_ENROLL"; then
    fail "Expected TPM 2.0 enrollment phase message" "$LOG_ENROLL"
fi

if ! grep -q "reencrypt: copying" "$LOG_ENROLL"; then
    fail "Expected reencrypt copying in enrollment log" "$LOG_ENROLL"
fi

if ! grep -q "reencrypt: finalizing" "$LOG_ENROLL"; then
    fail "Expected reencrypt finalizing in enrollment log" "$LOG_ENROLL"
fi

copy_time_str=$(sed -n 's/.*reencrypt: copy completed in \([0-9.]*\) s.*/\1/p' "$LOG_ENROLL" | head -n 1)
echo "Re-encryption copy time: ${copy_time_str:-unknown}s (boot total: ${boot_elapsed}s, budget: 60s)"
if [ "$boot_elapsed" -gt 60 ]; then
    fail "Enrollment boot took ${boot_elapsed}s, exceeding budget of 60s" "$LOG_ENROLL"
fi

if ! grep -q "tpm2: enrolled DEK sealed in Keyslot 1, Slot 0 cleared" "$LOG_ENROLL"; then
    fail "Expected DEK sealed into Keyslot 1 and Slot 0 cleared" "$LOG_ENROLL"
fi

if ! grep -q "RESULT: PASS" "$LOG_ENROLL"; then
    fail "Enrollment boot did not achieve RESULT: PASS" "$LOG_ENROLL"
fi
echo "Enrollment Phase: PASS (DEK sealed into Keyslot 1, Slot 0 cleared)"

echo "--- 3. Normal Boot with TPM 2.0 Hardware Unsealing (Zero Passphrase) ---"
# Boot again with same enrolled esp.img and same TPM state directory
export RESET_VARS="0"
./tools/run_tpm.sh > "$LOG_NORMAL" 2>&1 || true

if ! grep -q "tpm2: unsealing DEK using PCR 7 policy" "$LOG_NORMAL"; then
    fail "Expected TPM 2.0 unsealing message" "$LOG_NORMAL"
fi

if ! grep -q "tpm2: hardware unseal successful (PCR 7 verified)" "$LOG_NORMAL"; then
    fail "Expected successful hardware unseal message" "$LOG_NORMAL"
fi

if grep -q "tpm2: enrollment phase" "$LOG_NORMAL"; then
    fail "Enrollment triggered again during normal boot!" "$LOG_NORMAL"
fi

if grep -q "reencrypt: copying" "$LOG_NORMAL"; then
    fail "Re-encryption triggered again during normal boot!" "$LOG_NORMAL"
fi

if ! grep -q "RESULT: PASS" "$LOG_NORMAL"; then
    fail "Normal boot did not achieve RESULT: PASS" "$LOG_NORMAL"
fi
echo "Normal Boot Phase: PASS (Pure TPM 2.0 PCR 7 unseal, no passphrase)"

echo "--- 4. Tamper Detection (Tampered / Mismatched TPM State) ---"
# Boot with empty / mismatched TPM state simulating altered PCR 7 / foreign system
export TPM_DIR="$TPM_TAMPER_DIR"
./tools/run_tpm.sh > "$LOG_TAMPER" 2>&1 || true

if ! grep -q "SECURITY ALERT: TPM 2.0 unseal failed!" "$LOG_TAMPER"; then
    fail "Expected tamper detection alert" "$LOG_TAMPER"
fi

if grep -q "RESULT: PASS" "$LOG_TAMPER"; then
    fail "Boot succeeded despite tampered / mismatched TPM!" "$LOG_TAMPER"
fi
echo "Tamper Prevention Phase: PASS (Tampered boot prevented)"

echo "--- 1b: Power Cut During Re-encryption Copy ---"
CUT_STORE="$SCRATCH/cut1.store"
CUT_TPM="$SCRATCH/tpm_cut1"
LOG_CUT1="$BUILD/tpm_cut1.log"
LOG_REBOOT1="$BUILD/tpm_reboot1.log"
rm -rf "$CUT_TPM"; mkdir -p "$CUT_TPM"
$BUILD/mkstore --encrypt "$CUT_STORE"
sh tools/mkesp.sh esp.img "$CUT_STORE"

TPM_DIR="$CUT_TPM" RESET_VARS=1 run_tpm_kill_on "reencrypt: copying" "$LOG_CUT1"

# Verify store on disk right now: factory key is still valid (publish has not occurred)
store_get esp.img 0 "$SCRATCH/cut1_extracted.store"
RWY_KEY=runway-test-passphrase $BUILD/rwyctl --store "$SCRATCH/cut1_extracted.store" status >/dev/null 2>&1 || fail "factory key failed after interrupted copy"
$BUILD/fsck.runway --key runway-test-passphrase "$SCRATCH/cut1_extracted.store" >/dev/null 2>&1 || fail "fsck.runway failed on interrupted copy"

# Reboot: completes re-encryption and enrollment
TPM_DIR="$CUT_TPM" RESET_VARS=0 ./tools/run_tpm.sh > "$LOG_REBOOT1" 2>&1 || true
if ! grep -q "RESULT: PASS" "$LOG_REBOOT1"; then
    fail "Reboot after interrupted copy failed" "$LOG_REBOOT1"
fi
if ! grep -q "reencrypt: copying" "$LOG_REBOOT1"; then
    fail "Re-encryption copy did not re-run on reboot" "$LOG_REBOOT1"
fi
if ! grep -q "tpm2: enrolled DEK sealed in Keyslot 1, Slot 0 cleared" "$LOG_REBOOT1"; then
    fail "TPM enrollment missing on reboot" "$LOG_REBOOT1"
fi

# Verify store on disk: factory key now fails
store_get esp.img 0 "$SCRATCH/cut1_extracted2.store"
if RWY_KEY=runway-test-passphrase $BUILD/rwyctl --store "$SCRATCH/cut1_extracted2.store" status >/dev/null 2>&1; then
    fail "Factory passphrase still worked after completed re-encryption!"
fi
if $BUILD/fsck.runway --key runway-test-passphrase "$SCRATCH/cut1_extracted2.store" >/dev/null 2>&1; then
    fail "fsck.runway with factory key still worked after completed re-encryption!"
fi

# Subsequent boot: unseals with TPM without re-encryption
LOG_NORMAL_CUT1="$BUILD/tpm_normal_cut1.log"
TPM_DIR="$CUT_TPM" RESET_VARS=0 ./tools/run_tpm.sh > "$LOG_NORMAL_CUT1" 2>&1 || true
if ! grep -q "tpm2: hardware unseal successful" "$LOG_NORMAL_CUT1"; then
    fail "TPM unseal failed after recovered copy" "$LOG_NORMAL_CUT1"
fi
if grep -q "reencrypt: copying" "$LOG_NORMAL_CUT1"; then
    fail "Re-encryption ran again after recovery" "$LOG_NORMAL_CUT1"
fi
if ! grep -q "RESULT: PASS" "$LOG_NORMAL_CUT1"; then
    fail "Normal boot after recovery failed" "$LOG_NORMAL_CUT1"
fi
echo "1b: PASS (power cut during copy recovered cleanly)"

echo "--- 1c: Power Cut After Publish, Before Finalize ---"
CUT2_STORE="$SCRATCH/cut2.store"
CUT2_TPM="$SCRATCH/tpm_cut2"
LOG_CUT2="$BUILD/tpm_cut2.log"
LOG_REBOOT2="$BUILD/tpm_reboot2.log"
rm -rf "$CUT2_TPM"; mkdir -p "$CUT2_TPM"
$BUILD/mkstore --encrypt "$CUT2_STORE"
sh tools/mkesp.sh esp.img "$CUT2_STORE"

TPM_DIR="$CUT2_TPM" RESET_VARS=1 run_tpm_kill_on "reencrypt: finalizing" "$LOG_CUT2"

# Verify store on disk right now:
store_get esp.img 0 "$SCRATCH/cut2_extracted.store"
if RWY_KEY=runway-test-passphrase $BUILD/rwyctl --store "$SCRATCH/cut2_extracted.store" status >/dev/null 2>&1; then
    fail "Factory passphrase unexpectedly succeeded on published slot!"
fi

# Reboot: mounts via TPM unseal, detects pending finalize, resumes and completes finalize
TPM_DIR="$CUT2_TPM" RESET_VARS=0 ./tools/run_tpm.sh > "$LOG_REBOOT2" 2>&1 || true
if ! grep -q "tpm2: hardware unseal successful" "$LOG_REBOOT2"; then
    fail "TPM unseal failed on reboot with pending finalize" "$LOG_REBOOT2"
fi
if ! grep -q "reencrypt: resuming pending finalize from previous boot" "$LOG_REBOOT2"; then
    fail "Resume of pending finalize not logged on reboot" "$LOG_REBOOT2"
fi
if ! grep -q "RESULT: PASS" "$LOG_REBOOT2"; then
    fail "Reboot with pending finalize failed" "$LOG_REBOOT2"
fi

# Subsequent boot: unseals cleanly, pending finalize is no longer triggered
LOG_NORMAL_CUT2="$BUILD/tpm_normal_cut2.log"
TPM_DIR="$CUT2_TPM" RESET_VARS=0 ./tools/run_tpm.sh > "$LOG_NORMAL_CUT2" 2>&1 || true
if ! grep -q "tpm2: hardware unseal successful" "$LOG_NORMAL_CUT2"; then
    fail "TPM unseal failed on normal boot after finalized resume" "$LOG_NORMAL_CUT2"
fi
if grep -q "reencrypt: resuming pending finalize" "$LOG_NORMAL_CUT2"; then
    fail "Pending finalize resumed again after completion" "$LOG_NORMAL_CUT2"
fi
if ! grep -q "RESULT: PASS" "$LOG_NORMAL_CUT2"; then
    fail "Normal boot after finalized resume failed" "$LOG_NORMAL_CUT2"
fi
echo "1c: PASS (power cut before finalize resumed cleanly)"

echo "--- 1d: Nearly Full Store Re-encryption Refusal ---"
FULL_STORE="$SCRATCH/full.store"
FULL_TPM="$SCRATCH/tpm_full"
LOG_FULL="$BUILD/tpm_full.log"
rm -rf "$FULL_TPM"; mkdir -p "$FULL_TPM"

# Create a default 256 MiB store and populate with a 140 MiB file so live data > 50%
$BUILD/mkstore --encrypt "$FULL_STORE"
dd if=/dev/urandom of="$SCRATCH/fill.bin" bs=1M count=140 status=none
HOST_FS_PASSPHRASE=runway-test-passphrase $BUILD/host_fs "$FULL_STORE" put /BIG.BIN @"$SCRATCH/fill.bin"
sh tools/mkesp.sh esp.img "$FULL_STORE"

TPM_DIR="$FULL_TPM" RESET_VARS=1 ./tools/run_tpm.sh > "$LOG_FULL" 2>&1 || true
if ! grep -q "first-boot re-encryption failed, retrying at next boot (no space)" "$LOG_FULL"; then
    fail "Expected first-boot re-encryption refusal message for full store" "$LOG_FULL"
fi
if ! grep -q "RESULT: PASS" "$LOG_FULL"; then
    fail "Boot failed when re-encryption refused due to lack of space" "$LOG_FULL"
fi

# Check that store remains untouched and accessible with factory passphrase
store_get esp.img 0 "$SCRATCH/full_extracted.store"
RWY_KEY=runway-test-passphrase $BUILD/rwyctl --store "$SCRATCH/full_extracted.store" status >/dev/null 2>&1 || fail "factory key failed on full store"
$BUILD/fsck.runway --key runway-test-passphrase "$SCRATCH/full_extracted.store" >/dev/null 2>&1 || fail "fsck.runway failed on full store"

echo "1d: PASS (boots with the factory key when space is insufficient)"

echo "--- 4b. Native boot on an encrypted store: enrollment, unseal, tamper ---"
# EFI/RUNWAY is seeded before enrollment with the factory data key; the slots must boot the same
# through the enrollment boot and the unsealed boot, and a tampered TPM must boot nothing.
. tests/qemu/native_lib.sh
# TPM enrolment clears keyslot 0, so host_fs opens each store by its data key
NST="$BUILD/nat_tpm.store"; NLOG="$BUILD/nat_tpm.log"; NIMG2="$BUILD/nat_tpm2.img"
$BUILD/mkstore --encrypt --dek-out "$BUILD/nat_tpm.dek" "$NST" >/dev/null
export HOST_FS_DEK_FILE="$BUILD/nat_tpm.dek"
nat_seed "$NST"
nat_slot "$NST" 1 confirm OK
nat_slot "$NST" 2 confirm INSTALLED
# a watchdog application and a global WATCHDOG: WDT/ is read from the encrypted boot store
nat_fs "$NST" put $NAT_T/BLOBS/ok.efi @$BUILD/WDT_STUB_OK.EFI
nat_fs "$NST" put $NAT_T/WATCHDOG "30
"
HOST_FS_SYSTEM=1 nat_fs "$NST" mkdir $NAT_T/WDT
HOST_FS_SYSTEM=1 nat_fs "$NST" link $NAT_T/BLOBS/ok.efi $NAT_T/WDT/A1.EFI
sh tools/mkesp.sh esp.img "$NST" >/dev/null
dd if=/dev/zero of="$NIMG2" bs=1M count=64 status=none
printf 'label: gpt\nstart=2048, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="ESP"\n' | sfdisk "$NIMG2" >/dev/null
sz=$(sfdisk -d "$NIMG2" | awk -v n=1 '/start=/ {c++; if (c==n) {gsub(/[^0-9]/,"",$6); print $6}}')
mkfs.vfat --offset 2048 -F 16 "$NIMG2" $((sz / 2)) >/dev/null 2>&1
mmd -i "$NIMG2@@1048576" ::EFI ::EFI/BOOT
mcopy -i "$NIMG2@@1048576" "$NAT_KERNEL" "::EFI/BOOT/$QEMU_BOOT_FILE"
nat_store() { store_get esp.img 0 "$NST"; }

export TPM_DIR="$BUILD/tpmstate_native"; rm -rf "$TPM_DIR"
RESET_VARS=1 ./tools/run_tpm.sh > "$NLOG" 2>&1 || true
nat_expect "$NLOG" "tpm2: enrolled DEK sealed in Keyslot 1"
nat_expect "$NLOG" "RUNWAY: booting CONFIG2 (rev 2, TESTING, 0 tries left)"
nat_expect "$NLOG" "[NATIVE] confirm via BOOTED: 0x0"
nat_expect "$NLOG" "RUNWAY: WATCHDOG 30s armed by A1.EFI"
PCR7=$(sed -n 's/^RUNWAY: tpm2: PCR 7 = //p' "$NLOG" | head -1 | tr -d '\r')
RDEK=$(sed -n 's/^RUNWAY: tpm2: runtime DEK = //p' "$NLOG" | head -1 | tr -d '\r')
if [ -n "$RDEK" ]; then
    python3 -c "import sys; open('$BUILD/nat_tpm.dek', 'wb').write(bytes.fromhex('$RDEK'))"
fi
nat_store
[ "$(nat_cat "$NST" $NAT_T/CONFIG2/STATE)" = OK ] || fail "enrollment boot: CONFIG2 not confirmed"
[ "$(nat_cat "$NST" $NAT_T/WDT_HINT)" = A1.EFI ] || fail "enrollment boot: WDT_HINT not recorded"
echo "N1: PASS (native boot through the enrollment boot)"

RESET_VARS=0 ./tools/run_tpm.sh > "$NLOG" 2>&1 || true
nat_expect "$NLOG" "tpm2: hardware unseal successful"
nat_expect "$NLOG" "RUNWAY: booting CONFIG2 (rev 2, OK)"
nat_expect "$NLOG" "[NATIVE] already confirmed"
nat_expect "$NLOG" "RUNWAY: WATCHDOG 30s armed by A1.EFI"
echo "N2: PASS (native boot after the TPM unseal)"

nat_store
nat_slot "$NST" 3 confirm INSTALLED
store_put esp.img 0 "$NST"
export TPM_DIR="$TPM_TAMPER_DIR"
EXTRA_DRIVES="-drive format=raw,file=$NIMG2" RESET_VARS=1 ./tools/run_tpm.sh > "$NLOG" 2>&1 || true
nat_expect "$NLOG" "SECURITY ALERT: TPM 2.0 unseal failed!"
nat_expect "$NLOG" "RUNWAY: boot store cannot be mounted"
nat_refuse "$NLOG" "RUNWAY: booting"
nat_refuse "$NLOG" "started args=[confirm]"
nat_expect "$NLOG" "[NATIVE] started args=[]"
nat_store
[ "$(nat_cat "$NST" $NAT_T/CONFIG3/STATE)" = INSTALLED ] || fail "tamper boot changed CONFIG3"
echo "N3: PASS (tampered TPM boots nothing, the firmware boots the next disk)"

# N6: the recovery area. The store is enrolled to one TPM and booted with another.
RS="$BUILD/nat_rec.store"; RTPM="$BUILD/tpmstate_rec"
$BUILD/mkstore --encrypt --dek-out "$BUILD/nat_rec.dek" --recovery "$NAT_KERNEL" --args area-recovery "$RS" >/dev/null
export HOST_FS_DEK_FILE="$BUILD/nat_rec.dek"
# the area sits at the same place after the enrollment; the check needs the factory key
RA_AT=$($BUILD/fsck.runway --key runway-test-passphrase "$RS" 2>&1 | sed -n 's/^recovery area: 2 x [0-9]* blocks at \([0-9]*\).*/\1/p')
RA_BLK=$($BUILD/fsck.runway --key runway-test-passphrase "$RS" 2>&1 | sed -n 's/^recovery area: 2 x \([0-9]*\) blocks.*/\1/p')
[ -n "$RA_AT" ] && [ -n "$RA_BLK" ] || fail "no recovery area in fsck.runway output"
nat_seed "$RS"
nat_slot "$RS" 1 slot-one OK
sh tools/mkesp.sh esp.img "$RS" >/dev/null
rm -rf "$RTPM"
TPM_DIR="$RTPM" RESET_VARS=1 ./tools/run_tpm.sh > "$NLOG" 2>&1 || true
nat_expect "$NLOG" "tpm2: enrolled DEK sealed in Keyslot 1"
rec_boot() { TPM_DIR="$TPM_TAMPER_DIR" EXTRA_DRIVES="-drive format=raw,file=$NIMG2" RESET_VARS=1 ./tools/run_tpm.sh > "$NLOG" 2>&1 || true; }
rec_boot
nat_expect "$NLOG" "SECURITY ALERT: TPM 2.0 unseal failed!"
nat_expect "$NLOG" "RUNWAY: boot store cannot be mounted (0x"
nat_expect "$NLOG" "booting the recovery area"
nat_expect "$NLOG" "[NATIVE] started args=[area-recovery]"
nat_refuse "$NLOG" "RUNWAY: booting"
nat_refuse "$NLOG" "started args=[slot-one]"
nat_refuse "$NLOG" "recovery area copy"
echo "N6a: PASS (a tampered TPM boots the recovery area, no slot is read)"

# a damaged copy A: copy B is empty, so nothing is valid and the firmware boots the next disk
store_get esp.img 0 "$RS"
cp -f "$RS" "$RS.good"
printf '\377' | dd of="$RS" bs=1 seek=$(( (RA_AT + 1) * 4096 + 5 )) conv=notrunc status=none
store_put esp.img 0 "$RS"
rec_boot
nat_expect "$NLOG" "RUNWAY: recovery area invalid (bad hash), not booting"
nat_refuse "$NLOG" "booting the recovery area"
nat_refuse "$NLOG" "started args=[area-recovery]"
nat_expect "$NLOG" "[NATIVE] started args=[]"
echo "N6b: PASS (an invalid bundle returns to the firmware)"

# a second bundle written by rwyctl, then copy B damaged: copy A is the older bundle and is used
cp -f "$RS.good" "$RS"
$BUILD/rwyctl --store "$RS" recovery area set --kernel "$NAT_KERNEL" --args newer-recovery >/dev/null || fail "rwyctl recovery area set"
printf '\377' | dd of="$RS" bs=1 seek=$(( (RA_AT + RA_BLK + 1) * 4096 + 5 )) conv=notrunc status=none
store_put esp.img 0 "$RS"
rec_boot
nat_expect "$NLOG" "RUNWAY: recovery area copy B invalid (bad hash), using copy A"
nat_expect "$NLOG" "[NATIVE] started args=[area-recovery]"
echo "N6c: PASS (a bad newest copy falls back to the older one)"
# N6d: an encrypted mirror whose first member has a bad bundle: the second member's bundle boots
RM0="$BUILD/nat_recm0.store"; RM1="$BUILD/nat_recm1.store"; RE0="$BUILD/nat_recm0.img"; RE1="$BUILD/nat_recm1.img"
$BUILD/mkstore --encrypt --dek-out "$BUILD/nat_recm.dek" --uuid 0a0b0c0d-1111-2222-3333-444455556677 --recovery "$NAT_KERNEL" --args first-recovery "$RM0" >/dev/null
export HOST_FS_DEK_FILE="$BUILD/nat_recm.dek"
nat_seed "$RM0"
nat_slot "$RM0" 1 slot-one OK
cp -f "$RM0" "$RM1"
$BUILD/rwyctl --store "$RM1" recovery area set --kernel "$NAT_KERNEL" --args second-recovery >/dev/null || fail "rwyctl recovery area set"
tests/qemu/mkesp_mirror.sh "$RE0" "$RE1" "$RM0" "$RM1" >/dev/null
rm -rf "$RTPM"
ESP_IMG="$RE0" EXTRA_DRIVES="-drive format=raw,file=$RE1" TPM_DIR="$RTPM" RESET_VARS=1 ./tools/run_tpm.sh > "$NLOG" 2>&1 || true
nat_expect "$NLOG" "active mirror array initialized with 2 device(s)"
store_get $RE0 0 "$RM0"
printf '\377' | dd of="$RM0" bs=1 seek=$(( (RA_AT + 1) * 4096 + 5 )) conv=notrunc status=none
store_put $RE0 0 "$RM0"
ESP_IMG="$RE0" EXTRA_DRIVES="-drive format=raw,file=$RE1 -drive format=raw,file=$NIMG2" TPM_DIR="$TPM_TAMPER_DIR" RESET_VARS=1 ./tools/run_tpm.sh > "$NLOG" 2>&1 || true
nat_expect "$NLOG" "booting the recovery area"
nat_expect "$NLOG" "[NATIVE] started args=[second-recovery]"
nat_refuse "$NLOG" "started args=[first-recovery]"
nat_refuse "$NLOG" "not booting"
echo "N6d: PASS (the first member with a valid bundle boots)"
rm -f "$RM0" "$RM1" "$RE0" "$RE1" "$BUILD/nat_recm.dek"
unset HOST_FS_DEK_FILE
rm -rf "$RS" "$RS.good" "$RTPM" "$BUILD/nat_rec.dek"

# N4: an encrypted mirror, same generation, member 1 holds the newer state record (CONFIG2 FAILED)
MU=0a0b0c0d-1111-2222-3333-444455556666
NM0="$BUILD/nat_tpm_m0.store"; NM1="$BUILD/nat_tpm_m1.store"; NE0="$BUILD/nat_tpm_e0.img"; NE1="$BUILD/nat_tpm_e1.img"
$BUILD/mkstore --encrypt --dek-out "$BUILD/nat_tpm_m.dek" --uuid $MU "$NM0" >/dev/null
export HOST_FS_DEK_FILE="$BUILD/nat_tpm_m.dek"
nat_seed "$NM0"
nat_slot "$NM0" 1 confirm OK
nat_slot "$NM0" 2 confirm OK
cp -f "$NM0" "$NM1"
RWY_KEY=runway-test-passphrase $BUILD/rwyctl --store "$NM1" fail CONFIG2 >/dev/null 2>&1 || fail "rwyctl fail on member 1"
nat_fs "$NM0" mkdir EFI/SPARE
tests/qemu/mkesp_mirror.sh "$NE0" "$NE1" "$NM0" "$NM1" >/dev/null
export TPM_DIR="$BUILD/tpmstate_native_m"; rm -rf "$TPM_DIR"
ESP_IMG="$NE0" EXTRA_DRIVES="-drive format=raw,file=$NE1" RESET_VARS=1 ./tools/run_tpm.sh > "$NLOG" 2>&1 || true
nat_expect "$NLOG" "active mirror array initialized with 2 device(s)"
nat_refuse "$NLOG" "is lagging"
nat_expect "$NLOG" "RUNWAY: CONFIG2 FAILED, skipped (operator)"
nat_refuse "$NLOG" "RUNWAY: booting CONFIG2"
nat_expect "$NLOG" "RUNWAY: booting CONFIG1 (rev 1, OK)"
RDEK=$(sed -n 's/^RUNWAY: tpm2: runtime DEK = //p' "$NLOG" | head -1 | tr -d '\r')
if [ -n "$RDEK" ]; then
    python3 -c "import sys; open('$BUILD/nat_tpm_m.dek', 'wb').write(bytes.fromhex('$RDEK'))"
fi
store_get $NE0 0 "$NM0"; store_get $NE1 0 "$NM1"
[ "$(nat_cat "$NM0" $NAT_T/CONFIG2/STATE)" = FAILED ] && [ "$(nat_cat "$NM1" $NAT_T/CONFIG2/STATE)" = FAILED ] || fail "CONFIG2 is not FAILED on both members"
echo "N4: PASS (the newest record wins on an encrypted mirror, both members agree afterwards)"

# N5: the anti-rollback anchor. The counter is made offline for the PCR 7 the guest will have; the first
# boot adopts it, a slot that becomes FAILED raises it, and an old copy of the store boots RECOVERY only.
[ -n "$PCR7" ] || fail "no PCR 7 value in the log of the enrollment boot"
NA="$BUILD/nat_anc.store"; NAOLD="$BUILD/nat_anc_old.store"; NAFIN="$BUILD/nat_anc_fin.store"; NAPRE="$BUILD/nat_anc_pre.store"
AT="$BUILD/tpmstate_anchor"; ATPRE="$BUILD/tpmstate_anchor_pre"
$BUILD/mkstore --encrypt --dek-out "$BUILD/nat_anc.dek" "$NA" >/dev/null
export HOST_FS_DEK_FILE="$BUILD/nat_anc.dek"
nat_seed "$NA"
nat_slot "$NA" 1 old OK
nat_slot "$NA" 2 current OK
HOST_FS_SYSTEM=1 $BUILD/host_fs "$NA" mkdir $NAT_T/RECOVERY && HOST_FS_SYSTEM=1 $BUILD/host_fs "$NA" link $NAT_T/BLOBS/uki.efi $NAT_T/RECOVERY/KERNEL \
    && HOST_FS_SYSTEM=1 $BUILD/host_fs "$NA" put $NAT_T/RECOVERY/ARGS recovery || fail "RECOVERY"
cp -f "$NA" "$NAPRE"
rm -rf "$AT"; mkdir -p "$AT"
swtpm socket --tpmstate dir="$AT" --server type=unixio,path="$AT/hsock" --ctrl type=unixio,path="$AT/hctrl" \
      --tpm2 --flags not-need-init,startup-clear >"$AT/log" 2>&1 &
HSPID=$!
for i in $(seq 1 50); do [ -S "$AT/hsock" ] && break; sleep 0.1; done
RWY_KEY=runway-test-passphrase $BUILD/rwyctl --store "$NA" --tpm "$AT/hsock" anchor enroll --pcr7 "$PCR7" >/dev/null || fail "anchor enroll"
kill $HSPID; wait $HSPID 2>/dev/null || true
sh tools/mkesp.sh esp.img "$NA" >/dev/null
nat_store_to() { store_get esp.img 0 "$1"; }
export TPM_DIR="$AT"
RESET_VARS=1 ./tools/run_tpm.sh > "$NLOG" 2>&1 || true
nat_expect "$NLOG" "tpm2: enrolled DEK sealed in Keyslot 1"
nat_expect "$NLOG" "RUNWAY: anchor enrolled (epoch "
nat_expect "$NLOG" "RUNWAY: booting CONFIG2 (rev 2, OK)"
nat_refuse "$NLOG" "store rolled back"
RDEK=$(sed -n 's/^RUNWAY: tpm2: runtime DEK = //p' "$NLOG" | head -1 | tr -d '\r')
if [ -n "$RDEK" ]; then
    python3 -c "import sys; open('$BUILD/nat_anc.dek', 'wb').write(bytes.fromhex('$RDEK'))"
fi
nat_store_to "$NA"
nat_slot "$NA" 3 doomed INSTALLED 0
cp -f "$NA" "$NAOLD"
store_put esp.img 0 "$NA"
rm -rf "$ATPRE"; cp -a "$AT" "$ATPRE"
RESET_VARS=0 ./tools/run_tpm.sh > "$NLOG" 2>&1 || true
nat_expect "$NLOG" "RUNWAY: CONFIG3 FAILED, skipped (tries_used_up)"
nat_expect "$NLOG" "RUNWAY: booting CONFIG2 (rev 2, OK)"
nat_refuse "$NLOG" "store rolled back"
nat_store_to "$NAFIN"
echo "N5a: PASS (the counter is adopted at the first boot, a FAILED slot raises it)"

store_put esp.img 0 "$NAOLD"
RESET_VARS=0 ./tools/run_tpm.sh > "$NLOG" 2>&1 || true
nat_expect "$NLOG" "RUNWAY: store rolled back (epoch "
nat_expect "$NLOG" "not booting slots"
nat_expect "$NLOG" "[NATIVE] started args=[recovery]"
nat_refuse "$NLOG" "RUNWAY: booting CONFIG"
nat_refuse "$NLOG" "started args=[old]"
nat_refuse "$NLOG" "started args=[current]"
nat_refuse "$NLOG" "started args=[doomed]"
echo "N5b: PASS (an old copy of the store boots RECOVERY, no slot)"

# a cut between the record write and the increment: the record is one ahead of the counter
store_put esp.img 0 "$NAFIN"
export TPM_DIR="$ATPRE"
RESET_VARS=0 ./tools/run_tpm.sh > "$NLOG" 2>&1 || true
nat_refuse "$NLOG" "store rolled back"
nat_expect "$NLOG" "RUNWAY: booting CONFIG2 (rev 2, OK)"
RESET_VARS=0 ./tools/run_tpm.sh > "$NLOG" 2>&1 || true
nat_refuse "$NLOG" "store rolled back"
nat_expect "$NLOG" "RUNWAY: booting CONFIG2 (rev 2, OK)"
echo "N5c: PASS (a cut between the record and the increment boots normally)"
# N5d: a copy of the whole store made before the enrollment says "not enrolled", but the counter of the
# store exists in the TPM: no slot boots, RECOVERY does.
export TPM_DIR="$AT"
store_put esp.img 0 "$NAPRE"
RESET_VARS=0 ./tools/run_tpm.sh > "$NLOG" 2>&1 || true
nat_expect "$NLOG" "RUNWAY: store rolled back (the record says not enrolled, but the counter exists)"
nat_expect "$NLOG" "not booting slots"
nat_expect "$NLOG" "[NATIVE] started args=[recovery]"
nat_refuse "$NLOG" "RUNWAY: booting CONFIG"
nat_refuse "$NLOG" "started args=[current]"
echo "N5d: PASS (a copy made before the enrollment boots RECOVERY only)"
unset HOST_FS_DEK_FILE
rm -f "$BUILD/nat_tpm.dek" "$BUILD/nat_tpm_m.dek" "$BUILD/nat_anc.dek"
rm -rf "$BUILD/tpmstate_native" "$BUILD/tpmstate_native_m" "$NST" "$NIMG2" "$NM0" "$NM1" "$NE0" "$NE1" "$NA" "$NAOLD" "$NAFIN" "$NAPRE" "$AT" "$ATPRE"

echo "--- 5. Restoring Clean Unencrypted Store for Standard Runs ---"
$BUILD/mkstore "$BUILD/RUNWAY.DAT"
sh tools/mkesp.sh esp.img
rm -rf "$TPM_TEST_DIR" "$TPM_TAMPER_DIR"

echo "================================================================="
echo "TEST-TPM: PASS (All phases verified: enrollment, unseal, tamper)"
echo "================================================================="
