#!/bin/sh
# tests/qemu/provision.sh — prepare selected disks and hand off to the benchmark slot.
# SPDX-License-Identifier: Apache-2.0
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export TERMINFO=/usr/share/terminfo

STORE_MB=96
RAW_MB=16
# The benchmark partitions are not store partitions: RUNWAY.EFI would mount them and re-encrypt the encrypted one.
DATA_TYPE=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
ESP_TYPE=C12A7328-F81F-11D2-BA4B-00A0C93EC93B
ESP_MB=$((STORE_MB + 64))
PART_GUID=5D0A7D6E-3B1C-4C1E-9A42-6F1B8E7021C5
ENC_GUID=5D0A7D6E-3B1C-4C1E-9A42-6F1B8E7021C6
RESULT_GUID=5D0A7D6E-3B1C-4C1E-9A42-6F1B8E7021C7
ESP_MNT=/mnt/esp

UNATTENDED=0
TARGETS=
CONFIRM=
REPROVISION=0
TARGET_ESP=1
LABEL=provisioned
ROUNDS=
WARMUP=1
DEVICES=
PAYLOAD_MIB=16
DISK_MODE=internal
INCLUDE_BOOT=0
RESULT=auto
PAUSE=
LINUX=1
REBOOT_WAIT=
PROVISIONED=
BOOT_DISK=

log() { echo "PROVISION: $*"; }
die() { echo "PROVISION: $*" >&2; exit 1; }

for arg in $(cat /proc/cmdline); do
    case "$arg" in
    iobench.unattended=*) UNATTENDED=${arg#*=} ;;
    iobench.targets=*) TARGETS=${arg#*=} ;;
    iobench.confirm=*) CONFIRM=${arg#*=} ;;
    iobench.reprovision=*) REPROVISION=${arg#*=} ;;
    iobench.esp=*) TARGET_ESP=${arg#*=} ;;
    iobench.label=*) LABEL=${arg#*=} ;;
    iobench.rounds=*) ROUNDS=${arg#*=} ;;
    iobench.warmup=*) WARMUP=${arg#*=} ;;
    iobench.devices=*) DEVICES=${arg#*=} ;;
    iobench.payload_mib=*) PAYLOAD_MIB=${arg#*=} ;;
    iobench.disk_mode=*) DISK_MODE=${arg#*=} ;;
    iobench.include_boot=*) INCLUDE_BOOT=${arg#*=} ;;
    iobench.result=*) RESULT=${arg#*=} ;;
    iobench.pause=*) PAUSE=${arg#*=} ;;
    iobench.linux=*) LINUX=${arg#*=} ;;
    iobench.reboot_wait=*) REBOOT_WAIT=${arg#*=} ;;
    iobench.boot_disk=*) BOOT_DISK=${arg#*=} ;;
    esac
done

# Disks that are members of an md array. The array is offered, not its members.
md_members() {
    for m in /sys/block/md*/slaves/*; do
        [ -e "$m" ] && echo "${m##*/}"
    done
}

all_disks() {
    held=" $(md_members | tr '\n' ' ') "
    for s in /sys/block/*; do
        n=${s##*/}
        case "$n" in
        loop*|ram*|dm-*|sr*) continue ;;
        esac
        case "$held" in *" $n "*) continue ;; esac
        [ "$(cat "$s/size" 2>/dev/null || echo 0)" != 0 ] || continue
        [ -b "/dev/$n" ] && echo "/dev/$n"
    done | sort
}

part_dev() {
    case "${1##*/}" in
    nvme*|mmcblk*|loop*|md*) echo "${1}p$2" ;;
    *) echo "${1}$2" ;;
    esac
}

part_name() {
    sfdisk -d "$1" 2>/dev/null |
        sed -n "s/^ *\([^ ]*\) *:.*name=\"$2\".*/\1/p" | head -n 1
}

has_guid() {
    sfdisk -d "$1" 2>/dev/null | tr 'A-F' 'a-f' | grep -q "$(echo "$2" | tr 'A-F' 'a-f')"
}

find_boot_disk() {
    for d in $(all_disks); do
        if has_guid "$d" "$RESULT_GUID"; then
            echo "$d"
            return
        fi
    done
}

disk_id() {
    sfdisk --disk-id "$1" 2>/dev/null | tr 'a-f' 'A-F'
}

marker_ok() {
    p=$(part_name "$1" IOBENCH)
    [ -n "$p" ] || return 1
    id=$(disk_id "$1")
    [ -n "$id" ] || return 1
    text=$(rwyctl --store "$p" cat PERF/READY 2>/dev/null) || return 1
    echo "$text" | grep -qx 'layout=1' || return 1
    echo "$text" | grep -qx "disk_guid=$id" || return 1
}

# The encrypted store must open with the key on the ESP. Another key, such as one the loader sealed in a TPM,
# leaves a disk that has a valid marker but cannot be measured.
enc_ok() {
    [ -n "$DEK" ] || return 0
    e=$(part_name "$1" IOBENCH-ENC)
    [ -n "$e" ] || return 1
    rwyctl --dek-file "$DEK" --store "$e" status >/dev/null 2>&1
}

# A disk prepared before the target ESP existed has none: prepare it again.
esp_ok() {
    [ "$TARGET_ESP" != 1 ] || [ -n "$(part_name "$1" BENCH-ESP)" ]
}

disk_info() {
    n=${1##*/}
    model=$(cat "/sys/block/$n/device/model" 2>/dev/null | tr -d '\n' | sed 's/ *$//' || true)
    serial=$(cat "/sys/block/$n/device/serial" 2>/dev/null | tr -d '\n' | sed 's/ *$//' || true)
    size=$(cat "/sys/block/$n/size" 2>/dev/null || echo 0)
    [ -n "$model" ] || [ ! -r "/sys/block/$n/md/level" ] || model="RAID $(cat "/sys/block/$n/md/level")"
    [ -n "$model" ] || model=unknown
    [ -n "$serial" ] || serial=-
    printf '%s\t%s\t%s\t%s MiB' "$1" "$model" "$serial" "$((size / 2048))"
}

is_removable() {
    [ "$(cat "/sys/block/${1##*/}/removable" 2>/dev/null || echo 0)" = 1 ]
}

candidates() {
    show_usb=$1
    for d in $(all_disks); do
        [ "$d" = "$BOOT_DISK" ] && continue
        if is_removable "$d" && [ "$show_usb" != 1 ]; then continue; fi
        disk_info "$d"
        echo
    done
}

line_select() {
    echo "WARNING: the selected disks will be erased." >&2
    echo "Select the disks to prepare (space-separated numbers):" >&2
    i=0
    for d in $(all_disks); do
        [ "$d" = "$BOOT_DISK" ] && continue
        i=$((i + 1))
        printf '  %d) %s\n' "$i" "$(disk_info "$d")" >&2
    done
    printf 'Selection: ' >&2
    read -r answer
    selected=
    for n in $answer; do
        i=0
        for d in $(all_disks); do
            [ "$d" = "$BOOT_DISK" ] && continue
            i=$((i + 1))
            if [ "$i" = "$n" ]; then selected="$selected $d"; fi
        done
    done
    printf 'Type yes to erase the selected disks: ' >&2
    read -r answer
    [ "$answer" = yes ] || die "not confirmed"
    echo "$selected"
}

whiptail_select() {
    set -- --title "Runway benchmark provisioning" --checklist \
        "The selected disks will be erased." 20 78 12
    for d in $(all_disks); do
        [ "$d" = "$BOOT_DISK" ] && continue
        is_removable "$d" && continue
        set -- "$@" "$d" "$(disk_info "$d")" on
    done
    [ $# -gt 5 ] || die "no internal disks found"
    selected=$(whiptail "$@" 3>&1 1>&2 2>&3) || die "selection cancelled"
    selected=$(echo "$selected" | tr -d '"')
    [ -n "$selected" ] || die "no disks selected"
    whiptail --title "Confirm erase" --yesno \
        "These disks will be erased:\n\n$selected\n\nContinue?" 16 78 >&2 ||
        die "not confirmed"
    echo "$selected"
}

# A USB disk shows up a few seconds after its driver loads.
wait_boot_disk() {
    i=0
    while [ "$i" -lt 30 ]; do
        d=$(find_boot_disk)
        if [ -n "$d" ]; then echo "$d"; return; fi
        [ "$i" -gt 0 ] || log "looking for the boot disk" >&2
        sleep 1
        i=$((i + 1))
    done
}

if [ -z "$BOOT_DISK" ]; then
    BOOT_DISK=$(wait_boot_disk)
fi
[ -n "$BOOT_DISK" ] || die "cannot find the boot disk (no results partition); disks seen: $(all_disks | tr '\n' ' ')"
ESP=$(part_name "$BOOT_DISK" ESP)
[ -n "$ESP" ] || ESP=$(part_dev "$BOOT_DISK" 1)
BOOTSTORE=$(part_name "$BOOT_DISK" BOOTSTORE)
[ -n "$BOOTSTORE" ] || BOOTSTORE=$(part_dev "$BOOT_DISK" 2)
[ -b "$ESP" ] || die "cannot find the boot ESP"
[ -b "$BOOTSTORE" ] || die "cannot find the boot store"
log "boot disk $BOOT_DISK, ESP $ESP, boot store $BOOTSTORE"

mkdir -p "$ESP_MNT"
mount -t vfat "$ESP" "$ESP_MNT" || die "cannot mount the ESP"
trap 'sync; umount "$ESP_MNT" 2>/dev/null || true' EXIT
DEK=
[ -f "$ESP_MNT/BENCH.DEK" ] && DEK="$ESP_MNT/BENCH.DEK"
if [ -z "$DEVICES" ]; then
    DEVICES=block,rwy-blk
    [ "$TARGET_ESP" != 1 ] || DEVICES=block,file,rwy-blk,rwy-file
    [ -z "$DEK" ] || DEVICES="$DEVICES,rwy-enc"
fi
case ",$DEVICES," in
*,rwy-enc,*) [ -n "$DEK" ] || die "rwy-enc is selected but BENCH.DEK is missing" ;;
esac

if [ "$UNATTENDED" = 1 ]; then
    [ -n "$TARGETS" ] || die "unattended mode needs iobench.targets"
    [ "$CONFIRM" = ERASE ] || die "unattended mode needs iobench.confirm=ERASE"
    selected=$(echo "$TARGETS" | tr ',' ' ')
else
    [ -t 0 ] || die "no terminal; use unattended mode"
    if command -v whiptail >/dev/null 2>&1; then
        selected=$(whiptail_select)
    else
        selected=$(line_select)
    fi
fi

SELECTED=
for d in $selected; do
    [ -b "$d" ] || die "not a block device: $d"
    [ "$d" = "$BOOT_DISK" ] && die "refusing to erase the boot disk $d"
    case " $SELECTED " in *" $d "*) continue ;; esac
    SELECTED="$SELECTED $d"
done
[ -n "$SELECTED" ] || die "no disks selected"
log "selected:$SELECTED"
for d in $SELECTED; do log "will erase $(disk_info "$d")"; done

# The partitions of a target disk. The ESP is last, so that the numbers of the others do not change.
partition_disk() {
    enc_line= esp_line=
    [ -z "$DEK" ] || enc_line="size=${STORE_MB}MiB, type=$DATA_TYPE, uuid=$ENC_GUID, name=\"IOBENCH-ENC\""
    [ "$TARGET_ESP" != 1 ] || esp_line="size=${ESP_MB}MiB, type=$ESP_TYPE, name=\"BENCH-ESP\""
    sfdisk "$1" >/dev/null <<PARTS || die "cannot partition $1"
label: gpt
size=${2}MiB, type=$DATA_TYPE, uuid=$PART_GUID, name="IOBENCH"
$enc_line
$esp_line
PARTS
}

# A FAT volume with a RWYBENCH.DAT, for the file cells on this disk.
make_target_esp() {
    esp=$(part_dev "$1" "$2")
    [ -b "$esp" ] || die "partition $esp did not appear"
    log "writing the ESP and its store to $esp"
    mkfs.fat -F 32 -s 2 -n BENCHESP "$esp" >/dev/null || die "cannot format $esp"
    mkdir -p /mnt/tesp
    mount -t vfat "$esp" /mnt/tesp || die "cannot mount $esp"
    mkstore --size "$STORE_MB" --flight-records 0 --journal-records 0 /mnt/tesp/RWYBENCH.DAT >/dev/null ||
        die "cannot create the store on $esp"
    umount /mnt/tesp || die "cannot unmount $esp"
}

provision_disk() {
    dev=$1
    log "erasing $dev"
    wipefs -a "$dev" >/dev/null 2>&1 || true
    log "partitioning $dev"
    id=$(cat /proc/sys/kernel/random/uuid | tr 'a-f' 'A-F')
    parts=$((STORE_MB + RAW_MB))
    partition_disk "$dev" "$parts"
    sfdisk --disk-id "$dev" "$id" >/dev/null
    partprobe "$dev" 2>/dev/null || true
    sleep 1
    plain=$(part_dev "$dev" 1)
    [ -b "$plain" ] || die "partition $plain did not appear"
    log "writing the plain store to $plain"
    truncate -s "${STORE_MB}M" /run/plain.store
    mkstore --size "$STORE_MB" --flight-records 0 --journal-records 0 /run/plain.store >/dev/null ||
        die "cannot create the plain store"
    dd if=/run/plain.store of="$plain" bs=1M 2>/dev/null || die "cannot write the plain store to $plain"
    rm -f /run/plain.store
    if [ -n "$DEK" ]; then
        enc=$(part_dev "$dev" 2)
        [ -b "$enc" ] || die "partition $enc did not appear"
        log "writing the encrypted store to $enc"
        truncate -s "${STORE_MB}M" /run/enc.store
        mkstore --size "$STORE_MB" --encrypt --dek-in "$DEK" \
            --flight-records 0 --journal-records 0 /run/enc.store >/dev/null ||
            die "cannot create the encrypted store"
        dd if=/run/enc.store of="$enc" bs=1M 2>/dev/null || die "cannot write the encrypted store to $enc"
        rm -f /run/enc.store
    fi
    if [ "$TARGET_ESP" = 1 ]; then
        esp_part=2
        [ -z "$DEK" ] || esp_part=3
        make_target_esp "$dev" "$esp_part"
    fi
    printf 'layout=1\npayload_mib=%s\ndisk_guid=%s\n' "$PAYLOAD_MIB" "$id" >/run/READY
    rwyctl --store "$plain" put PERF/READY /run/READY >/dev/null ||
        die "cannot write the marker on $plain"
    log "flushing $dev"
    sync
    log "prepared $dev ($id)"
}

for d in $SELECTED; do
    if marker_ok "$d"; then
        if [ "$REPROVISION" = 1 ]; then
            log "$d is prepared; re-provisioning"
        elif ! esp_ok "$d"; then
            log "$d is prepared, but it has no ESP for the file cells; re-provisioning"
        elif ! enc_ok "$d"; then
            log "$d is prepared, but its encrypted store does not open with BENCH.DEK; re-provisioning"
        else
            log "$d is prepared; reusing"
            continue
        fi
    fi
    provision_disk "$d"
    PROVISIONED="$PROVISIONED $d"
done

# Facts about the machine and the disks, for the report: on the ESP and in the results store.
save_hardware_facts() {
    . "$(dirname "$0")/provision_hw.sh"
    hw_report "$BOOT_DISK" $SELECTED >/run/HW.TXT 2>/dev/null || return 0
    mkdir -p "$ESP_MNT/PERF" && cp /run/HW.TXT "$ESP_MNT/PERF/HW.TXT" || log "cannot save HW.TXT on the ESP"
    res=$(part_name "$BOOT_DISK" RESULTS)
    if [ -n "$res" ]; then
        rwyctl --store "$res" put PERF/HW.TXT /run/HW.TXT >/dev/null || log "cannot save HW.TXT in the results store"
    fi
    log "saved the hardware facts"
}
save_hardware_facts

# Put the rows of a report file made under Linux in the results store, before the rows the benchmark adds.
merge_report() {
    cur=/run/store-$1.CSV
    if rwyctl --store "$res" cat "PERF/$1.CSV" >"$cur" 2>/dev/null; then
        tail -n +2 "$out/$1.CSV" >>"$cur"
    else
        cp "$out/$1.CSV" "$cur"
    fi
    rwyctl --store "$res" put "PERF/$1.CSV" "$cur" >/dev/null || log "cannot save $1.CSV in the results store"
}

# The reference numbers of each disk prepared in this session under Linux, to compare with what the firmware path
# reaches. A disk that is only reused keeps the rows of the session that prepared it.
run_linux_bench() {
    [ "$LINUX" = 1 ] || return 0
    res=$(part_name "$BOOT_DISK" RESULTS)
    [ -n "$res" ] || { log "no results partition, skipping the Linux benchmark"; return 0; }
    for d in $PROVISIONED; do
        out=/run/linux-${d##*/}
        mkdir -p "$out"
        log "measuring $d under Linux"
        iobench_linux --rounds "$ROUNDS" --warmup "$WARMUP" --perf-governor --out "$out" "$(part_dev "$d" 1)" ||
            log "the Linux benchmark found a problem on $d"
        [ -s "$out/RUNS.CSV" ] && [ -s "$out/SAMPLES.CSV" ] || continue
        merge_report RUNS
        merge_report SAMPLES
    done
}

# An operator reads the screen, wants a spread of samples and can press a key. An unattended run is a quick check.
if [ -z "$PAUSE" ]; then
    if [ "$UNATTENDED" = 1 ]; then PAUSE=never; else PAUSE=fail; fi
fi
if [ -z "$ROUNDS" ]; then
    if [ "$UNATTENDED" = 1 ]; then ROUNDS=1; else ROUNDS=5; fi
fi
run_linux_bench
cat >/run/IOBENCH.CFG <<CFG
layout=1
label=$LABEL
rounds=$ROUNDS
warmup=$WARMUP
payload_mib=$PAYLOAD_MIB
devices=$DEVICES
disks=$DISK_MODE
include_boot=$INCLUDE_BOOT
result=$RESULT
pause=$PAUSE
CFG
cp /run/IOBENCH.CFG "$ESP_MNT/IOBENCH.CFG" || die "cannot write IOBENCH.CFG"
sync
log "wrote IOBENCH.CFG"
cat /run/IOBENCH.CFG

rwyctl --store "$BOOTSTORE" confirm || die "cannot confirm the provisioning slot"
if rwyctl --store "$BOOTSTORE" get CONFIG1 STATE >/dev/null 2>&1; then
    entry=$(rwyctl --store "$BOOTSTORE" get CONFIG1 ENTRY 2>/dev/null || true)
    state=$(rwyctl --store "$BOOTSTORE" get CONFIG1 STATE 2>/dev/null || true)
    case "$entry" in
    *.efi)
        if [ "$state" = FAILED ]; then
            [ "$UNATTENDED" = 1 ] && die "the benchmark slot already ran; not arming it again without an operator"
            rwyctl --store "$BOOTSTORE" remove CONFIG1 || die "cannot remove the used benchmark slot"
        fi
        ;;
    *) rwyctl --store "$BOOTSTORE" remove CONFIG1 || die "cannot remove the old benchmark slot" ;;
    esac
fi
if ! rwyctl --store "$BOOTSTORE" get CONFIG1 STATE >/dev/null 2>&1; then
    rwyctl --store "$BOOTSTORE" install --name CONFIG1 \
        --kernel /payload/IOBENCH.EFI --tries 1 ||
        die "cannot install the benchmark slot"
fi
rwyctl --store "$BOOTSTORE" status
sync
umount "$ESP_MNT"
trap - EXIT
# Leave the IMSM volumes clean for the firmware.
mdadm --wait-clean --scan >/dev/null 2>&1 || true
# An operator gets time to read the screen; an unattended run does not wait.
if [ -z "$REBOOT_WAIT" ]; then
    if [ "$UNATTENDED" = 1 ]; then REBOOT_WAIT=0; else REBOOT_WAIT=15; fi
fi
log "rebooting into the benchmark slot in $REBOOT_WAIT seconds"
sleep "$REBOOT_WAIT"
reboot -f
