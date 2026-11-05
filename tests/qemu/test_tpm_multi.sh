#!/bin/sh
# tests/qemu/test_tpm_multi.sh — TPM 2.0 sealing of encrypted stores in the
# mirror and multi-volume configurations.
# SPDX-License-Identifier: Apache-2.0
#   M: two-disk mirror of one encrypted store. Enroll boot seals once and the
#      keyslot header lands on BOTH members; next boot unseals; a stale
#      member (gen behind) is caught up through the unsealed DEK; foreign TPM
#      halts boot.
#      Partial enrollment (power loss after master sealed, member 1 still on the
#      factory keyslot): lagging+factory member, and same-generation factory
#      header; both must converge, and member 1 must then unseal on its own.
#      Plus: master on factory keyslot with an enrolled lagging member, TPM
#      absent, PCR 7 changed on the same TPM, partial multi-volume enrollment.
#   V: ESP + 2 config + XBOOTLDR, all encrypted with distinct UUIDs and
#      distinct DEKs (a group unsealing another group's DEK can't mount). Every
#      group enrolls, then every group unseals; foreign TPM halts boot.
set -e
export PATH="$PATH:/sbin:/usr/sbin"
BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
[ "$ARCH" = x86_64 ] || { echo "test-tpm-multi: SKIP (AAVMF has no TPM2 stack)"; exit 0; }
fail() { echo "FAIL: $1"; [ -n "$2" ] && tail -n 60 "$2"; exit 1; }
count() { grep -c -F "$1" "$2" || true; }

SCRATCH=$(mktemp -d /var/tmp/rwy-tpmm-XXXXXX)
cleanup_all() {
    pkill -9 -f "swtpm.*$SCRATCH" 2>/dev/null || true
    rm -rf "$SCRATCH"
    leftovers=$(ls -d /var/tmp/rwy-tpm* 2>/dev/null || true)
    if [ -n "$leftovers" ]; then
        echo "WARNING: Leftovers detected in /var/tmp: $leftovers"
    fi
}
trap cleanup_all EXIT INT TERM

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

# slot_state FILE_IMG SLOT [STORE] -> keyslot[SLOT].state in both superblock copies (must agree)
slot_state() {
    store_get "$1" "${3:-0}" $BUILD/tm_x.store
    python3 - "$BUILD/tm_x.store" "$2" <<'P'
import struct, sys
f = open(sys.argv[1], 'rb'); n = int(sys.argv[2])
v = set()
for base in (512, 4096 + 512):
    f.seek(base + 64 + 512 * n + 4); v.add(struct.unpack('<I', f.read(4))[0])
print(v.pop() if len(v) == 1 else 'DIVERGED')
P
}
SLOT_ACTIVE=1; SLOT_EMPTY=0
# ponytail: assumes RWY_KEYSLOT_STATE_ACTIVE == 1, EMPTY == 0 (matches mkstore slot0 = active)

echo "--- M1: mirror enrollment ---"
rm -rf $BUILD/tm_tpm_a $BUILD/tm_tpm_bad
$BUILD/mkstore --encrypt $BUILD/tm.store >/dev/null
tests/qemu/mkesp_mirror.sh esp0.img esp1.img $BUILD/tm.store $BUILD/tm.store >/dev/null
export ESP_IMG=esp0.img
export EXTRA_DRIVES="-drive format=raw,file=esp1.img"
TPM_DIR=$BUILD/tm_tpm_a RESET_VARS=1 ./tools/run_tpm.sh > $BUILD/tm_m1.log 2>&1 || true
L=$BUILD/tm_m1.log
grep -q "tpm2: enrollment phase" $L || fail "no enrollment" $L
[ "$(count 'tpm2: enrollment phase' $L)" = 1 ] || fail "enrolled more than once" $L
grep -q "reencrypt: copying" $L || fail "no reencrypt copying" $L
grep -q "reencrypt: finalizing" $L || fail "no reencrypt finalizing" $L
grep -q "active mirror array initialized with 2 device(s)" $L || fail "mirror not formed" $L
grep -q "RESULT: PASS" $L || fail "boot failed" $L
for img in esp0.img esp1.img; do
    [ "$(slot_state $img 1)" = $SLOT_ACTIVE ] || fail "$img: TPM keyslot not active"
    [ "$(slot_state $img 0)" = $SLOT_EMPTY ] || fail "$img: factory keyslot not cleared"
done
echo "M1: PASS (sealed once, keyslot replicated to both members)"

echo "--- M2: mirror unseal + stale-member catch-up ---"
cp -f esp1.img $BUILD/tm_esp1_old.img     # snapshot: enrolled, older generation
TPM_DIR=$BUILD/tm_tpm_a RESET_VARS=0 ./tools/run_tpm.sh > $BUILD/tm_m2a.log 2>&1 || true
L=$BUILD/tm_m2a.log
grep -q "hardware unseal successful" $L || fail "no unseal" $L
grep -q "tpm2: enrollment phase" $L && fail "re-enrolled" $L
grep -q "RESULT: PASS" $L || fail "boot failed" $L
cp -f $BUILD/tm_esp1_old.img esp1.img     # member 1 now lags member 0
TPM_DIR=$BUILD/tm_tpm_a RESET_VARS=0 ./tools/run_tpm.sh > $BUILD/tm_m2b.log 2>&1 || true
L=$BUILD/tm_m2b.log
grep -q "hardware unseal successful" $L || fail "no unseal (lagging)" $L
grep -q "member 1 is lagging" $L || fail "lag not detected" $L
grep -q "mirror: catch-up synchronization complete" $L || fail "no catch-up" $L
grep -q "RESULT: PASS" $L || fail "boot failed after catch-up" $L
echo "M2: PASS"

echo "--- M3: mirror tamper (foreign TPM) ---"
TPM_DIR=$BUILD/tm_tpm_bad RESET_VARS=0 ./tools/run_tpm.sh > $BUILD/tm_m3.log 2>&1 || true
grep -q "SECURITY ALERT: TPM 2.0 unseal failed" $BUILD/tm_m3.log || fail "no alert" $BUILD/tm_m3.log
grep -q "RESULT: PASS" $BUILD/tm_m3.log && fail "booted on foreign TPM"
echo "M3: PASS"
echo "--- M4: partial enrollment, member 1 lagging + factory keyslot ---"
# fresh pre-enrollment pair, enroll via member 0 only, then restore member 1
$BUILD/mkstore --encrypt $BUILD/tm.store >/dev/null
tests/qemu/mkesp_mirror.sh esp0.img esp1.img $BUILD/tm.store $BUILD/tm.store >/dev/null
cp -f esp1.img $BUILD/tm_esp1_factory.img
rm -rf $BUILD/tm_tpm_a
TPM_DIR=$BUILD/tm_tpm_a RESET_VARS=1 ./tools/run_tpm.sh > $BUILD/tm_m4a.log 2>&1 || true
grep -q "tpm2: enrollment phase" $BUILD/tm_m4a.log || fail "no enrollment" $BUILD/tm_m4a.log
cp -f $BUILD/tm_esp1_factory.img esp1.img
[ "$(slot_state esp1.img 1)" = $SLOT_EMPTY ] || fail "member 1 not factory"
TPM_DIR=$BUILD/tm_tpm_a RESET_VARS=0 ./tools/run_tpm.sh > $BUILD/tm_m4b.log 2>&1 || true
L=$BUILD/tm_m4b.log
grep -q "hardware unseal successful" $L || fail "no unseal" $L
grep -q "member 1 is lagging" $L || fail "lag not detected" $L
grep -q "RESULT: PASS" $L || fail "boot failed" $L
[ "$(slot_state esp1.img 1)" = $SLOT_ACTIVE ] || fail "member 1 TPM keyslot not converged"
[ "$(slot_state esp1.img 0)" = $SLOT_EMPTY ] || fail "member 1 factory keyslot not cleared"
echo "M4: PASS"

echo "--- M5: partial enrollment, same generation, member 1 factory header ---"
# revert only member 1's keyslot header (both superblock copies) to factory
cp -f esp1.img $BUILD/tm_esp1_m4.img
store_get esp1.img 0 $BUILD/tm_x.store
python3 - $BUILD/tm_x.store $BUILD/tm.store <<'P'
import sys
d = bytearray(open(sys.argv[1], 'rb').read()); fac = open(sys.argv[2], 'rb').read()
for b in (512, 4096 + 512): d[b:b + 1600] = fac[b:b + 1600]
open(sys.argv[1], 'wb').write(d)
P
store_put esp1.img 0 $BUILD/tm_x.store
[ "$(slot_state esp1.img 1)" = $SLOT_EMPTY ] || fail "member 1 header not reverted"
TPM_DIR=$BUILD/tm_tpm_a RESET_VARS=0 ./tools/run_tpm.sh > $BUILD/tm_m5.log 2>&1 || true
L=$BUILD/tm_m5.log
grep -q "mirror member has a different DEK than master" $L || fail "different DEK not detected" $L
grep -q "RESULT: PASS" $L && fail "booted with mismatched DEK"
echo "M5: PASS (same generation with factory header rejected as DEK mismatch)"

echo "--- M6: member 1 boots alone after recovery ---"
cp -f $BUILD/tm_esp1_m4.img esp1.img
ESP_IMG=esp1.img EXTRA_DRIVES= TPM_DIR=$BUILD/tm_tpm_a RESET_VARS=0 ./tools/run_tpm.sh > $BUILD/tm_m6.log 2>&1 || true
L=$BUILD/tm_m6.log
grep -q "hardware unseal successful" $L || fail "no unseal" $L
grep -q "RESULT: PASS" $L || fail "boot failed" $L
echo "M6: PASS"

echo "--- M7: mirror degraded member on first boot ---"
rm -rf $BUILD/tm_tpm_deg
$BUILD/mkstore --encrypt $BUILD/tm_deg_old.store >/dev/null
cp -f $BUILD/tm_deg_old.store $BUILD/tm_deg.store
HOST_FS_PASSPHRASE=runway-test-passphrase $BUILD/host_fs $BUILD/tm_deg.store mkdir /FOO
tests/qemu/mkesp_mirror.sh esp0.img esp1.img $BUILD/tm_deg.store $BUILD/tm_deg_old.store >/dev/null
export ESP_IMG=esp0.img
if [ "$STORE" = block ]; then
    # Member 1 is a disk of its own that holds only the store partition, attached read-only: virtio-blk tells the firmware
    store_get esp1.img 0 $BUILD/tm_ro.store
    truncate -s $(($(stat -c %s $BUILD/tm_ro.store) / 1048576 + 2))M $BUILD/tm_ro.img
    printf 'label: gpt\nstart=2048, type=%s\n' $XBL_TYPE | sfdisk $BUILD/tm_ro.img >/dev/null
    dd if=$BUILD/tm_ro.store of=$BUILD/tm_ro.img bs=1M seek=1 conv=notrunc status=none
    export EXTRA_DRIVES="-drive if=none,id=ro1,format=raw,file=$BUILD/tm_ro.img,readonly=on -device virtio-blk-pci,drive=ro1"
else
    mattrib -i "esp1.img@@$(img_part_off esp1.img 1)" +r ::RUNWAY.DAT
    export EXTRA_DRIVES="-drive format=raw,file=esp1.img"
fi
TPM_DIR=$BUILD/tm_tpm_deg RESET_VARS=1 ./tools/run_tpm.sh > $BUILD/tm_m7.log 2>&1 || true
L=$BUILD/tm_m7.log
grep -q "mirror degraded" $L || fail "no mirror degraded warning" $L
grep -q "first-boot re-encryption failed, retrying at next boot" $L || fail "no retry logged" $L
grep -q "RESULT: PASS" $L || fail "boot failed on degraded mirror" $L
[ "$(slot_state esp0.img 0)" = $SLOT_ACTIVE ] || fail "esp0.img: factory keyslot should still be active"
echo "M7: PASS (degraded mirror member delays re-encryption, boots on factory key)"

echo "--- M8: mirror power cut during re-encryption ---"
rm -rf $BUILD/tm_tpm_cut
$BUILD/mkstore --encrypt $BUILD/tm_cut.store >/dev/null
tests/qemu/mkesp_mirror.sh esp0.img esp1.img $BUILD/tm_cut.store $BUILD/tm_cut.store >/dev/null
export ESP_IMG=esp0.img
export EXTRA_DRIVES="-drive format=raw,file=esp1.img"
TPM_DIR=$BUILD/tm_tpm_cut RESET_VARS=1 run_tpm_kill_on "reencrypt: finalizing" "$BUILD/tm_m8_cut.log"
# Reboot mirror: resumes pending finalize across both members
TPM_DIR=$BUILD/tm_tpm_cut RESET_VARS=0 ./tools/run_tpm.sh > "$BUILD/tm_m8_reboot.log" 2>&1 || true
L=$BUILD/tm_m8_reboot.log
grep -q "reencrypt: resuming pending finalize from previous boot" $L || fail "pending finalize not resumed on mirror" $L
grep -q "RESULT: PASS" $L || fail "mirror reboot failed after cut" $L
for img in esp0.img esp1.img; do
    [ "$(slot_state $img 1)" = $SLOT_ACTIVE ] || fail "$img: TPM keyslot not active after resume"
    [ "$(slot_state $img 0)" = $SLOT_EMPTY ] || fail "$img: factory keyslot not cleared after resume"
done
# Next boot unseals cleanly on mirror
TPM_DIR=$BUILD/tm_tpm_cut RESET_VARS=0 ./tools/run_tpm.sh > "$BUILD/tm_m8_norm.log" 2>&1 || true
L=$BUILD/tm_m8_norm.log
grep -q "hardware unseal successful" $L || fail "no unseal" $L
grep -q "reencrypt: resuming pending finalize" $L && fail "resumed again" $L
grep -q "RESULT: PASS" $L || fail "boot failed" $L
echo "M8: PASS (mirror power cut before finalize resumed cleanly on both members)"

unset EXTRA_DRIVES ESP_IMG

echo "--- V1: multi-volume enrollment (4 encrypted groups) ---"
IMG=$BUILD/tm_multivol.img
ENCRYPT=1 tests/qemu/mkdisk_multivol.sh $IMG 2 1 >/dev/null
ESP_IMG=$IMG TPM_DIR=$BUILD/tm_tpm_v RESET_VARS=1 ./tools/run_tpm.sh > $BUILD/tm_v1.log 2>&1 || true
L=$BUILD/tm_v1.log
grep -q "scan: 4 volume group(s) discovered" $L || fail "expected 4 groups" $L
[ "$(count 'tpm2: enrollment phase' $L)" = 4 ] || fail "expected 4 enrollments" $L
grep -q "chain-loaded application: OK" $L || fail "chain app" $L
echo "V1: PASS"

echo "--- V2: multi-volume unseal ---"
ESP_IMG=$IMG TPM_DIR=$BUILD/tm_tpm_v RESET_VARS=0 ./tools/run_tpm.sh > $BUILD/tm_v2.log 2>&1 || true
L=$BUILD/tm_v2.log
[ "$(count 'hardware unseal successful' $L)" = 4 ] || fail "expected 4 unseals" $L
grep -q "tpm2: enrollment phase" $L && fail "re-enrolled" $L
grep -q "chain-loaded application: OK" $L || fail "chain app" $L
echo "V2: PASS"

echo "--- V3: multi-volume tamper ---"
ESP_IMG=$IMG TPM_DIR=$BUILD/tm_tpm_bad RESET_VARS=0 ./tools/run_tpm.sh > $BUILD/tm_v3.log 2>&1 || true
grep -q "SECURITY ALERT: TPM 2.0 unseal failed" $BUILD/tm_v3.log || fail "no alert" $BUILD/tm_v3.log
grep -q "chain-loaded application: OK" $BUILD/tm_v3.log && fail "booted on foreign TPM"
echo "V3: PASS"

# ---- hardening scenarios ----------------------------------------------------
# edit_store IMG STORE PYCODE: run PYCODE on the bytes `d` of store STORE (default 0) inside IMG
edit_store() {
    store_get "$1" "${2:-0}" $BUILD/tm_e.store
    python3 -c 'import sys; d = bytearray(open(sys.argv[1], "rb").read()); exec(sys.argv[2]); open(sys.argv[1], "wb").write(d)' $BUILD/tm_e.store "$3"
    store_put "$1" "${2:-0}" $BUILD/tm_e.store
}
# boot IMG TPMDIR RESET LOG [extra drive]: one TPM boot
tboot() { ESP_IMG=$1 EXTRA_DRIVES="${5:-}" TPM_DIR=$BUILD/$2 RESET_VARS=$3 ./tools/run_tpm.sh > "$4" 2>&1 || true; }
# safe_or_unsealed LOG: a boot that reaches RESULT: PASS must have unlocked via the TPM
safe_or_unsealed() { ! grep -q "RESULT: PASS" "$1" || grep -q "hardware unseal successful" "$1"; }

echo "--- H1: corrupt keyslot header must not fall open to plaintext ---"
$BUILD/mkstore --encrypt $BUILD/tm.store >/dev/null
sh tools/mkesp.sh esp0.img $BUILD/tm.store >/dev/null
rm -rf $BUILD/tm_tpm_h
tboot esp0.img tm_tpm_h 1 $BUILD/tm_h1a.log
grep -q "RESULT: PASS" $BUILD/tm_h1a.log || fail "single-disk enrollment" $BUILD/tm_h1a.log
cp -f esp0.img $BUILD/tm_h1_good.img
# A: copy A of header corrupted (CRC mismatch), copy B intact
edit_store esp0.img "" 'd[512+64+512+100] ^= 0xFF'
tboot esp0.img tm_tpm_h 0 $BUILD/tm_h1b.log
safe_or_unsealed $BUILD/tm_h1b.log || fail "H1 copy A corrupt: booted without unlocking" $BUILD/tm_h1b.log
# B: both copies corrupted: must halt
cp -f $BUILD/tm_h1_good.img esp0.img
edit_store esp0.img "" 'd[512+64+512+100] ^= 0xFF; d[4096+512+64+512+100] ^= 0xFF'
tboot esp0.img tm_tpm_h 0 $BUILD/tm_h1c.log
grep -q "RESULT: PASS" $BUILD/tm_h1c.log && fail "H1 both copies corrupt: booted" $BUILD/tm_h1c.log
echo "H1: PASS"

echo "--- H2: mirror members with different DEKs must not be mirrored ---"
U=00000000-0000-4000-8000-0000000000aa
$BUILD/mkstore --encrypt --uuid $U $BUILD/tm.store >/dev/null
$BUILD/mkstore --encrypt --uuid $U $BUILD/tm_b.store >/dev/null
tests/qemu/mkesp_mirror.sh esp0.img esp1.img $BUILD/tm.store $BUILD/tm_b.store >/dev/null
store_get esp1.img 0 $BUILD/tm_before.store
rm -rf $BUILD/tm_tpm_h
tboot esp0.img tm_tpm_h 1 $BUILD/tm_h2.log "-drive format=raw,file=esp1.img"
grep -q "active mirror array initialized with 2 device(s)" $BUILD/tm_h2.log && fail "H2 mirrored members with different DEKs" $BUILD/tm_h2.log
store_get esp1.img 0 $BUILD/tm_after.store
cmp -s -n 8192 $BUILD/tm_before.store $BUILD/tm_after.store || fail "H2 foreign member's keyslot header was overwritten"
echo "H2: PASS"

echo "--- H3: mirror with one encrypted and one plaintext member ---"
$BUILD/mkstore --encrypt --uuid $U $BUILD/tm.store >/dev/null
$BUILD/mkstore --uuid $U $BUILD/tm_b.store >/dev/null
tests/qemu/mkesp_mirror.sh esp0.img esp1.img $BUILD/tm.store $BUILD/tm_b.store >/dev/null
rm -rf $BUILD/tm_tpm_h
tboot esp0.img tm_tpm_h 1 $BUILD/tm_h3.log "-drive format=raw,file=esp1.img"
grep -q "active mirror array initialized with 2 device(s)" $BUILD/tm_h3.log && fail "H3 mirrored encrypted with plaintext member" $BUILD/tm_h3.log
echo "H3: PASS"

echo "--- H4: plaintext ESP + encrypted config/XBOOTLDR stores ---"
IMG=$BUILD/tm_multivol.img
ENCRYPT=1 ENCRYPT_SKIP="0" tests/qemu/mkdisk_multivol.sh $IMG 2 1 >/dev/null
rm -rf $BUILD/tm_tpm_h
tboot $IMG tm_tpm_h 1 $BUILD/tm_h4a.log
L=$BUILD/tm_h4a.log
[ "$(count 'tpm2: enrollment phase' $L)" = 3 ] || fail "expected 3 enrollments" $L
grep -q "chain-loaded application: OK" $L || fail "chain app" $L
tboot $IMG tm_tpm_h 0 $BUILD/tm_h4b.log
L=$BUILD/tm_h4b.log
[ "$(count 'hardware unseal successful' $L)" = 3 ] || fail "expected 3 unseals" $L
grep -q "chain-loaded application: OK" $L || fail "chain app (unseal)" $L
echo "H4a: PASS (plaintext ESP)"
ENCRYPT=1 ENCRYPT_SKIP="2" tests/qemu/mkdisk_multivol.sh $IMG 2 1 >/dev/null
rm -rf $BUILD/tm_tpm_h
tboot $IMG tm_tpm_h 1 $BUILD/tm_h4c.log
L=$BUILD/tm_h4c.log
[ "$(count 'tpm2: enrollment phase' $L)" = 3 ] || fail "expected 3 enrollments (plain config2)" $L
grep -q "chain-loaded application: OK" $L || fail "chain app (plain config2)" $L
echo "H4: PASS"

echo "--- H5: keyslot headers swapped between two groups ---"
# the boot group carries a native slot, so this also checks that a group which cannot
# be mounted does not stop the boot group from booting its slot
. tests/qemu/native_lib.sh
ENCRYPT=1 tests/qemu/mkdisk_multivol.sh $IMG 2 1 >/dev/null
store_get $IMG 0 $BUILD/tm_n.store
export HOST_FS_PASSPHRASE=runway-test-passphrase
nat_seed $BUILD/tm_n.store; nat_slot $BUILD/tm_n.store 1 confirm OK
unset HOST_FS_PASSPHRASE
store_put $IMG 0 $BUILD/tm_n.store
rm -rf $BUILD/tm_tpm_h
tboot $IMG tm_tpm_h 1 $BUILD/tm_h5a.log
[ "$(count 'tpm2: enrollment phase' $BUILD/tm_h5a.log)" = 4 ] || fail "baseline enrollment" $BUILD/tm_h5a.log
nat_expect $BUILD/tm_h5a.log "[NATIVE] started args=[confirm]"
store_get $IMG 1 $BUILD/tm_s2.store
store_get $IMG 2 $BUILD/tm_s3.store
python3 - $BUILD/tm_s2.store $BUILD/tm_s3.store <<'P'
import sys
a = bytearray(open(sys.argv[1], 'rb').read()); b = bytearray(open(sys.argv[2], 'rb').read())
for o in (512, 4096 + 512):
    a[o:o + 1600], b[o:o + 1600] = b[o:o + 1600], a[o:o + 1600]
open(sys.argv[1], 'wb').write(a); open(sys.argv[2], 'wb').write(b)
P
store_put $IMG 1 $BUILD/tm_s2.store
store_put $IMG 2 $BUILD/tm_s3.store
tboot $IMG tm_tpm_h 0 $BUILD/tm_h5b.log
[ "$(count '), skipped' $BUILD/tm_h5b.log)" = 2 ] || fail "H5: expected the two swapped groups to be skipped" $BUILD/tm_h5b.log
nat_expect $BUILD/tm_h5b.log "RUNWAY: booting CONFIG1 (rev 1, OK)"
nat_refuse $BUILD/tm_h5b.log "boot store cannot be mounted"
echo "H5: PASS (the swapped groups are skipped, the boot group boots its slot)"

echo "--- H6: eight encrypted groups (TPM transient handle pressure) ---"
ENCRYPT=1 tests/qemu/mkdisk_multivol.sh $IMG 7 0 >/dev/null
rm -rf $BUILD/tm_tpm_h
tboot $IMG tm_tpm_h 1 $BUILD/tm_h6a.log
L=$BUILD/tm_h6a.log
grep -q "scan: 8 volume group(s) discovered" $L || fail "expected 8 groups" $L
[ "$(count 'tpm2: enrollment phase' $L)" = 8 ] || fail "expected 8 enrollments" $L
grep -q "chain-loaded application: OK" $L || fail "chain app" $L
tboot $IMG tm_tpm_h 0 $BUILD/tm_h6b.log
L=$BUILD/tm_h6b.log
[ "$(count 'hardware unseal successful' $L)" = 8 ] || fail "expected 8 unseals" $L
grep -q "chain-loaded application: OK" $L || fail "chain app (unseal)" $L
echo "H6: PASS"

# ---- recovery / environment scenarios ---------------------------------------
# set_hdr IMG STORE FILE: overwrite both keyslot header copies with FILE's (1600 B at 512)
set_hdr() { edit_store "$1" "$2" "f = open('$3', 'rb').read()
for o in (512, 4096 + 512): d[o:o + 1600] = f[512:512 + 1600]"; }
# save_hdr IMG STORE: keep the factory keyslot header of a volume, before the first boot enrolls it
save_hdr() { store_get "$1" $2 $BUILD/tm_fac_store-$2.store; }

echo "--- P1: partial multi-volume enrollment (2 of 4 groups still factory) ---"
ENCRYPT=1 tests/qemu/mkdisk_multivol.sh $IMG 2 1 >/dev/null
save_hdr $IMG 2; save_hdr $IMG 3
rm -rf $BUILD/tm_tpm_p
tboot $IMG tm_tpm_p 1 $BUILD/tm_p1a.log
[ "$(count 'tpm2: enrollment phase' $BUILD/tm_p1a.log)" = 4 ] || fail "baseline enrollment" $BUILD/tm_p1a.log
store_put $IMG 2 $BUILD/tm_fac_store-2.store
store_put $IMG 3 $BUILD/tm_fac_store-3.store
tboot $IMG tm_tpm_p 0 $BUILD/tm_p1b.log
L=$BUILD/tm_p1b.log
[ "$(count 'tpm2: enrollment phase' $L)" = 2 ] || fail "expected 2 enrollments" $L
[ "$(count 'hardware unseal successful' $L)" = 2 ] || fail "expected 2 unseals" $L
grep -q "chain-loaded application: OK" $L || fail "chain app" $L
tboot $IMG tm_tpm_p 0 $BUILD/tm_p1c.log
L=$BUILD/tm_p1c.log
[ "$(count 'hardware unseal successful' $L)" = 4 ] || fail "expected 4 unseals after recovery" $L
grep -q "tpm2: enrollment phase" $L && fail "re-enrolled" $L
echo "P1: PASS"

echo "--- P2: master on factory keyslot, lagging member already enrolled ---"
$BUILD/mkstore --encrypt $BUILD/tm.store >/dev/null
tests/qemu/mkesp_mirror.sh esp0.img esp1.img $BUILD/tm.store $BUILD/tm.store >/dev/null
rm -rf $BUILD/tm_tpm_p
tboot esp0.img tm_tpm_p 1 $BUILD/tm_p2a.log "-drive format=raw,file=esp1.img"
grep -q "tpm2: enrollment phase" $BUILD/tm_p2a.log || fail "no enrollment" $BUILD/tm_p2a.log
cp -f esp1.img $BUILD/tm_esp1_old.img                       # enrolled, older generation
sed -n 's/.*runtime DEK = \([0-9a-f]*\).*/\1/p' $BUILD/tm_p2a.log | head -n 1 | xxd -r -p > $BUILD/tm_p2.dek
tboot esp0.img tm_tpm_p 0 $BUILD/tm_p2b.log "-drive format=raw,file=esp1.img"   # both advance
cp -f $BUILD/tm_esp1_old.img esp1.img                       # member 1 lags, enrolled
store_get esp0.img 0 $BUILD/tm_p2_master.store
$BUILD/rwyctl --store $BUILD/tm_p2_master.store --dek-file $BUILD/tm_p2.dek reencrypt --new-passphrase runway-test-passphrase >/dev/null
store_put esp0.img 0 $BUILD/tm_p2_master.store
[ "$(slot_state esp0.img 0)" = $SLOT_ACTIVE ] || fail "master not factory"
[ "$(slot_state esp1.img 1)" = $SLOT_ACTIVE ] || fail "member not enrolled"
tboot esp0.img tm_tpm_p 0 $BUILD/tm_p2c.log "-drive format=raw,file=esp1.img"
L=$BUILD/tm_p2c.log
grep -q "tpm2: enrollment phase" $L || fail "master did not enroll" $L
grep -q "member 1 is lagging" $L || fail "lag not detected" $L
grep -q "RESULT: PASS" $L || fail "boot failed" $L
for img in esp0.img esp1.img; do
    [ "$(slot_state $img 1)" = $SLOT_ACTIVE ] && [ "$(slot_state $img 0)" = $SLOT_EMPTY ] || fail "$img not converged"
done
tboot esp0.img tm_tpm_p 0 $BUILD/tm_p2d.log "-drive format=raw,file=esp1.img"
grep -q "hardware unseal successful" $BUILD/tm_p2d.log && grep -q "RESULT: PASS" $BUILD/tm_p2d.log || fail "unseal after recovery" $BUILD/tm_p2d.log
tboot esp1.img tm_tpm_p 0 $BUILD/tm_p2e.log
grep -q "hardware unseal successful" $BUILD/tm_p2e.log && grep -q "RESULT: PASS" $BUILD/tm_p2e.log || fail "member 1 alone" $BUILD/tm_p2e.log
echo "P2: PASS"

echo "--- P3: TPM missing ---"
$BUILD/mkstore --encrypt $BUILD/tm.store >/dev/null
sh tools/mkesp.sh esp0.img $BUILD/tm.store >/dev/null
cp -f esp0.img $BUILD/tm_factory.img
(export NO_TPM=1; tboot esp0.img tm_tpm_p 1 $BUILD/tm_p3a.log)
grep -q "TPM 2.0 required for enrollment but not available" $BUILD/tm_p3a.log || fail "factory, no TPM" $BUILD/tm_p3a.log
grep -q "RESULT: PASS" $BUILD/tm_p3a.log && fail "factory store booted without TPM"
[ "$(slot_state esp0.img 0)" = $SLOT_ACTIVE ] || fail "factory keyslot modified without TPM"
rm -rf $BUILD/tm_tpm_p
tboot esp0.img tm_tpm_p 1 $BUILD/tm_p3b.log
grep -q "RESULT: PASS" $BUILD/tm_p3b.log || fail "enrollment" $BUILD/tm_p3b.log
(export NO_TPM=1; tboot esp0.img tm_tpm_p 1 $BUILD/tm_p3c.log)
grep -q "TPM 2.0 required for unsealing but not available" $BUILD/tm_p3c.log || fail "enrolled, no TPM" $BUILD/tm_p3c.log
grep -q "RESULT: PASS" $BUILD/tm_p3c.log && fail "enrolled store booted without TPM"
echo "P3: PASS"

echo "--- P4: PCR 7 change on the same TPM ---"
# esp0.img is enrolled against $BUILD/tm_tpm_p; same firmware config unseals, other Secure Boot vars do not
tboot esp0.img tm_tpm_p 1 $BUILD/tm_p4a.log
grep -q "hardware unseal successful" $BUILD/tm_p4a.log || fail "control: same PCR 7 must unseal" $BUILD/tm_p4a.log
(export OVMF_VARS_TEMPLATE=/usr/share/OVMF/OVMF_VARS_4M.ms.fd; tboot esp0.img tm_tpm_p 1 $BUILD/tm_p4b.log)
grep -q "SECURITY ALERT: TPM 2.0 unseal failed" $BUILD/tm_p4b.log || fail "PCR 7 change not detected" $BUILD/tm_p4b.log
grep -q "RESULT: PASS" $BUILD/tm_p4b.log && fail "booted with changed PCR 7"
tboot esp0.img tm_tpm_p 1 $BUILD/tm_p4c.log   # restore default vars for later runs
grep -q "hardware unseal successful" $BUILD/tm_p4c.log || fail "unseal after restoring vars" $BUILD/tm_p4c.log
echo "P4: PASS"

rm -rf $BUILD/tm_tpm_a $BUILD/tm_tpm_v $BUILD/tm_tpm_bad $BUILD/tm_tpm_deg $BUILD/tm_tpm_cut $BUILD/tm.store $BUILD/tm_x.store $BUILD/tm_deg.store $BUILD/tm_deg_old.store $BUILD/tm_cut.store $BUILD/tm_esp1_old.img $BUILD/tm_ro.img $BUILD/tm_ro.store $BUILD/tm_esp1_factory.img $BUILD/tm_esp1_m4.img $BUILD/tm_tpm_h $BUILD/tm_b.store $BUILD/tm_e.store $BUILD/tm_s2.store $BUILD/tm_s3.store $BUILD/tm_before.store $BUILD/tm_after.store $BUILD/tm_h1_good.img $BUILD/tm_tpm_p $BUILD/tm_fac_store-*.store $BUILD/tm_factory.img $BUILD/tm_n.store $BUILD/tm_p2.dek $BUILD/tm_p2_master.store $IMG
$BUILD/mkstore $BUILD/RUNWAY.DAT >/dev/null
sh tools/mkesp.sh esp.img >/dev/null
echo "TEST-TPM-MULTI: PASS"
