# tests/qemu/iobench_lib.sh — the benchmark image layouts and the readiness marker.
# SPDX-License-Identifier: Apache-2.0
# Source this after tools/imglib.sh. It uses mkstore, rwyctl, sfdisk, mkfs.vfat,
# mcopy and mmd from $BUILD or the PATH.
set -eu
export PATH="$PATH:/sbin:/usr/sbin"

IOBENCH_STORE_MB=${IOBENCH_STORE_MB:-96}
IOBENCH_RAW_MB=${IOBENCH_RAW_MB:-16}
IOBENCH_RESULT_MB=${IOBENCH_RESULT_MB:-8}
IOBENCH_BOOTSTORE_MB=${IOBENCH_BOOTSTORE_MB:-128}
IOBENCH_ESP_MB=${IOBENCH_ESP_MB:-300}
IOBENCH_PART_GUID=5D0A7D6E-3B1C-4C1E-9A42-6F1B8E7021C5
IOBENCH_ENC_GUID=5D0A7D6E-3B1C-4C1E-9A42-6F1B8E7021C6
IOBENCH_RESULT_GUID=5D0A7D6E-3B1C-4C1E-9A42-6F1B8E7021C7
IOBENCH_LAYOUT=1
IOBENCH_PAYLOAD_MIB=${IOBENCH_PAYLOAD_MIB:-16}
IOBENCH_DATA_TYPE=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
IOBENCH_RESULT_TYPE=$IOBENCH_DATA_TYPE

iobench_tools() {
    for t in sfdisk mkfs.vfat mcopy mmd uuidgen; do
        command -v "$t" >/dev/null 2>&1 || { echo "missing $t" >&2; exit 1; }
    done
    MKSTORE=${MKSTORE:-$BUILD/mkstore}
    [ -x "$MKSTORE" ] || MKSTORE=$BUILD/tools/mkstore
    RWYCTL=${RWYCTL:-$BUILD/rwyctl}
    [ -x "$RWYCTL" ] || RWYCTL=$BUILD/tools/rwyctl
    [ -x "$MKSTORE" ] && [ -x "$RWYCTL" ] || { echo "missing mkstore or rwyctl" >&2; exit 1; }
}

iobench_mkstore() {
    "$MKSTORE" --size "$1" --flight-records 0 --journal-records 0 "$2" >/dev/null
}

# A target store: iobench_mkstore plus the read object.
iobench_target_store() {
    "$MKSTORE" --size "$1" --flight-records 0 --journal-records 0 --add "$(iobench_object)" "$2" >/dev/null
}

# The object the read benchmark opens in every target store (PERF/READ16M.BIN).
iobench_object() {
    [ -f "$T/READ16M.BIN" ] || head -c 16777216 /dev/urandom >"$T/READ16M.BIN"
    echo "PERF/READ16M.BIN=$T/READ16M.BIN"
}

iobench_marker() {
    store=$1 disk_id=$2 out=$3
    cat >"$out" <<MARK
layout=$IOBENCH_LAYOUT
payload_mib=$IOBENCH_PAYLOAD_MIB
disk_guid=$disk_id
MARK
    "$RWYCTL" --store "$store" put PERF/READY "$out"
}

iobench_disk_id() {
    if [ -n "${IOBENCH_DISK_ID:-}" ]; then
        echo "$IOBENCH_DISK_ID"
    elif [ -f "$1" ] || [ -b "$1" ]; then
        sfdisk --disk-id "$1" | tr a-f A-F
    else
        uuidgen | tr a-f A-F
    fi
}

# A target image (or a named disk) with the plain store, raw area and optional encrypted store.
# The partitions have the plain data type, not the store type: RUNWAY.EFI mounts every store partition it finds
# and would take the benchmark stores over (and re-encrypt the encrypted one when a TPM is present).
# The last partition is a small ESP with a RWYBENCH.DAT, for the file cells on this disk.
# Usage: iobench_target OUT [DEK_FILE]. IOBENCH_NO_MARKER=1 leaves the marker out for a skip test and
# IOBENCH_NO_ESP=1 leaves the ESP out.
iobench_target() {
    out=$1 dek=${2:-}
    [ -n "$dek" ] && [ ! -f "$dek" ] && { echo "missing DEK $dek" >&2; exit 1; }
    parts=$((IOBENCH_STORE_MB + IOBENCH_RAW_MB))
    enc_mb=0
    [ -n "$dek" ] && enc_mb=$IOBENCH_STORE_MB
    esp_mb=${IOBENCH_TARGET_ESP_MB:-$((IOBENCH_STORE_MB + 64))}
    esp_line="size=${esp_mb}MiB, type=$ESP_TYPE, name=\"BENCH-ESP\""
    [ -z "${IOBENCH_NO_ESP:-}" ] || { esp_mb=0 esp_line=; }
    mb=$((1 + parts + enc_mb + esp_mb + 1))
    if [ -b "$out" ]; then
        wipefs -a "$out" >/dev/null 2>&1 || true
    else
        rm -f "$out"
        truncate -s "${mb}M" "$out"
    fi
    if [ -n "$dek" ]; then
        sfdisk "$out" >/dev/null <<PARTS
label: gpt
size=${parts}MiB, type=$IOBENCH_DATA_TYPE, uuid=$IOBENCH_PART_GUID, name="IOBENCH"
size=${IOBENCH_STORE_MB}MiB, type=$IOBENCH_DATA_TYPE, uuid=$IOBENCH_ENC_GUID, name="IOBENCH-ENC"
$esp_line
PARTS
    else
        sfdisk "$out" >/dev/null <<PARTS
label: gpt
size=${parts}MiB, type=$IOBENCH_DATA_TYPE, uuid=$IOBENCH_PART_GUID, name="IOBENCH"
$esp_line
PARTS
    fi
    disk_id=$(iobench_disk_id "$out")
    sfdisk --disk-id "$out" "$disk_id" >/dev/null

    plain="$out@$(img_part_off "$out" 1)+$(img_part_bytes "$out" 1)"
    iobench_target_store "$IOBENCH_STORE_MB" "$plain"
    if [ -n "$dek" ]; then
        enc="$out@$(img_part_off "$out" 2)+$(img_part_bytes "$out" 2)"
        "$MKSTORE" --size "$IOBENCH_STORE_MB" --encrypt --dek-in "$dek" \
            --flight-records 0 --journal-records 0 --add "$(iobench_object)" "$enc" >/dev/null
    fi
    if [ -z "${IOBENCH_NO_ESP:-}" ]; then
        esp_part=2
        [ -z "$dek" ] || esp_part=3
        esp_start=$(($(img_part_off "$out" $esp_part) / 512))
        mkfs.vfat --offset "$esp_start" -F 32 -s 2 -n BENCHESP "$out" $((esp_mb * 1024)) >/dev/null
        iobench_target_store "$IOBENCH_STORE_MB" "$T/RWYBENCH.DAT"
        mcopy -i "$out@@$((esp_start * 512))" "$T/RWYBENCH.DAT" ::RWYBENCH.DAT
    fi
    if [ -z "${IOBENCH_NO_MARKER:-}" ]; then
        t=$(mktemp -d /var/tmp/rwy-iobench-mark.XXXXXX)
        iobench_marker "$plain" "$disk_id" "$t/READY"
        rm -rf "$t"
    fi
}

# Write the base keys and config files of a boot or combined ESP.
iobench_esp_files() {
    esp=$1 app=$2 config=$3
    mmd -i "$esp" ::EFI ::EFI/BOOT
    if [ -n "${RUNWAY_EFI:-}" ] && [ -f "$RUNWAY_EFI" ]; then
        mcopy -o -i "$esp" "$RUNWAY_EFI" "::EFI/BOOT/${QEMU_BOOT_FILE:-BOOTX64.EFI}"
        mcopy -o -i "$esp" "$RUNWAY_EFI" ::RUNWAY.EFI
    else
        mcopy -o -i "$esp" "$app" "::EFI/BOOT/${QEMU_BOOT_FILE:-BOOTX64.EFI}"
    fi
    mcopy -o -i "$esp" "$app" ::IOBENCH.EFI
    printf '%s\n' "$config" >"$T/IOBENCH.CFG"
    mcopy -i "$esp" "$T/IOBENCH.CFG" ::IOBENCH.CFG
}

# Kernel arguments of the provisioning slot. The last console= is /dev/console, where the dialog runs. On a PC
# that is the screen, with the serial port as a second output; boards without a display keep the serial port.
# A test that reads the serial port sets IOBENCH_BOOT_ARGS.
iobench_boot_args() {
    case "${ARCH:-$(uname -m)}" in
    x86_64|amd64) echo "console=ttyS0,115200 console=tty0 rdinit=/init iobench=1" ;;
    *) echo "console=ttyS0,115200 console=ttyAMA0,115200 rdinit=/init iobench=1" ;;
    esac
}

# A boot USB image: ESP, a plain Runway slot store with the provisioning slot, and the results store.
# Usage: iobench_boot OUT APP RUNWAY KERNEL INITRD [DEK_FILE]
iobench_boot() {
    out=$1 app=$2 runway=$3 kernel=$4 initrd=$5 dek=${6:-}
    [ -n "$kernel" ] && [ -n "$initrd" ] || { echo "the boot image needs a kernel and an initrd" >&2; exit 1; }
    mb=$((1 + IOBENCH_ESP_MB + IOBENCH_BOOTSTORE_MB + IOBENCH_RESULT_MB + 1))
    rm -f "$out"
    truncate -s "${mb}M" "$out"
    sfdisk "$out" >/dev/null <<PARTS
label: gpt
start=2048, size=${IOBENCH_ESP_MB}MiB, type=$ESP_TYPE, name="ESP"
size=${IOBENCH_BOOTSTORE_MB}MiB, type=$XBL_TYPE, name="BOOTSTORE"
size=${IOBENCH_RESULT_MB}MiB, type=$IOBENCH_DATA_TYPE, uuid=$IOBENCH_RESULT_GUID, name="RESULTS"
PARTS
    esp_off=$((2048 * 512))
    mkfs.vfat --offset 2048 -F 32 -s 8 -n ESP "$out" $((IOBENCH_ESP_MB * 1024)) >/dev/null
    RUNWAY_EFI=$runway iobench_esp_files "$out@@$esp_off" "$app" "$(iobench_config)"
    if [ -n "$dek" ]; then mcopy -i "$out@@$esp_off" "$dek" ::BENCH.DEK; fi
    iobench_target_store "$IOBENCH_STORE_MB" "$T/RWYBENCH.DAT"
    mcopy -i "$out@@$esp_off" "$T/RWYBENCH.DAT" ::RWYBENCH.DAT

    bootstore="$out@$(img_part_off "$out" 2)+$(img_part_bytes "$out" 2)"
    result="$out@$(img_part_off "$out" 3)+$(img_part_bytes "$out" 3)"
    "$MKSTORE" --size "$IOBENCH_BOOTSTORE_MB" --slots 2 "$bootstore" >/dev/null
    "$RWYCTL" --store "$bootstore" install --name CONFIG0 --kernel "$kernel" --initrd "$initrd" \
        --args "${IOBENCH_BOOT_ARGS:-$(iobench_boot_args)}" \
        --label provisioning --tries 3 >/dev/null
    # Five seconds to read the loader lines before the provisioning kernel takes the screen.
    "$RWYCTL" --store "$bootstore" set BOOT-DELAY 5 >/dev/null
    iobench_mkstore "$IOBENCH_RESULT_MB" "$result"
}

# A combined QEMU and USB-only image: direct benchmark boot plus target partitions and results.
# Usage: iobench_combined OUT APP [DEK_FILE]
iobench_combined() {
    out=$1 app=$2 dek=${3:-}
    parts=$((IOBENCH_STORE_MB + IOBENCH_RAW_MB))
    mb=$((1 + IOBENCH_ESP_MB + parts + IOBENCH_STORE_MB + IOBENCH_RESULT_MB + 1))
    rm -f "$out"
    truncate -s "${mb}M" "$out"
    sfdisk "$out" >/dev/null <<PARTS
label: gpt
start=2048, size=${IOBENCH_ESP_MB}MiB, type=$ESP_TYPE, name="ESP"
size=${parts}MiB, type=$IOBENCH_DATA_TYPE, uuid=$IOBENCH_PART_GUID, name="IOBENCH"
size=${IOBENCH_STORE_MB}MiB, type=$IOBENCH_DATA_TYPE, uuid=$IOBENCH_ENC_GUID, name="IOBENCH-ENC"
size=${IOBENCH_RESULT_MB}MiB, type=$IOBENCH_DATA_TYPE, uuid=$IOBENCH_RESULT_GUID, name="RESULTS"
PARTS
    disk_id=$(iobench_disk_id "$out")
    sfdisk --disk-id "$out" "$disk_id" >/dev/null
    esp_off=$((2048 * 512))
    mkfs.vfat --offset 2048 -F 32 -s 8 -n ESP "$out" $((IOBENCH_ESP_MB * 1024)) >/dev/null
    iobench_esp_files "$out@@$esp_off" "$app" "$(iobench_config combined_rounds)"
    iobench_target_store "$IOBENCH_STORE_MB" "$T/RWYBENCH.DAT"
    mcopy -i "$out@@$esp_off" "$T/RWYBENCH.DAT" ::RWYBENCH.DAT

    plain="$out@$(img_part_off "$out" 2)+$(img_part_bytes "$out" 2)"
    enc="$out@$(img_part_off "$out" 3)+$(img_part_bytes "$out" 3)"
    result="$out@$(img_part_off "$out" 4)+$(img_part_bytes "$out" 4)"
    iobench_target_store "$IOBENCH_STORE_MB" "$plain"
    if [ -z "$dek" ]; then
        "$MKSTORE" --size "$IOBENCH_STORE_MB" --encrypt --dek-out "$T/BENCH.DEK" \
            --flight-records 0 --journal-records 0 --add "$(iobench_object)" "$enc" >/dev/null
        dek=$T/BENCH.DEK
    else
        "$MKSTORE" --size "$IOBENCH_STORE_MB" --encrypt --dek-in "$dek" \
            --flight-records 0 --journal-records 0 --add "$(iobench_object)" "$enc" >/dev/null
    fi
    mcopy -i "$out@@$esp_off" "$dek" ::BENCH.DEK
    iobench_marker "$plain" "$disk_id" "$T/READY"
    iobench_mkstore "$IOBENCH_RESULT_MB" "$result"
}

iobench_config() {
    mode=${1:-boot}
    if [ "$mode" = combined_rounds ]; then
        rounds=${IOBENCH_ROUNDS:-${ROUNDS:-1}}
        warmup=${IOBENCH_WARMUP:-0}
        disks=${IOBENCH_DISKS:-all}
        include_boot=${IOBENCH_INCLUDE_BOOT:-1}
    else
        rounds=${IOBENCH_ROUNDS:-${ROUNDS:-5}}
        warmup=${IOBENCH_WARMUP:-1}
        disks=${IOBENCH_DISKS:-internal}
        include_boot=${IOBENCH_INCLUDE_BOOT:-0}
    fi
    cat <<CFG
layout=$IOBENCH_LAYOUT
label=${IOBENCH_LABEL:-bf}
rounds=$rounds
warmup=$warmup
payload_mib=$IOBENCH_PAYLOAD_MIB
devices=${IOBENCH_DEVICES:-all}
disks=$disks
include_boot=$include_boot
result=${IOBENCH_RESULT:-auto}
pause=${IOBENCH_PAUSE:-never}
CFG
}
