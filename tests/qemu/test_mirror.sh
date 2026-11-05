#!/bin/sh
# tests/qemu/test_mirror.sh — Automated integration test suite for Runway RAID/Mirroring.
# SPDX-License-Identifier: Apache-2.0
#
# Validates:
#   Scenario A (Synchronized Boot): Boot with both drives attached, verify both
#              members are discovered and enrolled, and mutations commit across both.
#   Scenario B (Lagging Replica Catch-up): Simulate lagging replica (Disk 0 at Gen 1,
#              Disk 1 updated ahead to Gen 2 via host write); verify Disk 0 is detected
#              as lagging, synchronized to Gen 2, and mounted cleanly.
#   Scenarios D-K: native boot on a mirror, I adds a WDT/ application,
#              J reads RunwayBootMembers from the payload, K boots with a member that cannot be written.
#   Scenario C (Post-Boot Integrity): Run build/tools/fsck.runway independently on both
#              esp0.img and esp1.img after each scenario to verify exact matching
#              generation, identical committed roots, and 0 consistency errors.
set -e

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
LOG_SYNC="$BUILD/mirror_synced.log"
LOG_CATCHUP="$BUILD/mirror_catchup.log"

echo "================================================================="
echo "       Runway RAID & Multi-Disk Mirroring Integration Suite        "
echo "================================================================="

mkdir -p "$BUILD"

# The member UUID of a store: both slots hold the same value
member_uuid() { "$BUILD/fsck.runway" "$1" | sed -n 's/^slot [AB]: member uuid //p' | sort -u; }
# fsck.runway of the store of a disk image
check_disk() { store_get "$1" 0 "$BUILD/mirror_chk.store"; "$BUILD/fsck.runway" "$BUILD/mirror_chk.store"; }

echo "--- 1. Provisioning Clean Backing Store (Gen 1) ---"
$BUILD/mkstore --arch "$ARCH" "$BUILD/RUNWAY.DAT"

echo "--- 2. Scenario A: Synchronized Dual-Drive Boot ---"
tests/qemu/mkesp_mirror.sh esp0.img esp1.img "$BUILD/RUNWAY.DAT" "$BUILD/RUNWAY.DAT" >/dev/null

qemu_vars_copy

"$QEMU_BIN" \
    -nographic \
    $(qemu_args) \
    -drive format=raw,file=esp0.img \
    -drive format=raw,file=esp1.img \
    -serial stdio -display none > "$LOG_SYNC" 2>&1 || true

if ! grep -qF "RUNWAY: $STORE_LOG" "$LOG_SYNC" || grep -qF "$STORE_LOG_OTHER" "$LOG_SYNC"; then
    echo "FAIL: the log does not say '$STORE_LOG' alone"
    cat "$LOG_SYNC"
    exit 1
fi

if ! grep -q "mirror: booted store UUID" "$LOG_SYNC"; then
    echo "FAIL: Booted store UUID not found in log"
    cat "$LOG_SYNC"
    exit 1
fi

if ! grep -q "mirror: discovered member 1" "$LOG_SYNC"; then
    echo "FAIL: Expected member 1 discovery in log"
    cat "$LOG_SYNC"
    exit 1
fi

if ! grep -q "mirror: authoritative master is member 0 (generation 1" "$LOG_SYNC"; then
    echo "FAIL: Member 0 not selected as authoritative master"
    cat "$LOG_SYNC"
    exit 1
fi

if ! grep -q "mirror: active mirror array initialized with 2 device(s)" "$LOG_SYNC"; then
    echo "FAIL: Mirror array failed to initialize with 2 devices"
    cat "$LOG_SYNC"
    exit 1
fi

if ! grep -q "flush: PASS" "$LOG_SYNC"; then
    echo "FAIL: Volume transaction flush failed"
    cat "$LOG_SYNC"
    exit 1
fi

if ! grep -q "RESULT: PASS" "$LOG_SYNC"; then
    echo "FAIL: Chain-loaded application did not achieve RESULT: PASS"
    cat "$LOG_SYNC"
    exit 1
fi
echo "Scenario A: PASS (Both members discovered, enrolled, and committed across)"

echo "--- 3. Scenario C (Part 1): Post-Boot Integrity Check on Synchronized Array ---"
CHK0_OUT=$(check_disk esp0.img)
CHK1_OUT=$(check_disk esp1.img)

echo "$CHK0_OUT" | grep "committed:"
echo "$CHK1_OUT" | grep "committed:"

COMM0=$(echo "$CHK0_OUT" | grep "committed:")
COMM1=$(echo "$CHK1_OUT" | grep "committed:")

if [ "$COMM0" != "$COMM1" ]; then
    echo "FAIL: Committed root/generation mismatch between esp0.img and esp1.img!"
    echo "esp0: $COMM0"
    echo "esp1: $COMM1"
    exit 1
fi

if ! echo "$CHK0_OUT" | grep -q "check: OK (0 errors, 0 warnings)"; then
    echo "FAIL: fsck.runway found errors on esp0.img"
    echo "$CHK0_OUT"
    exit 1
fi

if ! echo "$CHK1_OUT" | grep -q "check: OK (0 errors, 0 warnings)"; then
    echo "FAIL: fsck.runway found errors on esp1.img"
    echo "$CHK1_OUT"
    exit 1
fi
echo "Scenario C (Part 1): PASS (Identical committed roots, 0 errors on both disks)"

echo "--- 3b. Member UUIDs: the two copies of one store are told apart ---"
store_get esp0.img 0 "$BUILD/mirror_chk.store"; MU0=$(member_uuid "$BUILD/mirror_chk.store")
store_get esp1.img 0 "$BUILD/mirror_chk.store"; MU1=$(member_uuid "$BUILD/mirror_chk.store")
if [ -z "$MU0" ] || [ -z "$MU1" ] || [ "$MU0" = "$MU1" ]; then
    echo "FAIL: members do not have their own UUID (esp0: '$MU0', esp1: '$MU1')"
    exit 1
fi
echo "Member UUIDs: PASS"

echo "--- 4. Scenario B: Lagging Replica Catch-up Synchronization ---"
# Prepare store at Gen 1 and store ahead at Gen 2 with identical UUID
cp -f "$BUILD/RUNWAY.DAT" "$BUILD/store_lagging.store"
cp -f "$BUILD/RUNWAY.DAT" "$BUILD/store_ahead.store"
"$BUILD/host_mutate" "$BUILD/store_ahead.store" /HOST_MUTATION.TXT "Catch-up test data" >/dev/null

# Verify ahead store has Gen 2
AHEAD_GEN=$("$BUILD/fsck.runway" "$BUILD/store_ahead.store" | grep "committed:" | grep "gen 2")
if [ -z "$AHEAD_GEN" ]; then
    echo "FAIL: store_ahead.store was not updated to Gen 2"
    "$BUILD/fsck.runway" "$BUILD/store_ahead.store"
    exit 1
fi

# Disk 0 (booted drive) starts at Gen 1; Disk 1 starts at Gen 2
tests/qemu/mkesp_mirror.sh esp0.img esp1.img "$BUILD/store_lagging.store" "$BUILD/store_ahead.store" >/dev/null

qemu_vars_copy

"$QEMU_BIN" \
    -nographic \
    $(qemu_args) \
    -drive format=raw,file=esp0.img \
    -drive format=raw,file=esp1.img \
    -serial stdio -display none > "$LOG_CATCHUP" 2>&1 || true

if ! grep -q "mirror: authoritative master is member 1 (generation 2" "$LOG_CATCHUP"; then
    echo "FAIL: Member 1 (ahead) was not selected as authoritative master"
    cat "$LOG_CATCHUP"
    exit 1
fi

if ! grep -q "mirror: member 0 is lagging (generation 1 < 2); synchronizing..." "$LOG_CATCHUP"; then
    echo "FAIL: Member 0 not detected as lagging"
    cat "$LOG_CATCHUP"
    exit 1
fi

if ! grep -q "mirror: replica caught up" "$LOG_CATCHUP"; then
    echo "FAIL: Catch-up synchronization did not report replica caught up"
    cat "$LOG_CATCHUP"
    exit 1
fi

if ! grep -q "mirror: catch-up synchronization complete" "$LOG_CATCHUP"; then
    echo "FAIL: Catch-up synchronization did not complete"
    cat "$LOG_CATCHUP"
    exit 1
fi

if ! grep -q "mirror: active mirror array initialized with 2 device(s) (primary: member 1)" "$LOG_CATCHUP"; then
    echo "FAIL: Mirror array not initialized with member 1 as primary"
    cat "$LOG_CATCHUP"
    exit 1
fi

if ! grep -q "HOST_MUTATION.TXT" "$LOG_CATCHUP"; then
    echo "FAIL: Catch-up replica did not expose synchronized HOST_MUTATION.TXT"
    cat "$LOG_CATCHUP"
    exit 1
fi

if ! grep -q "flush: PASS" "$LOG_CATCHUP"; then
    echo "FAIL: Volume flush failed on synchronized array"
    cat "$LOG_CATCHUP"
    exit 1
fi

if ! grep -q "RESULT: PASS" "$LOG_CATCHUP"; then
    echo "FAIL: Chain-loaded application did not achieve RESULT: PASS after sync"
    cat "$LOG_CATCHUP"
    exit 1
fi
echo "Scenario B: PASS (Disk 0 detected as lagging, synchronized to master, mounted cleanly)"

echo "--- 5. Scenario C (Part 2): Post-Boot Integrity Check after Catch-up ---"
CHK0_CATCHUP=$(check_disk esp0.img)
CHK1_CATCHUP=$(check_disk esp1.img)

echo "$CHK0_CATCHUP" | grep "committed:"
echo "$CHK1_CATCHUP" | grep "committed:"

COMM0_CATCHUP=$(echo "$CHK0_CATCHUP" | grep "committed:")
COMM1_CATCHUP=$(echo "$CHK1_CATCHUP" | grep "committed:")

if [ "$COMM0_CATCHUP" != "$COMM1_CATCHUP" ]; then
    echo "FAIL: Committed root/generation mismatch post catch-up!"
    echo "esp0: $COMM0_CATCHUP"
    echo "esp1: $COMM1_CATCHUP"
    exit 1
fi

if ! echo "$CHK0_CATCHUP" | grep -q "check: OK (0 errors, 0 warnings)"; then
    echo "FAIL: fsck.runway found errors on esp0.img post catch-up"
    echo "$CHK0_CATCHUP"
    exit 1
fi

if ! echo "$CHK1_CATCHUP" | grep -q "check: OK (0 errors, 0 warnings)"; then
    echo "FAIL: fsck.runway found errors on esp1.img post catch-up"
    echo "$CHK1_CATCHUP"
    exit 1
fi
echo "Scenario C (Part 2): PASS (Identical committed roots, 0 errors post catch-up)"

echo "--- 6. Native boot on a mirror: stale copy, missing copy ---"
fail() { echo "FAIL: $1"; [ -n "$2" ] && tail -n 60 "$2"; exit 1; }
. tests/qemu/native_lib.sh
NST0="$BUILD/nat_m0.store"; NST1="$BUILD/nat_m1.store"; LOG_N="$BUILD/mirror_native.log"
nboot() { # drives... : boot the given images, first one first
    qemu_vars_copy
    set --  $(for d in "$@"; do printf -- '-drive format=raw,file=%s ' "$d"; done)
    timeout 150 "$QEMU_BIN" -nographic $(qemu_args) \
        $* -serial stdio -display none > "$LOG_N" 2>&1 || true
}
pull() { store_get esp0.img 0 "$NST0"; [ -f esp1.img ] && store_get esp1.img 0 "$NST1"; return 0; }
push() { store_put "$1" 0 "$2"; }
same_root() {
    pull
    a=$("$BUILD/fsck.runway" "$NST0" | grep "committed:"); b=$("$BUILD/fsck.runway" "$NST1" | grep "committed:")
    [ "$a" = "$b" ] || fail "mirror members diverged: [$a] [$b]"
    "$BUILD/fsck.runway" "$NST0" | grep -q "check: OK (0 errors, 0 warnings)" || fail "esp0 store not clean"
    "$BUILD/fsck.runway" "$NST1" | grep -q "check: OK (0 errors, 0 warnings)" || fail "esp1 store not clean"
}
both() { [ "$(nat_cat "$NST0" $NAT_T/$1)" = "$2" ] && [ "$(nat_cat "$NST1" $NAT_T/$1)" = "$2" ] || fail "$1 is not '$2' on both members"; }

NSEED="$BUILD/nat_m.store"
$BUILD/mkstore "$NSEED" >/dev/null
nat_seed "$NSEED"
nat_slot "$NSEED" 1 confirm OK
nat_slot "$NSEED" 2 confirm INSTALLED
tests/qemu/mkesp_mirror.sh esp0.img esp1.img "$NSEED" "$NSEED" >/dev/null

# D: both copies in step: the selection commit (TRIES, STATE, BOOTED, REASON) lands on both
nboot esp0.img esp1.img
nat_expect "$LOG_N" "RUNWAY: crypto: AES on CPU instructions"
nat_expect "$LOG_N" "mirror: active mirror array initialized with 2 device(s)"
nat_expect "$LOG_N" "RUNWAY: booting CONFIG2 (rev 2, TESTING, 0 tries left)"
nat_expect "$LOG_N" "[NATIVE] confirm via BOOTED: 0x0"
same_root
both CONFIG2/STATE OK; both CONFIG2/TRIES 0; both BOOTED/STATE OK; both REASON normal
echo "Scenario D: PASS (selection and confirmation reach both members)"

# The debug loader keeps its log on the ESP, not in the store
mcopy -i "esp0.img@@$(img_part_off esp0.img 1)" ::RUNWAY.LOG "$BUILD/esp_runway.log" || fail "no RUNWAY.LOG on the ESP"
rm -f "$BUILD/esp_runway.log"
nat_cat "$NST0" RUNWAY.LOG >/dev/null 2>&1 && fail "RUNWAY.LOG is in the store"
echo "Scenario D2: PASS (RUNWAY.LOG is on the ESP and not in the store)"

# E: member 1 is ahead and holds a FAILED verdict on CONFIG2: that copy wins, the lagging one is caught up
pull
nat_fs "$NST1" put $NAT_T/CONFIG2/STATE "FAILED
"
push esp1.img "$NST1"
nboot esp0.img esp1.img
nat_expect "$LOG_N" "member 0 is lagging"
nat_expect "$LOG_N" "RUNWAY: CONFIG2 FAILED, skipped"
nat_expect "$LOG_N" "RUNWAY: booting CONFIG1 (rev 1, OK)"
nat_refuse "$LOG_N" "RUNWAY: booting CONFIG2"
same_root
both CONFIG2/STATE FAILED; both BOOTED/STATE OK; both REASON normal
echo "Scenario E: PASS (the newest copy's slot state decides, both members agree afterwards)"

# F: a copy is missing: boot from one disk, the other is stale, then caught up with the new state
pull
nat_slot "$NST0" 3 confirm INSTALLED
push esp0.img "$NST0"
nboot esp0.img
nat_refuse "$LOG_N" "mirror: active mirror array initialized with 2"
nat_expect "$LOG_N" "RUNWAY: booting CONFIG3 (rev 3, TESTING, 0 tries left)"
nat_expect "$LOG_N" "[NATIVE] confirm via BOOTED: 0x0"
pull
[ "$(nat_cat "$NST0" $NAT_T/CONFIG3/STATE)" = OK ] || fail "CONFIG3 not confirmed on the lone member"
nat_cat "$NST1" $NAT_T/CONFIG3/STATE >/dev/null 2>&1 && fail "the missing copy cannot know CONFIG3"
nboot esp0.img esp1.img
nat_expect "$LOG_N" "member 1 is lagging"
nat_expect "$LOG_N" "RUNWAY: booting CONFIG3 (rev 3, OK)"
same_root
both CONFIG3/STATE OK; both BOOTED/STATE OK
echo "Scenario F: PASS (one copy missing, then caught up)"
# G: same generation, but the second member holds a newer state record (CONFIG3 FAILED): it wins
pull
$BUILD/rwyctl --store "$NST1" fail CONFIG3 >/dev/null 2>&1 || fail "rwyctl fail on member 1"
nat_fs "$NST0" mkdir EFI/SPARE
push esp0.img "$NST0"; push esp1.img "$NST1"
nboot esp0.img esp1.img
nat_refuse "$LOG_N" "is lagging"
nat_expect "$LOG_N" "RUNWAY: CONFIG3 FAILED, skipped"
nat_refuse "$LOG_N" "RUNWAY: booting CONFIG3"
nat_expect "$LOG_N" "RUNWAY: booting CONFIG1 (rev 1, OK)"
pull
both CONFIG3/STATE FAILED
echo "Scenario G: PASS (the newest state record wins at the same generation, both members agree afterwards)"
# H: member 0 is behind in the tree but ahead in the record (CONFIG4 FAILED): the catch-up keeps that record
pull
nat_slot "$NST0" 4 confirm OK; nat_slot "$NST1" 4 confirm OK
$BUILD/rwyctl --store "$NST0" fail CONFIG4 >/dev/null 2>&1 || fail "rwyctl fail on member 0"
nat_fs "$NST1" mkdir EFI/SPARE2; nat_fs "$NST1" mkdir EFI/SPARE3
push esp0.img "$NST0"; push esp1.img "$NST1"
nboot esp0.img esp1.img
nat_expect "$LOG_N" "member 0 is lagging"
nat_expect "$LOG_N" "RUNWAY: CONFIG4 FAILED, skipped"
nat_refuse "$LOG_N" "RUNWAY: booting CONFIG4"
pull
both CONFIG4/STATE FAILED
echo "Scenario H: PASS (a record newer than the tree survives the catch-up)"
# I: WDT/ exists only on the member that is ahead: the lagging member does not change the arming
pull
nat_fs "$NST1" put $NAT_T/BLOBS/ok.efi @$BUILD/WDT_STUB_OK.EFI
HOST_FS_SYSTEM=1 nat_fs "$NST1" mkdir $NAT_T/WDT
HOST_FS_SYSTEM=1 nat_fs "$NST1" link $NAT_T/BLOBS/ok.efi $NAT_T/WDT/A1.EFI
nat_fs "$NST1" put $NAT_T/WATCHDOG "30
"
push esp1.img "$NST1"
nboot esp0.img esp1.img
nat_expect "$LOG_N" "member 0 is lagging"
nat_expect "$LOG_N" "RUNWAY: WATCHDOG 30s armed by A1.EFI"
nat_expect "$LOG_N" "RUNWAY: booting CONFIG1 (rev 1, OK)"
pull
both WDT_HINT A1.EFI
echo "Scenario I: PASS (WDT/ is read from the newest copy, the hint reaches both members)"

# J: RunwayBootMembers names the partitions of the members in use, the booted one first
U0=5A0B0C0D-0000-4000-8000-00000000000A; U1=5A0B0C0D-0000-4000-8000-00000000000B
tests/qemu/mkesp_mirror.sh "$BUILD/mirror_gpt0.img" "$BUILD/mirror_gpt1.img" "$NSEED" "$NSEED" $U0 $U1 >/dev/null
nboot "$BUILD/mirror_gpt0.img" "$BUILD/mirror_gpt1.img"
nat_expect "$LOG_N" "[NATIVE] RunwayBootMembers: $U0 $U1 (attr 0x6)"
nboot "$BUILD/mirror_gpt1.img" "$BUILD/mirror_gpt0.img"
nat_expect "$LOG_N" "[NATIVE] RunwayBootMembers: $U1 $U0 (attr 0x6)"
rm -f "$BUILD/mirror_gpt0.img" "$BUILD/mirror_gpt1.img"
echo "Scenario J: PASS (RunwayBootMembers lists the members in use, the booted one first)"

# K: member 1 lags and every write to it fails: it is left out, the group boots from member 0
pull
nat_slot "$NST0" 5 confirm INSTALLED
push esp0.img "$NST0"
printf '[inject-error]\nevent = "write_aio"\nerrno = "5"\n' > "$BUILD/mirror.blkdebug"
nboot esp0.img "blkdebug:$BUILD/mirror.blkdebug:esp1.img"
nat_expect "$LOG_N" "member 1 is lagging"
nat_expect "$LOG_N" "RUNWAY: mirror: member 1 cannot be caught up"
nat_refuse "$LOG_N" "FAIL: catch-up synchronization failed"
nat_expect "$LOG_N" "mirror: active mirror array initialized with 1 device(s)"
nat_expect "$LOG_N" "RUNWAY: booting CONFIG5 (rev 5, TESTING, 0 tries left)"
nat_expect "$LOG_N" "[NATIVE] confirm via BOOTED: 0x0"
pull
[ "$(nat_cat "$NST0" $NAT_T/CONFIG5/STATE)" = OK ] || fail "CONFIG5 not confirmed on member 0"
nat_cat "$NST1" $NAT_T/CONFIG5/STATE >/dev/null 2>&1 && fail "member 1 cannot know CONFIG5"
rm -f "$BUILD/mirror.blkdebug"
echo "Scenario K: PASS (a member that cannot be caught up is left out, the group boots)"
rm -f "$NST0" "$NST1" "$NSEED"

# Clean up temporary test files and restore single esp.img
rm -f "$BUILD/store_lagging.store" "$BUILD/store_ahead.store" "$BUILD/mirror_chk.store"
sh tools/mkesp.sh esp.img "$BUILD/RUNWAY.DAT" >/dev/null

echo "================================================================="
echo "TEST-MIRROR: PASS (All scenarios verified: A, B, C, D, E, F, G, H, I, J and K)"
echo "================================================================="
