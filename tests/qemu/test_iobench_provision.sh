#!/bin/sh
# tests/qemu/test_iobench_provision.sh — provision a target disk from the Linux slot,
# reboot into the benchmark slot, check the handoff and the fallback to the provisioning slot.
# SPDX-License-Identifier: Apache-2.0
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"
# TOPOLOGY=usb-nvme boots from a USB disk and prepares an NVMe disk; the default is two SATA disks.
TOPOLOGY=${TOPOLOGY:-sata}
# TPM=1 attaches a software TPM 2.0, as on a PC: the loader must leave the benchmark stores alone.
TPM=${TPM:-0}
# STALE=1 starts from a disk with a valid marker whose encrypted store has another key: it must be provisioned again.
STALE=${STALE:-0}
SUFFIX=; [ "$TOPOLOGY" = sata ] || SUFFIX=-$TOPOLOGY
[ "$TPM" != 1 ] || SUFFIX=$SUFFIX-tpm
[ "$STALE" != 1 ] || SUFFIX=$SUFFIX-stale
LOG="$BUILD/iobench-provision$SUFFIX.log"
BOOT="$BUILD/iobench-provision$SUFFIX-boot.img"
TARGET="$BUILD/iobench-provision$SUFFIX-target.img"
DEK="$BUILD/iobench-provision.dek"
KERNEL="$BUILD/provision/vmlinuz"
INITRD="$BUILD/provision/initramfs.cpio.gz"
RWYCTL="$BUILD/rwyctl"
TMP=$(mktemp -d /var/tmp/rwy-provision.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

case "$TOPOLOGY" in
sata)
    TARGET_DEV=/dev/sdb
    DRIVES="-drive format=raw,file=$BOOT -drive format=raw,file=$TARGET"
    ;;
usb-nvme)
    TARGET_DEV=/dev/nvme0n1
    DRIVES="-device qemu-xhci -drive if=none,id=boot,format=raw,file=$BOOT -device usb-storage,drive=boot,bootindex=0 -device usb-kbd -drive if=none,id=target,format=raw,file=$TARGET -device nvme,drive=target,serial=iobench0"
    ;;
*) echo "FAILED: unknown TOPOLOGY $TOPOLOGY" >&2; exit 1 ;;
esac

# swtpm ends when QEMU shuts down, so start it again, on the same state, before each boot.
start_tpm() {
    [ "$TPM" = 1 ] || return 0
    [ -z "${SWTPM_PID:-}" ] || kill -9 "$SWTPM_PID" 2>/dev/null || true
    rm -f "$TMP/tpm/sock"
    swtpm socket --tpmstate dir="$TMP/tpm" --ctrl type=unixio,path="$TMP/tpm/sock" --tpm2 --flags not-need-init &
    SWTPM_PID=$!
    n=0
    until [ -S "$TMP/tpm/sock" ]; do
        n=$((n + 1))
        [ "$n" -lt 50 ] || { echo "FAILED: swtpm did not start" >&2; exit 1; }
        sleep 0.1
    done
}

if [ "$TPM" = 1 ]; then
    command -v swtpm >/dev/null 2>&1 || { echo "test-iobench-provision: SKIP (no swtpm)"; exit 0; }
    mkdir -p "$TMP/tpm"
    trap 'kill -9 "${SWTPM_PID:-0}" 2>/dev/null || true; rm -rf "$TMP"' EXIT
    DRIVES="$DRIVES -chardev socket,id=chrtpm,path=$TMP/tpm/sock -tpmdev emulator,id=tpm0,chardev=chrtpm -device $QEMU_TPM_DEVICE,tpmdev=tpm0"
fi

[ "$ARCH" = x86_64 ] || { echo "test-iobench-provision: SKIP (x86_64 only)"; exit 0; }
[ -x "$RWYCTL" ] || RWYCTL="$BUILD/tools/rwyctl"
[ -x "$RWYCTL" ] || { echo "FAILED: missing rwyctl" >&2; exit 1; }

rc=0
sh tests/qemu/mkinitramfs.sh "$INITRD" "$KERNEL" || rc=$?
if [ "$rc" = 77 ]; then echo "test-iobench-provision: SKIP (no Debian rootfs)"; exit 0; fi
[ "$rc" = 0 ] || { echo "FAILED: cannot build the initramfs" >&2; exit 1; }
size=$(stat -c %s "$INITRD")
[ "$size" -lt $((128 * 1024 * 1024)) ] ||
    { echo "FAILED: initramfs is $size bytes, larger than the 128 MiB bootstore" >&2; exit 1; }

dd if=/dev/urandom of="$DEK" bs=32 count=1 2>/dev/null
rm -f "$TARGET"
if [ "$STALE" = 1 ]; then
    dd if=/dev/urandom of="$TMP/other.dek" bs=32 count=1 2>/dev/null
    sh tests/qemu/mkiobench.sh --target "$TARGET" "$TMP/other.dek" >/dev/null
else
    truncate -s 512M "$TARGET"
fi

IOBENCH_BOOT_ARGS="console=ttyS0,115200 console=tty0 rdinit=/init iobench.unattended=1 iobench.targets=$TARGET_DEV iobench.confirm=ERASE iobench.label=provisioned iobench.rounds=1 iobench.warmup=0 iobench.devices=block,file,rwy-blk,rwy-file,rwy-enc" \
    sh tests/qemu/mkiobench.sh --boot "$BOOT" "$BUILD/IOBENCH.EFI" "$BUILD/RUNWAY.EFI" \
    "$KERNEL" "$INITRD" "$DEK"

start_tpm
qemu_vars_copy
# shellcheck disable=SC2046
timeout 900 "$QEMU_BIN" -nographic $(qemu_args) \
    $DRIVES \
    -serial stdio -display none > "$LOG" 2>&1 || true

grep -E '^(PROVISION|iobench|IOBENCH)' "$LOG" || true
if [ "$TOPOLOGY" = usb-nvme ]; then
    grep -q 'input: .*Keyboard' "$LOG" ||
        { echo "FAILED: the USB keyboard was not found" >&2; exit 1; }
fi
grep -q "^RUNWAY: BOOT-DELAY 5s" "$LOG" ||
    { echo "FAILED: the provisioning slot did not wait 5 seconds: see $LOG" >&2; exit 1; }
grep -q "^PROVISION: selected: $TARGET_DEV" "$LOG" ||
    { echo "FAILED: the target disk was not selected" >&2; exit 1; }
if [ "$STALE" = 1 ]; then
    grep -q 'encrypted store does not open with BENCH.DEK' "$LOG" ||
        { echo "FAILED: the stale encrypted store was not noticed" >&2; exit 1; }
fi
grep -q "^PROVISION: prepared $TARGET_DEV" "$LOG" ||
    { echo "FAILED: the target disk was not prepared" >&2; exit 1; }
grep -q '^PROVISION: rebooting into the benchmark slot' "$LOG" ||
    { echo "FAILED: no handoff reboot" >&2; exit 1; }
if [ "$TPM" = 1 ]; then
    grep -q 'reencrypt: copying' "$LOG" &&
        { echo "FAILED: the loader re-encrypted a benchmark store" >&2; exit 1; }
fi
grep -q '^IOBENCH DONE' "$LOG" ||
    { echo "FAILED: the benchmark slot did not finish" >&2; exit 1; }
grep -q '^IOBENCH FAIL' "$LOG" &&
    { echo "FAILED: the benchmark slot reported a failure" >&2; exit 1; }

[ "$(sfdisk -d "$TARGET" | grep -c 'name="IOBENCH"')" -eq 1 ] ||
    { echo "FAILED: the target has no IOBENCH partition" >&2; exit 1; }
[ "$(sfdisk -d "$TARGET" | grep -c 'name="IOBENCH-ENC"')" -eq 1 ] ||
    { echo "FAILED: the target has no IOBENCH-ENC partition" >&2; exit 1; }
[ "$(sfdisk -d "$TARGET" | grep -c 'name="BENCH-ESP"')" -eq 1 ] ||
    { echo "FAILED: the target has no BENCH-ESP partition" >&2; exit 1; }
mdir -i "$TARGET@@$(img_part_off "$TARGET" 3)" :: | grep -q RWYBENCH ||
    { echo "FAILED: the target ESP has no RWYBENCH.DAT" >&2; exit 1; }
id=$(sfdisk --disk-id "$TARGET" | tr 'a-f' 'A-F')
plain="$TARGET@$(img_part_off "$TARGET" 1)+$(img_part_bytes "$TARGET" 1)"
"$RWYCTL" --store "$plain" cat PERF/READY | grep -qx "disk_guid=$id" ||
    { echo "FAILED: the marker does not name the target disk" >&2; exit 1; }

esp_off=$(img_part_off "$BOOT" 1)
mcopy -o -i "$BOOT@@$esp_off" ::IOBENCH.CFG "$TMP/IOBENCH.CFG"
grep -qx 'devices=block,file,rwy-blk,rwy-file,rwy-enc' "$TMP/IOBENCH.CFG" ||
    { echo "FAILED: IOBENCH.CFG was not written" >&2; exit 1; }

bs_off=$(img_part_off "$BOOT" 2)
bs_len=$(img_part_bytes "$BOOT" 2)
status=$("$RWYCTL" --store "$BOOT@$bs_off+$bs_len" status)
echo "$status"
echo "$status" | grep -q 'CONFIG1' ||
    { echo "FAILED: CONFIG1 was not installed" >&2; exit 1; }
echo "$status" | grep -q 'runway-1.efi' ||
    { echo "FAILED: CONFIG1 is not a Type 2 .efi slot" >&2; exit 1; }
echo "$status" | grep -q 'CONFIG0' ||
    { echo "FAILED: CONFIG0 is missing" >&2; exit 1; }

res_off=$(img_part_off "$BOOT" 3)
res_len=$(img_part_bytes "$BOOT" 3)
"$RWYCTL" --store "$BOOT@$res_off+$res_len" cat PERF/RUNS.CSV | grep -q ',ok,' ||
    { echo "FAILED: no successful run row" >&2; exit 1; }
"$RWYCTL" --store "$BOOT@$res_off+$res_len" cat PERF/SAMPLES.CSV | grep -q ',block,' ||
    { echo "FAILED: no block samples" >&2; exit 1; }
for d in file rwy-file; do
    "$RWYCTL" --store "$BOOT@$res_off+$res_len" cat PERF/SAMPLES.CSV | grep -q ",$d," ||
        { echo "FAILED: no $d samples from the ESP of the target" >&2; exit 1; }
done
if "$RWYCTL" --store "$BOOT@$res_off+$res_len" cat PERF/RUNNING >/dev/null 2>&1; then
    echo "FAILED: RUNNING marker was not removed" >&2
    exit 1
fi

# The reference run under Linux put its rows in the results store, before the rows of the benchmark.
samples=$("$RWYCTL" --store "$BOOT@$res_off+$res_len" cat PERF/SAMPLES.CSV)
[ "$(echo "$samples" | grep -c ',linux-block,')" -eq 10 ] ||
    { echo "FAILED: expected 10 linux-block samples (4 sizes, write and read, and 2 sizes of write-sync)" >&2; exit 1; }
[ "$(echo "$samples" | grep -c ',linux-cipher,')" -eq 5 ] ||
    { echo "FAILED: expected 5 linux-cipher samples (encrypt, decrypt and 3 clocks)" >&2; exit 1; }
echo "$samples" | grep ',linux-' | grep -q ',failed$' && { echo "FAILED: a Linux sample failed" >&2; exit 1; }
"$RWYCTL" --store "$BOOT@$res_off+$res_len" cat PERF/RUNS.CSV | grep -q ',raw,target,' ||
    { echo "FAILED: no run row of the Linux benchmark" >&2; exit 1; }
echo "$samples" | head -n 1 | grep -q '^run,disk,dev' && [ "$(echo "$samples" | grep -c '^run,disk,dev')" -eq 1 ] ||
    { echo "FAILED: SAMPLES.CSV must have its header once, first" >&2; exit 1; }

# The facts about the machine, on the ESP and in the results store: the system, the boot disk and the target.
"$RWYCTL" --store "$BOOT@$res_off+$res_len" cat PERF/HW.TXT >"$TMP/HW.store" 2>/dev/null ||
    { echo "FAILED: no HW.TXT in the results store" >&2; exit 1; }
mcopy -o -i "$BOOT@@$esp_off" ::PERF/HW.TXT "$TMP/HW.esp" ||
    { echo "FAILED: no HW.TXT on the ESP" >&2; exit 1; }
cmp -s "$TMP/HW.store" "$TMP/HW.esp" || { echo "FAILED: the two copies of HW.TXT differ" >&2; exit 1; }
grep -q '^\[system\]$' "$TMP/HW.esp" && grep -q '^cpu_model=.' "$TMP/HW.esp" ||
    { echo "FAILED: HW.TXT has no system section" >&2; exit 1; }
grep -q "^\[disk $TARGET_DEV target\]$" "$TMP/HW.esp" || { echo "FAILED: HW.TXT has no target disk" >&2; exit 1; }
grep -q '^\[disk .* boot\]$' "$TMP/HW.esp" || { echo "FAILED: HW.TXT has no boot disk" >&2; exit 1; }

# The benchmark slot ran its one try and did not confirm: the next boot must fall back to the provisioning slot.
FALLBACK_LOG="$BUILD/iobench-provision$SUFFIX-fallback.log"
start_tpm
qemu_vars_copy
# shellcheck disable=SC2046
timeout 60 "$QEMU_BIN" -nographic $(qemu_args) \
    $DRIVES \
    -serial stdio -display none > "$FALLBACK_LOG" 2>&1 || true
grep -q "^PROVISION: $TARGET_DEV is prepared; reusing" "$FALLBACK_LOG" ||
    { echo "FAILED: the provisioning slot did not run after the benchmark" >&2; exit 1; }
grep -q '^PROVISION: the benchmark slot already ran' "$FALLBACK_LOG" ||
    { echo "FAILED: the unattended slot armed the benchmark again" >&2; exit 1; }
[ "$(grep -c '^PROVISION: rebooting' "$FALLBACK_LOG")" -eq 0 ] ||
    { echo "FAILED: the provisioning slot rebooted in a loop" >&2; exit 1; }
grep -q '^IOBENCH' "$FALLBACK_LOG" &&
    { echo "FAILED: the benchmark slot ran again" >&2; exit 1; }

echo ">>> SUCCESS: provisioning prepared the target, handed off to the benchmark and fell back"
