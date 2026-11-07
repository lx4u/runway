#!/bin/sh
# rwyctl: store discovery on fake sysfs/mountinfo trees, mirror,
# keys, and the read-only commands stores / status / get. Host only: no root, no QEMU, no FUSE.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
CTL=$BUILD/rwyctl
HFS=$BUILD/host_fs
T=$(mktemp -d /var/tmp/rwy-rwyctl.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail() { echo "RWYCTL FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }   # got want what
code() { "$@" >"$T/out" 2>"$T/err" </dev/null; echo $?; }
json_ok() { command -v python3 >/dev/null 2>&1 || return 0; python3 -I -m json.tool <"$1" >/dev/null || { cat "$1" >&2; fail "$2"; }; }
has() { grep -qF -- "$2" "$1" || { cat "$1" >&2; fail "missing '$2' in $1"; }; }

UUID1=11111111-2222-3333-4444-555555555555
UUID2=66666666-7777-8888-9999-aaaaaaaaaaaa
UUID3=66666666-7777-8888-9999-bbbbbbbbbbbb
mk() { $BUILD/mkstore --size 64 "$@" >/dev/null || fail "mkstore $*"; }
slot() {   # slot STORE N STATE
    $HFS "$1" mkdir EFI/RUNWAY/CONFIG$2 && $HFS "$1" put EFI/RUNWAY/CONFIG$2/KERNEL "kernel$2" &&
    $HFS "$1" put EFI/RUNWAY/CONFIG$2/ARGS "root=/dev/x$2" && $HFS "$1" put EFI/RUNWAY/CONFIG$2/STATE "INSTALLED
" || fail "slot $2"
    [ "$3" = INSTALLED ] || HOST_FS_SYSTEM=1 $HFS "$1" put EFI/RUNWAY/CONFIG$2/STATE "$3
" || fail "slot $2 $3"
}
seed() { $HFS "$1" mkdir EFI/RUNWAY || fail seed; slot "$1" 1 OK; slot "$1" 2 INSTALLED; }

# --- 1. plain store, --store ---------------------------------------------------------------
mk --uuid $UUID1 "$T/a.store"
want "$(code $CTL --store "$T/a.store" status)" 0 "status without EFI/RUNWAY"
has "$T/err" "EFI/RUNWAY is absent"
want "$(code $CTL --store "$T/a.store" status --json)" 0 "status --json without EFI/RUNWAY"
want "$(cat "$T/out")" "{}" "status --json without EFI/RUNWAY prints an empty object"
# install refuses without EFI/RUNWAY and points at init; init only creates the empty subtree
cp "$T/a.store" "$T/n0.store"; echo kimg >"$T/k0"
want "$(code $CTL --store "$T/n0.store" install --kernel "$T/k0")" 1 "install without EFI/RUNWAY"
has "$T/err" "rwyctl init"
want "$(code $CTL --store "$T/n0.store" init)" 0 "init"
want "$(code $BUILD/fsck.runway "$T/n0.store")" 0 "init leaves a clean store"
want "$($HFS "$T/n0.store" cat EFI/RUNWAY/VERSION)" 1 "init writes VERSION"
$CTL --store "$T/n0.store" status >"$T/out" 2>&1; grep -q "^CONFIG" "$T/out" && fail "init made a slot"
want "$(code $CTL --store "$T/n0.store" init)" 0 "init twice"
has "$T/err" "already"
want "$(code $CTL --store "$T/n0.store" install --kernel "$T/k0")" 0 "install after init"
seed "$T/a.store"
want "$(code $CTL --store "$T/a.store" status)" 0 "status"
has "$T/out" "CONFIG1"
awk '$1=="CONFIG2" && $2!="-" && $3=="INSTALLED"' "$T/out" | grep -q . || { cat "$T/out"; fail "CONFIG2 row"; }
has "$T/out" "INCOMPLETE=0"
has "$T/out" "RECOVERY=absent"
want "$($CTL --store "$T/a.store" get CONFIG1 STATE)" OK "get STATE"
want "$($CTL --store "$T/a.store" get CONFIG2 ARGS)" "root=/dev/x2" "get ARGS"
want "$($CTL --store "$T/a.store" get CONFIG2 INDEX)" 2 "get INDEX"
want "$($CTL --store "$T/a.store" get REVISION)" 2 "get global REVISION"
want "$(code $CTL --store "$T/a.store" get CONFIG1 WATCHDOG)" 1 "get unset field"
want "$($CTL --store "$T/a.store" get CONFIG1 TRIES)" 1 "TRIES of a final slot reads the default"
want "$(code $CTL --store "$T/a.store" get CONFIG1 BOGUS)" 2 "get unknown field"
want "$(code $CTL --store "$T/a.store" get NOPE STATE)" 2 "get bad slot name"
want "$(code $CTL --store "$T/a.store" bogus)" 2 "unknown command"
want "$(code $CTL --store "$T/none.store" status)" 3 "missing store file"
want "$(code $CTL --store "$T/a.store" stores)" 0 "stores"
has "$T/out" "$UUID1"
# read-only: the store file is untouched
sum1=$(sha256sum "$T/a.store"); $CTL --store "$T/a.store" status >/dev/null; want "$(sha256sum "$T/a.store")" "$sum1" "store changed"
# an incomplete slot (no REVISION) is listed and counted
$HFS "$T/a.store" mkdir EFI/RUNWAY/CONFIG3
$CTL --store "$T/a.store" status >"$T/out"; has "$T/out" "INCOMPLETE=1"

# --- 2. mirror: lagging member ----------------------------------------------------------------
cp "$T/a.store" "$T/b.store"
slot "$T/a.store" 4 INSTALLED           # a is newer than b
want "$(code $CTL --store "$T/b.store" --store "$T/a.store" status)" 0 "mirror status"
has "$T/err" "lagging"
has "$T/out" "CONFIG4"
want "$(code $CTL --store "$T/b.store" --store "$T/a.store" status --json)" 0 "mirror status --json"
json_ok "$T/out" "mirror status --json is valid JSON"
has "$T/out" '"mirror": ['
has "$T/out" '"record_resets": 0'
want "$(code $CTL --store "$T/a.store" --store "$T/b.store" get CONFIG4 STATE)" 0 "mirror order independent"
want "$(code $CTL --store "$T/a.store" --store "$T/gone.store" status)" 0 "missing member tolerated"
has "$T/err" "missing"
mk --uuid $UUID2 "$T/c.store"
want "$(code $CTL --store "$T/a.store" --store "$T/c.store" status)" 2 "members of different stores"

# --- 3. discovery on fake sysfs / mountinfo / by-partuuid ---------------------------------------------
R=$T/root
PU=0a0b0c0d-1111-2222-3333-444444444444
mkdir -p "$R/proc/self" "$R/dev/disk/by-partuuid" "$R/sys/firmware/efi/efivars" "$R/sys/class/block" \
         "$R/sys/devices/sda/sda1" "$R/sys/devices/sdb/sdb1" "$R/sys/block" "$T/mnt-a" "$T/mnt-b"
ln -s ../devices/sda "$R/sys/block/sda"; ln -s ../devices/sdb "$R/sys/block/sdb"
ln -s ../../devices/sda/sda1 "$R/sys/class/block/sda1"; ln -s ../../devices/sdb/sdb1 "$R/sys/class/block/sdb1"
: >"$R/sys/devices/sda/sda1/partition"; : >"$R/sys/devices/sdb/sdb1/partition"
: >"$R/dev/sda1"; : >"$R/dev/sdb1"
ln -s ../../sda1 "$R/dev/disk/by-partuuid/$PU"
cp "$T/a.store" "$T/mnt-a/RUNWAY.DAT"; cp "$T/c.store" "$T/mnt-b/RUNWAY.DAT"
printf '39 1 8:1 / %s rw - vfat /dev/sda1 rw\n40 1 8:17 / %s rw - vfat /dev/sdb1 rw\n' "$T/mnt-a" "$T/mnt-b" >"$R/proc/self/mountinfo"
{ printf '\007\000\000\000'; printf %s "$PU" | iconv -t UTF-16LE; printf '\000\000'; } \
    >"$R/sys/firmware/efi/efivars/LoaderDevicePartUUID-4a67b082-0a4c-41cf-b6c7-440b29bb8c4f"
export RWYCTL_ROOT=$R
want "$(code $CTL status)" 0 "boot ESP discovery"
has "$T/out" "CONFIG4"
want "$(code $CTL stores)" 0 "stores on the boot disk"
has "$T/out" "$UUID1"; grep -q "$UUID2" "$T/out" && fail "other disk listed without --all-devices"
want "$(code $CTL --all-devices stores)" 0 "stores --all-devices"
has "$T/out" "$UUID2"
awk -v u=$UUID1 '$1==u && $5=="boot"' "$T/out" | grep -q . || fail "boot mark"
want "$(code $CTL --uuid $UUID2 status)" 0 "--uuid other store"
has "$T/err" "not the boot store"
want "$(code $CTL --device /dev/sda1 get CONFIG1 STATE)" 0 "--device"
want "$(code $CTL members)" 0 "members of the boot store"
want "$(cat "$T/out")" "$T/mnt-a/RUNWAY.DAT" "members path"
# SYSTEM operations are refused on a store found on the running system, allowed on a named one
sumd=$(sha256sum "$T/mnt-a/RUNWAY.DAT")
want "$(code $CTL check --repair --unlock)" 6 "repair on a discovered store"
want "$(code $CTL recovery set --unlock --kernel "$T/a.store")" 6 "recovery set on a discovered store"
want "$(sha256sum "$T/mnt-a/RUNWAY.DAT")" "$sumd" "refused SYSTEM command wrote"
want "$(code $CTL --device /dev/sda1 check --repair --unlock)" 0 "repair on a named device"
# two --device partitions with the same store form a mirror (sdb1 holds an older copy of a)
mkdir -p "$T/mnt-c"; cp "$T/b.store" "$T/mnt-c/RUNWAY.DAT"
printf '41 1 8:33 / %s rw - vfat /dev/sdc1 rw\n' "$T/mnt-c" >>"$R/proc/self/mountinfo"
mkdir -p "$R/sys/devices/sdc/sdc1"; : >"$R/sys/devices/sdc/sdc1/partition"; : >"$R/dev/sdc1"
want "$(code $CTL --device /dev/sda1 --device /dev/sdc1 status)" 0 "mirror of two --device"
has "$T/err" "lagging"; has "$T/out" CONFIG4
want "$(code $CTL --device /dev/sda1 --device /dev/sdc1 members)" 0 "members of a mirror"
want "$(wc -l <"$T/out")" 2 "members of a mirror: one line each"
want "$(code $CTL --device /dev/sda1 --device /dev/sdb1 status)" 2 "--device of different stores"
want "$(code $CTL --device /dev/sda1 --device /dev/nope status)" 0 "missing --device member tolerated"
has "$T/err" "missing"
# RunwayBootESP wins over LoaderDevicePartUUID; a value that names nothing is skipped
PU2=0a0b0c0d-5555-6666-7777-888888888888
ln -s ../../sdb1 "$R/dev/disk/by-partuuid/$PU2"
rwyesp() { { printf '\007\000\000\000'; printf %s "$1" | iconv -t UTF-16LE; printf '\000\000'; } \
    >"$R/sys/firmware/efi/efivars/RunwayBootESP-b371a076-b7ef-4e6e-b98a-8d0d2c493c7b"; }
rwyesp "$(echo $PU2 | tr a-f A-F)"
want "$(code $CTL stores)" 0 "stores with RunwayBootESP"
awk -v u=$UUID2 '$1==u && $5=="boot"' "$T/out" | grep -q . || fail "RunwayBootESP does not win"
rwyesp 00000000-0000-0000-0000-000000000000
want "$(code $CTL stores)" 0 "stores with a wrong RunwayBootESP"
awk -v u=$UUID1 '$1==u && $5=="boot"' "$T/out" | grep -q . || fail "wrong RunwayBootESP does not fall back"
rm "$R/sys/firmware/efi/efivars/RunwayBootESP-"*
rm "$R/sys/firmware/efi/efivars/"*                      # no LoaderDevicePartUUID: every disk, two stores
want "$(code $CTL status)" 3 "several stores, none chosen"
has "$T/err" "none chosen"
has "$T/err" "boot ESP unknown"
want "$(code $CTL members)" 3 "members, several stores and none chosen"
[ -s "$T/out" ] && fail "members printed a path without a choice"
want "$(code $CTL --uuid $UUID2 members)" 0 "members with --uuid"
has "$T/out" "mnt-b/RUNWAY.DAT"
want "$(code $CTL --uuid $UUID1 status)" 0 "--uuid without efivar"
unset RWYCTL_ROOT
want "$(RWYCTL_ROOT=$T/empty code $CTL status)" 3 "no store found"
want "$(RWYCTL_ROOT=$T/empty code $CTL members)" 3 "members, no store found"
has "$T/err" "no Runway store"

# --- 3c. RunwayBootMembers: the members Runway used, on other disks too ---------------------------------
# sda1 and sdc1 hold the mirror of UUID1 (sdc1 is older), sdd1 a same-generation copy that was not used at boot
M=$T/mroot
PU3=0a0b0c0d-7777-8888-9999-aaaaaaaaaaaa
PU4=0a0b0c0d-bbbb-cccc-dddd-eeeeeeeeeeee
mkdir -p "$M/proc/self" "$M/dev/disk/by-partuuid" "$M/sys/firmware/efi/efivars" "$M/sys/class/block" "$M/sys/block" "$T/mnt-d"
for d in a b c d; do
    mkdir -p "$M/sys/devices/sd$d/sd${d}1"; : >"$M/sys/devices/sd$d/sd${d}1/partition"; : >"$M/dev/sd${d}1"
    ln -s ../devices/sd$d "$M/sys/block/sd$d"; ln -s ../../devices/sd$d/sd${d}1 "$M/sys/class/block/sd${d}1"
done
ln -s ../../sda1 "$M/dev/disk/by-partuuid/$PU"; ln -s ../../sdb1 "$M/dev/disk/by-partuuid/$PU2"
ln -s ../../sdc1 "$M/dev/disk/by-partuuid/$PU3"; ln -s ../../sdd1 "$M/dev/disk/by-partuuid/$PU4"
cp "$T/a.store" "$T/mnt-d/RUNWAY.DAT"
printf '39 1 8:1 / %s rw - vfat /dev/sda1 rw\n40 1 8:17 / %s rw - vfat /dev/sdb1 rw\n41 1 8:33 / %s rw - vfat /dev/sdc1 rw\n42 1 8:49 / %s rw - vfat /dev/sdd1 rw\n' \
    "$T/mnt-a" "$T/mnt-b" "$T/mnt-c" "$T/mnt-d" >"$M/proc/self/mountinfo"
efivar() { { printf '\007\000\000\000'; printf %s "$2" | iconv -t UTF-16LE; printf '\000\000'; } >"$M/sys/firmware/efi/efivars/$1-b371a076-b7ef-4e6e-b98a-8d0d2c493c7b"; }
efivar RunwayBootESP "$PU"
members() { awk -v u=$UUID1 '$1==u {print $2}' "$T/out"; }
export RWYCTL_ROOT=$M
want "$(code $CTL stores)" 0 "stores without RunwayBootMembers"
want "$(members)" 1 "members without the variable (boot disk only)"
efivar RunwayBootMembers "$PU $PU3"
want "$(code $CTL stores)" 0 "stores with RunwayBootMembers"
want "$(members)" 2 "members from RunwayBootMembers"
awk -v u=$UUID1 '$1==u && $5=="boot"' "$T/out" | grep -q . || fail "members: boot mark"
want "$(code $CTL status)" 0 "status over members of two disks"
has "$T/err" "lagging"
efivar RunwayBootMembers "$PU 0a0b0c0d-9999-9999-9999-999999999999"
want "$(code $CTL status)" 0 "a listed member that does not exist"
has "$T/err" "missing"
efivar RunwayBootMembers "$PU $PU2"
want "$(code $CTL status)" 0 "a listed member of another store"
has "$T/err" "another store"
want "$(code $CTL stores)" 0 "stores with a member of another store"
want "$(members)" 1 "the other store's member stays out of the mirror"
# a copy that was not used at boot is noted and not written
efivar RunwayBootMembers "$PU"
sumd=$(sha256sum <"$T/mnt-d/RUNWAY.DAT"); suma=$(sha256sum <"$T/mnt-a/RUNWAY.DAT")
want "$(code $CTL --all-devices set WATCHDOG 30)" 0 "write with a member not used at boot"
has "$T/err" "not used at boot"
[ "$(sha256sum <"$T/mnt-a/RUNWAY.DAT")" != "$suma" ] || fail "the boot member was not written"
want "$(sha256sum <"$T/mnt-d/RUNWAY.DAT")" "$sumd" "a member not used at boot was written"
unset RWYCTL_ROOT

# --- 3d. no by-partuuid directory: the GPT of each disk gives the boot ESP -------------------------------
PATH=$PATH:/usr/sbin:/sbin
if command -v sfdisk >/dev/null 2>&1; then
    G=$T/groot
    PU5=0a0b0c0d-cccc-dddd-eeee-ffffffffffff
    mkdir -p "$G/proc/self" "$G/dev" "$G/sys/firmware/efi/efivars" "$G/sys/class/block" "$G/sys/block" "$G/sys/devices/sda/sda1" "$G/sys/devices/sda/sda2"
    truncate -s 4M "$G/dev/sda"
    printf 'label: gpt\nstart=2048, size=1024, type=U, uuid=%s\nstart=4096, size=1024, type=U, uuid=%s\n' $PU2 $PU5 | sfdisk "$G/dev/sda" >/dev/null ||
        fail "sfdisk"
    echo 1 >"$G/sys/devices/sda/sda1/partition"; echo 2 >"$G/sys/devices/sda/sda2/partition"
    : >"$G/dev/sda1"; : >"$G/dev/sda2"
    ln -s ../devices/sda "$G/sys/block/sda"
    ln -s ../../devices/sda/sda1 "$G/sys/class/block/sda1"; ln -s ../../devices/sda/sda2 "$G/sys/class/block/sda2"
    printf '39 1 8:1 / %s rw - vfat /dev/sda1 rw\n40 1 8:2 / %s rw - vfat /dev/sda2 rw\n' "$T/mnt-b" "$T/mnt-a" >"$G/proc/self/mountinfo"
    { printf '\007\000\000\000'; printf %s "$(echo $PU5 | tr a-f A-F)" | iconv -t UTF-16LE; printf '\000\000'; } \
        >"$G/sys/firmware/efi/efivars/RunwayBootESP-b371a076-b7ef-4e6e-b98a-8d0d2c493c7b"
    export RWYCTL_ROOT=$G
    want "$(code $CTL stores)" 0 "stores with the GPT only"
    awk -v u=$UUID1 '$1==u && $5=="boot"' "$T/out" | grep -q . || fail "GPT: boot ESP not found"
    grep -q "boot ESP unknown" "$T/err" && fail "GPT: warned about an unknown boot ESP"
    rm "$G/sys/firmware/efi/efivars/"*
    want "$(code $CTL status)" 3 "no variable, two stores"
    has "$T/err" "boot ESP unknown"
    unset RWYCTL_ROOT
fi

# --- 3e. a raw store is only taken from a partition of the store type -------------------------------------
if command -v sfdisk >/dev/null 2>&1; then
    X=$T/xroot
    XB=bc13c2ff-59e6-4262-a352-b275fd6f7172
    XB_OLD=bc13c2ff-59e4-4262-a352-b275d65f7188     # a former, non-standard value is no store partition
    mkdir -p "$X/proc/self" "$X/dev" "$X/sys/block" "$X/sys/class/block" "$X/sys/devices/sda/sda1" "$X/sys/devices/sda/sda2"
    : >"$X/proc/self/mountinfo"
    truncate -s 4M "$X/dev/sda"
    printf 'label: gpt\nstart=2048, size=1024, type=%s\nstart=4096, size=1024, type=%s\n' $XB_OLD $XB | sfdisk "$X/dev/sda" >/dev/null || fail "sfdisk"
    echo 1 >"$X/sys/devices/sda/sda1/partition"; echo 2 >"$X/sys/devices/sda/sda2/partition"
    ln -s ../devices/sda "$X/sys/block/sda"
    ln -s ../../devices/sda/sda1 "$X/sys/class/block/sda1"; ln -s ../../devices/sda/sda2 "$X/sys/class/block/sda2"
    cp "$T/a.store" "$X/dev/sda1"; : >"$X/dev/sda2"
    export RWYCTL_ROOT=$X
    want "$(code $CTL stores)" 0 "stores, raw store on a partition of another type"
    grep -q "$UUID1" "$T/out" && fail "a raw store on a partition of another type was taken"
    cp "$T/a.store" "$X/dev/sda2"; : >"$X/dev/sda1"
    want "$(code $CTL stores)" 0 "raw store on a store partition"
    has "$T/out" "$UUID1"
    # a RUNWAY.DAT on the same disk is ignored, as RUNWAY.EFI does; it is used when there is no store partition
    printf '39 1 8:1 / %s rw - vfat /dev/sda1 rw\n' "$T/mnt-b" >"$X/proc/self/mountinfo"
    : >"$X/dev/sda1"
    want "$(code $CTL stores)" 0 "stores, partition and RUNWAY.DAT on one disk"
    has "$T/out" "$UUID1"; grep -q "$UUID2" "$T/out" && fail "RUNWAY.DAT taken next to a store partition"
    : >"$X/dev/sda2"
    want "$(code $CTL stores)" 0 "stores, RUNWAY.DAT only"
    has "$T/out" "$UUID2"
    unset RWYCTL_ROOT
fi

# --- 3b. --help, and why a partition was skipped (-v) -----------------------------------------------
want "$(code $CTL --help)" 0 "rwyctl --help"
has "$T/out" "usage: rwyctl"
want "$(code $BUILD/mkstore --help)" 0 "mkstore --help"
has "$T/out" "usage: mkstore"
R2=$T/root2
mkdir -p "$R2/proc/self" "$R2/sys/block" "$T/mnt-e" "$T/mnt-f"
printf '50 1 8:49 / %s rw - vfat /dev/sde1 rw\n51 1 8:65 / %s rw - vfat /dev/sdf1 rw\n' "$T/mnt-e" "$T/mnt-f" >"$R2/proc/self/mountinfo"
for d in sdd sde sdf; do
    mkdir -p "$R2/sys/devices/$d/${d}1"; : >"$R2/sys/devices/$d/${d}1/partition"; ln -s ../devices/$d "$R2/sys/block/$d"
done
head -c 8192 /dev/zero >"$T/mnt-f/RUNWAY.DAT"            # sdd1 is not mounted, sde1 has no store, sdf1 a bad one
export RWYCTL_ROOT=$R2
want "$(code $CTL --all-devices -v stores)" 0 "-v stores"
has "$T/err" "skipped /dev/sdd1: mount: "
has "$T/err" "skipped /dev/sde1: RUNWAY.DAT: No such file or directory"
has "$T/err" "skipped /dev/sdf1: not a Runway store (no valid superblock)"
want "$(code $CTL --all-devices stores)" 0 "stores without -v"
grep -q skipped "$T/err" && fail "skipped lines without -v"
want "$(code $CTL --all-devices status)" 3 "no store behind the skips"
has "$T/err" "use -v"
unset RWYCTL_ROOT

# --- 3c. raw store partitions: a RUNWAY.DAT on the same disk is ignored; nothing is written ---------------------
R3=$T/root3
PU3A=0a0b0c0d-aaaa-bbbb-cccc-dddddddddddd
mkdir -p "$R3/proc/self" "$R3/dev/disk/by-partuuid" "$R3/sys/block" "$R3/sys/class/block" "$T/mnt-g"
for n in 1 2 3; do mkdir -p "$R3/sys/devices/sda/sda$n"; : >"$R3/sys/devices/sda/sda$n/partition"; done
ln -s ../devices/sda "$R3/sys/block/sda"; ln -s ../../devices/sda/sda1 "$R3/sys/class/block/sda1"
ln -s ../../sda1 "$R3/dev/disk/by-partuuid/$PU3A"
cp "$T/a.store" "$R3/dev/sda1"                           # sda1 holds a store, no file system
head -c 100000 /dev/urandom >"$R3/dev/sda2"              # sda2 is a FAT file system with RUNWAY.DAT
cp "$T/c.store" "$T/mnt-g/RUNWAY.DAT"
head -c 100000 /dev/urandom >"$R3/dev/sda3"              # sda3 is nothing we know
printf '60 1 8:2 / %s rw - vfat /dev/sda2 rw\n' "$T/mnt-g" >"$R3/proc/self/mountinfo"
sums=$(sha256sum "$R3/dev/sda1" "$R3/dev/sda2" "$R3/dev/sda3" "$T/mnt-g/RUNWAY.DAT")
export RWYCTL_ROOT=$R3
want "$(code $CTL --all-devices -v stores)" 0 "stores with raw partitions"
has "$T/out" "$UUID1"; grep -q "$UUID2" "$T/out" && fail "RUNWAY.DAT taken next to a store partition"
grep -q "skipped /dev/sda1" "$T/err" && fail "a store partition was skipped"
want "$(sha256sum "$R3/dev/sda1" "$R3/dev/sda2" "$R3/dev/sda3" "$T/mnt-g/RUNWAY.DAT")" "$sums" "discovery wrote"
want "$(code $CTL --device /dev/sda1 get CONFIG1 STATE)" 0 "--device on a raw store"
want "$(code $CTL --device /dev/sda1 check --repair --unlock)" 0 "write command on a raw store"
want "$(code $CTL --store PARTUUID=$PU3A get CONFIG1 STATE)" 0 "--store PARTUUID="
want "$(code $CTL --store PARTUUID=$(echo $PU3A | tr a-f A-F) get CONFIG1 STATE)" 0 "--store PARTUUID= in capitals"
want "$(code $CTL --store PARTUUID=00000000-0000-0000-0000-000000000000 status)" 3 "--store with an unknown PARTUUID"
unset RWYCTL_ROOT

# --- 4. encrypted store ---------------------------------------------------------------------------
mk --encrypt --passphrase hunter2 --dek-out "$T/dek" --uuid $UUID1 "$T/e.store"
export HOST_FS_PASSPHRASE=hunter2; seed "$T/e.store"; unset HOST_FS_PASSPHRASE
want "$(code $CTL --store "$T/e.store" stores)" 0 "stores needs no key"
has "$T/out" yes
want "$(code $CTL --store "$T/e.store" status)" 4 "locked, no key, no terminal"
echo hunter2 >"$T/pass"; echo wrong >"$T/badpass"
want "$(code $CTL --store "$T/e.store" --passphrase-file "$T/pass" status)" 0 "passphrase file"
has "$T/out" CONFIG2
want "$(code $CTL --store "$T/e.store" --passphrase-file "$T/badpass" status)" 4 "wrong passphrase"
has "$T/err" "authentication failed"
want "$(RWY_KEY=hunter2 code $CTL --store "$T/e.store" get CONFIG1 STATE)" 0 "RWY_KEY"
want "$(code $CTL --store "$T/e.store" --dek-file "$T/dek" get CONFIG1 STATE)" 0 "dek file"
head -c 32 /dev/zero >"$T/baddek"
want "$(code $CTL --store "$T/e.store" --dek-file "$T/baddek" status)" 4 "wrong dek"
# a member with the same UUID but another data key is not a member
mk --encrypt --passphrase hunter2 --uuid $UUID1 "$T/e2.store"
want "$(code $CTL --store "$T/e.store" --store "$T/e2.store" --passphrase-file "$T/pass" get CONFIG1 STATE)" 0 "foreign key member"
has "$T/err" "excluded"
# the prompt, on a pty
if command -v python3 >/dev/null 2>&1; then
    timeout 60 python3 -I - "$CTL" "$T/e.store" <<'PY' || fail "prompt on a pty"
import os, pty, sys, time
ctl, store = sys.argv[1:3]
def run(pw):
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(ctl, [ctl, "--store", store, "get", "CONFIG1", "STATE"])
    out, seen = b"", 0
    while True:
        try:
            d = os.read(fd, 100)
        except OSError:
            break
        if not d:
            break
        out += d
        if out.count(b"Passphrase") > seen:   # one answer per prompt (three attempts)
            seen += 1
            time.sleep(0.1)   # let the child reach its read: it flushes pending input when it restores the terminal
            os.write(fd, pw)
    return os.waitpid(pid, 0)[1] >> 8, out
rc, out = run(b"hunter2\n")
assert rc == 0 and b"OK" in out, (rc, out)
rc, out = run(b"nope\n")
assert rc == 4, (rc, out)
PY
fi

# --- 5. writing commands: confirm, fail, set --------------------------------------------------------
mk --uuid $UUID1 "$T/w.store"; seed "$T/w.store"
W="$CTL --store $T/w.store"
sum0=$(sha256sum "$T/w.store")
want "$(code $W confirm CONFIG2)" 0 "confirm"
want "$($W get CONFIG2 STATE)" OK "confirm sets OK"
want "$(code $W fail CONFIG2)" 0 "fail"
want "$($W get CONFIG2 STATE)" FAILED "fail sets FAILED"
want "$($W get CONFIG2 CAUSE)" operator "fail records the cause"
$W status >"$T/out"; awk '$1=="CONFIG2" && $3=="FAILED" && $6=="operator"' "$T/out" | grep -q . || { cat "$T/out"; fail "status CAUSE column"; }
awk '$1=="CONFIG1" && $6=="-"' "$T/out" | grep -q . || { cat "$T/out"; fail "status CAUSE of a slot that is not FAILED"; }
awk '$1=="CONFIG1" && $7=="-"' "$T/out" | grep -q . || { cat "$T/out"; fail "status LABEL column"; }
want "$(code $W confirm CONFIG2)" 6 "FAILED is final for USER"
want "$($W get CONFIG2 STATE)" FAILED "refused confirm changes nothing"
want "$(code $W confirm CONFIG9)" 1 "confirm of a missing slot"
want "$($W get CONFIG1 PIN)" 0 "a slot is not pinned"
want "$(code $W set CONFIG1 PIN 1)" 0 "pin a slot"
want "$($W get CONFIG1 PIN)" 1 "pin is set"
$W status >"$T/out"; awk '$1=="CONFIG1" && $NF=="yes"' "$T/out" | grep -q . || { cat "$T/out"; fail "status PIN column"; }
awk '$1=="CONFIG2" && $NF=="-"' "$T/out" | grep -q . || { cat "$T/out"; fail "status PIN of a slot that is not pinned"; }
want "$(code $W set CONFIG1 PIN 2)" 6 "PIN takes 0 or 1"
[ "$(code $W remove CONFIG1)" != 0 ] || fail "remove of a pinned slot"
want "$($W get CONFIG1 STATE)" OK "a refused remove keeps the slot"
want "$(code $W set CONFIG1 PIN 0)" 0 "unpin"
want "$($W get CONFIG1 PIN)" 0 "pin is cleared"
want "$(code $W confirm bogus)" 2 "confirm of a bad name"
want "$(code $W confirm)" 1 "confirm without BOOTED"
$HFS "$T/w.store" booted CONFIG1 || fail "BOOTED fixture"
want "$(code $W fail)" 0 "fail defaults to BOOTED"
want "$($W get CONFIG1 STATE)" FAILED "BOOTED resolved to CONFIG1"
want "$($W get BOOTED STATE)" FAILED "get through BOOTED"
slot "$T/w.store" 3 INSTALLED
want "$(code $W set CONFIG3 STATE OK)" 0 "set STATE"
want "$($W get CONFIG3 STATE)" OK "set STATE value"
want "$(code $W set CONFIG1 WATCHDOG 30)" 6 "slot WATCHDOG is frozen after finalize"
want "$(code $W set CONFIG1 TRIES 3)" 6 "slot TRIES is frozen after finalize"
want "$(code $W set CONFIG1 LABEL test)" 6 "slot LABEL is frozen after finalize"
want "$(code $W set CONFIG1 ENTRY test.conf)" 6 "slot ENTRY is frozen after finalize"
want "$(code $W set CONFIG1 META test)" 6 "slot META is frozen after finalize"
want "$(code $W get CONFIG1 LABEL)" 1 "get unset LABEL exits 1"
want "$(code $W get CONFIG1 ENTRY)" 1 "get unset ENTRY exits 1"
want "$(code $W get CONFIG1 META)" 1 "get unset META exits 1"
$HFS "$T/w.store" mkdir EFI/RUNWAY/CONFIG4 && $HFS "$T/w.store" put EFI/RUNWAY/CONFIG4/KERNEL k4 || fail "CONFIG4 fixture"
want "$(code $W set CONFIG4 LABEL unfinalized)" 0 "set LABEL on unfinalized slot"
want "$($W get CONFIG4 LABEL)" unfinalized "get LABEL on unfinalized slot"
want "$(code $W set CONFIG4 ENTRY unfinalized.conf)" 0 "set ENTRY on unfinalized slot"
want "$($W get CONFIG4 ENTRY)" unfinalized.conf "get ENTRY on unfinalized slot"
printf "version 1.0.0\nmachine-id 0123456789abcdef0123456789abcdef\n" >"$T/meta.txt"
want "$(code $W set CONFIG4 META @"$T/meta.txt")" 0 "set META on unfinalized slot"
want "$($W get CONFIG4 META)" "$(cat "$T/meta.txt")" "get META on unfinalized slot"
$W status >"$T/out"; awk '$1=="CONFIG4" && $7=="unfinalized"' "$T/out" | grep -q . || fail "status LABEL of unfinalized slot"
awk '$1=="CONFIG4" && $8=="unfinalized.conf" && $9=="1.0.0"' "$T/out" | grep -q . || fail "status ENTRY and META columns"
want "$(code $W set CONFIG4 STATE INSTALLED)" 0 "finalize CONFIG4"
want "$(code $W set CONFIG4 LABEL rewrite)" 6 "slot LABEL is frozen after finalize"
want "$(code $W set CONFIG4 ENTRY rewrite.conf)" 6 "slot ENTRY is frozen after finalize"
want "$(code $W set CONFIG4 META rewrite)" 6 "slot META is frozen after finalize"
want "$(code $W set WATCHDOG 10)" 0 "set global WATCHDOG"
want "$($W get WATCHDOG)" 10 "global WATCHDOG value"
want "$(code $W get BOOT-DELAY)" 1 "BOOT-DELAY when unset"
want "$(code $W set BOOT-DELAY 5)" 0 "set BOOT-DELAY"
want "$($W get BOOT-DELAY)" 5 "BOOT-DELAY value"
$W status >"$T/out"; has "$T/out" "BOOT-DELAY=5"
want "$(code $W set BOOT-DELAY 31)" 6 "set BOOT-DELAY over 30"
want "$(code $W set BOOT-DELAY x)" 6 "set BOOT-DELAY text"
want "$($W get BOOT-DELAY)" 5 "a refused set keeps BOOT-DELAY"
want "$(code $W get LOOP-GUARD)" 1 "LOOP-GUARD when unset"
want "$(code $W set LOOP-GUARD on)" 0 "set LOOP-GUARD"
want "$($W get LOOP-GUARD)" on "LOOP-GUARD value"
want "$(code $W set LOOP-LIMIT 8)" 0 "set LOOP-LIMIT"
want "$(code $W set LOOP-WINDOW 900)" 0 "set LOOP-WINDOW"
$W status >"$T/out"; has "$T/out" "LOOP-GUARD=on"; has "$T/out" "LOOP-LIMIT=8"; has "$T/out" "LOOP-WINDOW=900"
want "$(code $W set LOOP-GUARD maybe)" 6 "set LOOP-GUARD text"
want "$(code $W set LOOP-LIMIT 1)" 6 "set LOOP-LIMIT under 2"
want "$(code $W set LOOP-WINDOW 29)" 6 "set LOOP-WINDOW under 30"
want "$($W get LOOP-LIMIT)" 8 "a refused set keeps LOOP-LIMIT"
want "$(code $W set CONFIG1 WATCHDOG abc)" 6 "set unparsable value"
want "$(code $W set CONFIG1 REVISION 99)" 6 "set read-only field"
want "$(code $W set CONFIG1 INDEX 99)" 6 "set INDEX"
want "$(code $W set CONFIG1 BOGUS 1)" 2 "set unknown field"
want "$(code $W set CONFIG1 WATCHDOG)" 2 "set without value"
want "$(code $W get CONFIG1 WATCHDOG)" 1 "refused set changes nothing"
# a refused or failed command leaves the file byte-identical
sum1=$(sha256sum "$T/w.store"); $W confirm CONFIG2 2>/dev/null; $W set CONFIG1 REVISION 1 2>/dev/null
want "$(sha256sum "$T/w.store")" "$sum1" "store changed by a refused command"
[ "$sum0" != "$sum1" ] || fail "no command wrote anything"
$CTL --store "$T/w.store" status >/dev/null 2>&1 || fail "store unreadable after writes"
$BUILD/fsck.runway "$T/w.store" >/dev/null 2>&1 || fail "fsck.runway after writes"
# RECOVERY is sealed
HOST_FS_SYSTEM=1 $HFS "$T/w.store" mkdir EFI/RUNWAY/RECOVERY && HOST_FS_SYSTEM=1 $HFS "$T/w.store" put EFI/RUNWAY/RECOVERY/KERNEL rk || fail "RECOVERY fixture"
want "$(code $W confirm RECOVERY)" 6 "RECOVERY is sealed"
# a second writer gets exit 5, a reader still works
cp "$T/w.store" "$T/w2.store"
if command -v flock >/dev/null 2>&1; then
    sh -c 'flock -x 9; exec sleep 30' 9<"$T/w.store" & holder=$!
    n=0; while flock -n "$T/w.store" true && [ $n -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
    want "$(code $W confirm CONFIG2)" 5 "store busy"
    has "$T/err" "busy"
    want "$(code $W status)" 0 "status while the store is locked"
    want "$(code $CTL --store "$T/w2.store" --store "$T/w.store" confirm CONFIG2)" 5 "one busy member blocks a mirror write"
    kill $holder 2>/dev/null; wait $holder 2>/dev/null
fi
# mirror: the newest copy is written, the lagging one is not touched
slot "$T/w.store" 7 INSTALLED
sumlag=$(sha256sum "$T/w2.store")
want "$(code $CTL --store "$T/w2.store" --store "$T/w.store" confirm CONFIG7)" 0 "mirror write"
has "$T/err" "lagging"
want "$($CTL --store "$T/w.store" get CONFIG7 STATE)" OK "newest copy written"
want "$(sha256sum "$T/w2.store")" "$sumlag" "lagging copy untouched"
# mirror: members that share a member UUID (a plain copy) are named by a write, not by a read
mu() { $BUILD/fsck.runway "$1" | sed -n 's/^slot [AB]: member uuid //p' | sort -u; }
mk --uuid $UUID1 "$T/u1.store"; seed "$T/u1.store"; cp "$T/u1.store" "$T/u2.store"
want "$(mu "$T/u1.store")" "$(mu "$T/u2.store")" "a copy starts with the same member UUID"
sumu=$(sha256sum "$T/u1.store" "$T/u2.store")
want "$(code $CTL --store "$T/u1.store" --store "$T/u2.store" status)" 0 "status of a copied pair"
want "$(sha256sum "$T/u1.store" "$T/u2.store")" "$sumu" "a read names nothing"
want "$(code $CTL --store "$T/u1.store" --store "$T/u2.store" confirm CONFIG2)" 0 "write to a copied pair"
[ "$(mu "$T/u1.store")" != "$(mu "$T/u2.store")" ] || fail "a write gives each copy its own member UUID"
mua=$(mu "$T/u1.store"); mub=$(mu "$T/u2.store")
echo kimg >"$T/ku"
want "$(code $CTL --store "$T/u1.store" --store "$T/u2.store" install --kernel "$T/ku")" 0 "second write"
want "$(mu "$T/u1.store") $(mu "$T/u2.store")" "$mua $mub" "a later write keeps both member UUIDs"
want "$(code $BUILD/fsck.runway "$T/u1.store")" 0 "named member is clean"

# mirror: same generation, but one member holds a newer state record (FAILED): its record decides
mk --uuid $UUID1 "$T/r1.store"; seed "$T/r1.store"; cp "$T/r1.store" "$T/r2.store"
$CTL --store "$T/r2.store" fail CONFIG2 >/dev/null 2>&1 || fail "fail on r2"
$HFS "$T/r1.store" mkdir EFI/SPARE || fail "spare dir"
g1=$($BUILD/fsck.runway "$T/r1.store" | sed -n 's/^committed: slot . gen \([0-9]*\).*/\1/p')
g2=$($BUILD/fsck.runway "$T/r2.store" | sed -n 's/^committed: slot . gen \([0-9]*\).*/\1/p')
want "$g1" "$g2" "members of the same generation"
want "$($CTL --store "$T/r1.store" get CONFIG2 STATE)" INSTALLED "the stale member alone"
sum1=$(sha256sum "$T/r1.store"); sum2=$(sha256sum "$T/r2.store")
want "$($CTL --store "$T/r1.store" --store "$T/r2.store" get CONFIG2 STATE)" FAILED "read: the newer record wins"
want "$($CTL --store "$T/r2.store" --store "$T/r1.store" get CONFIG2 STATE)" FAILED "read: order independent"
want "$(sha256sum "$T/r1.store")$(sha256sum "$T/r2.store")" "$sum1$sum2" "a read changes no member"
want "$(code $CTL --store "$T/r1.store" --store "$T/r2.store" fail CONFIG1)" 0 "write on the pair"
want "$($CTL --store "$T/r1.store" get CONFIG2 STATE)" FAILED "the stale member got the record"
want "$($CTL --store "$T/r1.store" get CONFIG1 STATE)" FAILED "and the new change"
# a member that fails during the commit: warning, exit 0 while the primary is written
FIO=$BUILD/faultio.so
slot "$T/w.store" 8 INSTALLED
cp "$T/w.store" "$T/f1.store"; cp "$T/w.store" "$T/f2.store"
want "$(code env LD_PRELOAD=$FIO FAULTIO_FAIL_PWRITE=f2.store $CTL --store "$T/f1.store" --store "$T/f2.store" confirm CONFIG8)" 0 "member fails during the commit"
has "$T/err" "failed during the commit"
want "$($CTL --store "$T/f1.store" get CONFIG8 STATE)" OK "primary written"
# every member fails: nothing was published
cp "$T/w.store" "$T/f1.store"; cp "$T/w.store" "$T/f2.store"
want "$(code env LD_PRELOAD=$FIO FAULTIO_FAIL_PWRITE=.store $CTL --store "$T/f1.store" --store "$T/f2.store" confirm CONFIG8)" 1 "all members fail"
want "$($CTL --store "$T/f1.store" get CONFIG8 STATE)" INSTALLED "no generation published"
# a read-only temporary mount is remounted sync, then opened read-write
cp "$T/w.store" "$T/r.store"
want "$(code env LD_PRELOAD=$FIO FAULTIO_EROFS="$T/r.store" FAULTIO_LOG="$T/mount.log" $CTL --store "$T/r.store" confirm CONFIG8)" 0 "read-only mount remounted"
want "$(cat "$T/mount.log")" "$T sync" "remount of the directory with sync"
want "$($CTL --store "$T/r.store" get CONFIG8 STATE)" OK "written after the remount"
# encrypted store
mk --encrypt --passphrase hunter2 --uuid $UUID1 "$T/we.store"
HOST_FS_PASSPHRASE=hunter2 seed "$T/we.store"
want "$(code $CTL --store "$T/we.store" confirm CONFIG2)" 4 "write to a locked store"
want "$(code $CTL --store "$T/we.store" --passphrase-file "$T/pass" confirm CONFIG2)" 0 "write to an encrypted store"
want "$($CTL --store "$T/we.store" --passphrase-file "$T/pass" get CONFIG2 STATE)" OK "encrypted write kept"

# --- 5b. RunwayBoot: confirm refuses the slot the store names when another one started -----------------
mk --uuid $UUID1 "$T/v.store"; seed "$T/v.store"
$HFS "$T/v.store" booted CONFIG2 || fail "BOOTED fixture"
V="$T/sys/firmware/efi/efivars/RunwayBoot-b371a076-b7ef-4e6e-b98a-8d0d2c493c7b"
mkdir -p "$T/sys/firmware/efi/efivars"
setvar() { printf '\007\000\000\000%s' "$1" >"$V"; }   # 4 bytes of attributes, then the value
CV="env RWYCTL_ROOT=$T $CTL --store $T/v.store"
want "$(code $CV confirm)" 0 "confirm without the variable"
HOST_FS_SYSTEM=1 $HFS "$T/v.store" put EFI/RUNWAY/CONFIG2/STATE "TESTING
" || fail "fixture"
setvar "CONFIG2 recorded"
want "$(code $CV confirm)" 0 "confirm when the variable agrees"
HOST_FS_SYSTEM=1 $HFS "$T/v.store" put EFI/RUNWAY/CONFIG2/STATE "TESTING
" || fail "fixture"
setvar "CONFIG1 unrecorded"
want "$(code $CV confirm)" 6 "confirm refused when another slot started"
has "$T/err" "CONFIG1"
want "$($CV get CONFIG2 STATE)" TESTING "a refused confirm changes nothing"
want "$(code $CV confirm CONFIG2)" 6 "same by name"
want "$(code $CV fail CONFIG2)" 0 "fail is never refused"
setvar "garbage"
want "$(code $CV confirm CONFIG1)" 0 "confirm of a slot BOOTED does not name"

# --- 5c. the health point clears the boot loop counter ----------------------------------------------
G="$T/sys/firmware/efi/efivars/RunwayBootGuard-b371a076-b7ef-4e6e-b98a-8d0d2c493c7b"
guard() { printf '\007\000\000\000counter' >"$G"; }
HOST_FS_SYSTEM=1 $HFS "$T/v.store" put EFI/RUNWAY/CONFIG2/STATE "OK
" || fail "fixture"
setvar "CONFIG2 recorded"
guard
want "$(code $CV confirm CONFIG2)" 0 "confirm OK over OK"
[ -e "$G" ] && fail "confirm of the slot that started kept the counter"
guard
HOST_FS_SYSTEM=1 $HFS "$T/v.store" put EFI/RUNWAY/CONFIG2/STATE "TESTING
" || fail "fixture"
want "$(code $CV confirm)" 0 "confirm TESTING by BOOTED"
[ -e "$G" ] && fail "confirm through BOOTED kept the counter"
guard
want "$(code $CV confirm CONFIG1)" 0 "confirm of another slot"
[ -e "$G" ] || fail "confirm of another slot cleared the counter"
setvar "CONFIG1 unrecorded"
HOST_FS_SYSTEM=1 $HFS "$T/v.store" put EFI/RUNWAY/CONFIG2/STATE "TESTING
" || fail "fixture"
want "$(code $CV confirm CONFIG2)" 6 "refused confirm"
[ -e "$G" ] || fail "refused confirm cleared the counter"
rm -f "$V"
want "$(code $CV set CONFIG2 STATE OK)" 0 "set STATE OK"
[ -e "$G" ] && fail "set STATE OK kept the counter"
want "$(code $CV confirm CONFIG2)" 0 "confirm with no counter"
guard
chmod 555 "$T/sys/firmware/efi/efivars"
if [ "$(id -u)" != 0 ]; then
    want "$(code $CV confirm CONFIG2)" 1 "a clear that fails is reported"
    has "$T/err" "boot loop counter"
    want "$($CV get CONFIG2 STATE)" OK "the confirm stays"
fi
chmod 755 "$T/sys/firmware/efi/efivars"
rm -f "$G"

# --- 6. remove, recover-next ----------------------------------------------------------------------
mk --uuid $UUID1 "$T/r.store"; seed "$T/r.store"
R1="$CTL --store $T/r.store"
$HFS "$T/r.store" mkdir EFI/RUNWAY/BLOBS && $HFS "$T/r.store" put EFI/RUNWAY/BLOBS/k1 blobkernel || fail "BLOBS fixture"
$HFS "$T/r.store" mkdir EFI/RUNWAY/CONFIG5 && $HFS "$T/r.store" link EFI/RUNWAY/BLOBS/k1 EFI/RUNWAY/CONFIG5/KERNEL || fail "hardlink fixture"
$HFS "$T/r.store" mkdir EFI/RUNWAY/CONFIG6 && $HFS "$T/r.store" put EFI/RUNWAY/CONFIG6/KERNEL partial || fail "incomplete fixture"
want "$(code $R1 remove CONFIG2)" 0 "remove a finalized slot"
$R1 status >"$T/out"; grep -q '^CONFIG2 ' "$T/out" && fail "CONFIG2 still listed"
want "$(code $R1 remove CONFIG5)" 0 "remove a slot with a hardlinked payload"
want "$($HFS "$T/r.store" cat EFI/RUNWAY/BLOBS/k1)" blobkernel "BLOBS file survives"
want "$(code $R1 remove CONFIG6)" 0 "remove an incomplete slot"
$R1 status >"$T/out"; has "$T/out" "INCOMPLETE=0"
want "$(code $R1 remove CONFIG2)" 1 "remove a missing slot"
want "$(code $R1 remove bogus)" 2 "remove a bad name"
want "$(code $R1 remove)" 2 "remove without a name"
HOST_FS_SYSTEM=1 $HFS "$T/r.store" mkdir EFI/RUNWAY/RECOVERY && HOST_FS_SYSTEM=1 $HFS "$T/r.store" put EFI/RUNWAY/RECOVERY/KERNEL rk || fail "RECOVERY fixture"
sum1=$(sha256sum "$T/r.store")
want "$(code $R1 remove RECOVERY)" 6 "RECOVERY cannot be removed"
want "$(sha256sum "$T/r.store")" "$sum1" "refused remove changed the store"
$BUILD/fsck.runway "$T/r.store" >/dev/null 2>&1 || fail "fsck.runway after remove"
want "$(code $R1 recover-next)" 0 "recover-next"
$R1 status >"$T/out"; has "$T/out" "RECOVER_NEXT=yes"
want "$(code $R1 recover-next)" 0 "recover-next twice"
want "$(code $R1 recover-next --cancel)" 0 "recover-next --cancel"
$R1 status >"$T/out"; has "$T/out" "RECOVER_NEXT=no"
want "$(code $R1 recover-next --cancel)" 0 "cancel without a request"
want "$(code $R1 recover-next --bogus)" 2 "recover-next bad option"
$BUILD/fsck.runway "$T/r.store" >/dev/null 2>&1 || fail "fsck.runway after recover-next"
mk --uuid $UUID1 "$T/r2.store"; seed "$T/r2.store"
want "$(code $CTL --store "$T/r2.store" recover-next)" 0 "recover-next without RECOVERY"
has "$T/err" "RECOVERY is absent"

# --- 7. install ------------------------------------------------------------------------------------
mk --uuid $UUID1 "$T/i.store"; seed "$T/i.store"
I="$CTL --store $T/i.store"
echo kernel-image >"$T/k.efi"; echo initrd-image >"$T/i.img"; echo dtb-image >"$T/d.dtb"; echo "console=ttyS0 quiet" >"$T/args.txt"
want "$(code $I install --kernel "$T/k.efi" --initrd "$T/i.img" --dtb "$T/d.dtb" --args @"$T/args.txt" --tries 3 --watchdog 20)" 0 "install"
want "$(awk '{print $1}' "$T/out")" CONFIG3 "install prints the name"
rev=$(awk '{print $2}' "$T/out"); want "$($I get CONFIG3 REVISION)" "$rev" "install prints the revision"
want "$rev" "$($I get REVISION)" "the counter holds the new revision"
$I status >"$T/out"; awk '$1=="CONFIG3" && $3=="INSTALLED" && $4==3 && $5==20' "$T/out" | grep -q . || fail "installed slot row"
want "$($I get CONFIG3 ARGS)" "console=ttyS0 quiet" "ARGS from a file, no newline"
want "$($HFS "$T/i.store" cat EFI/RUNWAY/CONFIG3/KERNEL)" kernel-image "KERNEL copied"
want "$($HFS "$T/i.store" cat EFI/RUNWAY/CONFIG3/INITRD)" initrd-image "INITRD copied"
want "$($HFS "$T/i.store" cat EFI/RUNWAY/CONFIG3/DTB)" dtb-image "DTB copied"
want "$($I get CONFIG3 ENTRY)" "runway-3.conf" "default Type 1 ENTRY"
want "$($I install --kernel "$T/k.efi" | awk '{print $1}')" CONFIG4 "next free name"
want "$($I get CONFIG4 ENTRY)" "runway-4.efi" "default Type 2 ENTRY"
want "$(code $I install --name CONFIG9 --kernel "$T/k.efi" --args "root=/dev/x")" 0 "install --name"
want "$($I get CONFIG9 ARGS)" "root=/dev/x" "ARGS from the command line"
want "$($I install --kernel "$T/k.efi" | awk '{print $1}')" CONFIG10 "name follows the highest"
echo "label text" >"$T/label.txt"
want "$(code $I install --kernel "$T/k.efi" --label @"$T/label.txt")" 0 "install with --label @FILE"
want "$($I get CONFIG11 LABEL)" "label text" "LABEL from a file, no newline"
$I status >"$T/out"; awk '$1=="CONFIG11" && $7=="label" && $8=="text"' "$T/out" | grep -q . || fail "status row with label text"
want "$(code $I install --kernel "$T/k.efi" --label "direct-label")" 0 "install with --label TEXT"
want "$($I get CONFIG12 LABEL)" "direct-label" "LABEL from command line"
$I status >"$T/out"; awk '$1=="CONFIG12" && $7=="direct-label"' "$T/out" | grep -q . || fail "status LABEL column"
want "$($I slots --label direct-label)" CONFIG12 "slots --label finds one slot"
want "$($I slots --label direct)" "" "slots --label is exact"
want "$(code $I slots --label nothing-like-it)" 0 "slots --label without a match"
want "$($I slots | head -3 | tr '\n' ' ')" "CONFIG1 CONFIG2 CONFIG3 " "slots lists names in order"
want "$($I slots | wc -l)" "$($I status | awk '$1 ~ /^CONFIG[0-9]+$/' | wc -l)" "slots lists every slot"
want "$(code $I slots --label)" 2 "slots --label needs a value"
want "$(code $I slots --invalid)" 2 "slots with invalid option exits 2"

# status --json and slots --json
want "$(code $I status --json)" 0 "status --json exit code"
json_ok "$T/out" "status --json produces valid JSON"
has "$T/out" '"slots": ['
has "$T/out" '"CONFIG12"'
has "$T/out" '"direct-label"'
has "$T/out" '"counter":'
has "$T/out" '"used": {'
has "$T/out" '"recovery":'
has "$T/out" '"incomplete": 0'
has "$T/out" '"used": {"percent"'
want "$(code $I status --invalid)" 2 "status with invalid option exits 2"

want "$(code $I slots --json)" 0 "slots --json exit code"
json_ok "$T/out" "slots --json produces valid JSON"
want "$($I slots --json --label direct-label | tr -d '\n ')" '["CONFIG12"]' "slots --json --label finds one slot"
want "$($I slots --label direct-label --json | tr -d '\n ')" '["CONFIG12"]' "slots --label --json ordering"
want "$($I slots --json --label nothing-like-it)" '[]' "slots --json without a match"
want "$(code $I install --kernel "$T/k.efi" --label 'bad"label')" 6 "install with bad quote in label"
want "$(code $I install --kernel "$T/k.efi" --entry "tok-6.12.efi")" 0 "install with --entry Type 2"
want "$($I get CONFIG13 ENTRY)" "tok-6.12.efi" "explicit Type 2 ENTRY"
want "$(code $I install --kernel "$T/k.efi" --entry "tok-6.12.conf" --args "quiet")" 0 "install with --entry Type 1"
want "$($I get CONFIG14 ENTRY)" "tok-6.12.conf" "explicit Type 1 ENTRY"
echo "entry-from-file.conf" >"$T/entry.txt"
want "$(code $I install --kernel "$T/k.efi" --entry @"$T/entry.txt")" 0 "install with --entry @FILE"
want "$($I get CONFIG15 ENTRY)" "entry-from-file.conf" "ENTRY from file"
want "$(code $I install --kernel "$T/k.efi" --entry "bad+1.conf")" 6 "install with invalid counter in entry"
want "$(code $I install --kernel "$T/k.efi" --entry "bad.efi" --args "quiet")" 6 "Type 2 entry refuses ARGS"
want "$(code $I install --kernel "$T/k.efi" --args "quiet" --meta "version 6.12.107")" 0 "install with --meta TEXT"
want "$($I get CONFIG16 META)" "version 6.12.107" "META from text"
printf "version 1.0.0\nmachine-id 0123456789abcdef0123456789abcdef\n" >"$T/meta2.txt"
want "$(code $I install --kernel "$T/k.efi" --args "quiet" --meta @"$T/meta2.txt")" 0 "install with --meta @FILE"
want "$($I get CONFIG17 META)" "$(cat "$T/meta2.txt")" "META from file"
$I status >"$T/out"; awk '$1=="CONFIG17" && $9=="1.0.0"' "$T/out" | grep -q . || fail "status row with META version"
want "$(code $I install --kernel "$T/k.efi" --meta "badkey 1")" 6 "install with invalid META key"
want "$(code $I install --kernel "$T/k.efi" --entry "foo.efi" --meta "version 1")" 6 "Type 2 entry refuses META"
$BUILD/fsck.runway "$T/i.store" >/dev/null 2>&1 || fail "fsck.runway after install"
# big payload spanning many chunks
head -c 3500000 /dev/urandom >"$T/big.efi"
want "$(code $I install --kernel "$T/big.efi")" 0 "install of a multi-chunk payload"
$BUILD/fsck.runway "$T/i.store" >/dev/null 2>&1 || fail "fsck.runway after a multi-chunk install"
# with --dedup, payloads are hard links to content blobs; the same content is stored once
data_blocks() { $BUILD/fsck.runway "$T/i.store" 2>&1 | sed -n 's/^blocks:.* data=\([0-9]*\).*/\1/p'; }
printf blobkernel >"$T/bk"; bh=$(sha256sum "$T/bk" | cut -d' ' -f1)
want "$(code $I install --dedup --kernel "$T/bk" --name CONFIG20)" 0 "install of a file"
want "$($HFS "$T/i.store" cat EFI/RUNWAY/BLOBS/$bh)" blobkernel "the blob holds the content"
want "$($HFS "$T/i.store" cat EFI/RUNWAY/CONFIG20/KERNEL)" blobkernel "slot payload content"
head -c 3500000 /dev/urandom >"$T/same.efi"; sh=$(sha256sum "$T/same.efi" | cut -d' ' -f1)
d0=$(data_blocks)
want "$(code $I install --dedup --kernel "$T/same.efi" --name CONFIG21)" 0 "install of new content"
d1=$(data_blocks)
[ $((d1 - d0)) -ge 850 ] || fail "new content took $((d1 - d0)) blocks"
want "$(code $I install --dedup --kernel "$T/same.efi" --name CONFIG22)" 0 "install of the same content again"
d2=$(data_blocks)
[ $((d2 - d1)) -lt 20 ] || fail "same content twice took $((d2 - d1)) more blocks"
want "$(code $I install --dedup --kernel blob:$sh --initrd "$T/same.efi" --name CONFIG23)" 0 "install from a blob"
want "$(code $I install --dedup --kernel blob:$sh --initrd "$T/bk" --name CONFIG24)" 0 "install from a blob and a file"
want "$($HFS "$T/i.store" cat EFI/RUNWAY/CONFIG24/INITRD)" blobkernel "file next to a blob"
# files carry real times, not the generation number: mkstore (SOURCE_DATE_EPOCH or the clock) and rwyctl
SOURCE_DATE_EPOCH=4102444800 $BUILD/mkstore --size 64 "$T/epoch.store" >/dev/null || fail "mkstore with SOURCE_DATE_EPOCH"
want "$($HFS "$T/epoch.store" times EFI | cut -d' ' -f1)" 4102444800 "mkstore honours SOURCE_DATE_EPOCH"
[ "$($HFS "$T/i.store" times EFI/RUNWAY/CONFIG3/ARGS | cut -d' ' -f1)" -gt 1700000000 ] || fail "ARGS written by rwyctl has no real time"
# SOURCE_DATE_EPOCH makes the times of a build reproducible
printf rep >"$T/rep.efi"; touch -d @4102444800 "$T/rep.efi"
SDE=1700000000; export SOURCE_DATE_EPOCH=$SDE
want "$(code $I install --kernel "$T/rep.efi" --args x --name CONFIG30)" 0 "install with SOURCE_DATE_EPOCH"
want "$($HFS "$T/i.store" times EFI/RUNWAY/CONFIG30/ARGS)" "$SDE $SDE" "ARGS times are the epoch"
want "$($HFS "$T/i.store" times EFI/RUNWAY/CONFIG30/KERNEL | cut -d' ' -f1)" "$SDE" "payload mtime is capped by the epoch"
unset SOURCE_DATE_EPOCH
# a payload keeps the mtime of its file, in 64 bits (a year after 2038); an own copy keeps the ctime too
printf timed >"$T/timed.efi"; touch -d @4102444800 "$T/timed.efi"; th=$(sha256sum "$T/timed.efi" | cut -d' ' -f1)
want "$(code $I install --dedup --kernel "$T/timed.efi" --name CONFIG25)" 0 "install of a file with a late mtime"
want "$($HFS "$T/i.store" times EFI/RUNWAY/CONFIG25/KERNEL | cut -d' ' -f1)" 4102444800 "payload mtime"
want "$($HFS "$T/i.store" times EFI/RUNWAY/BLOBS/$th | cut -d' ' -f1)" 4102444800 "blob mtime"
want "$(code $I remove CONFIG21)" 0 "remove one of the slots that share a blob"
$HFS "$T/i.store" cat EFI/RUNWAY/BLOBS/$sh >/dev/null || fail "the shared blob went with one slot"
for n in 22 23 24; do want "$(code $I remove CONFIG$n)" 0 "remove CONFIG$n"; done
want "$(code $HFS "$T/i.store" cat EFI/RUNWAY/BLOBS/$sh)" 1 "the blob went with its last slot"
want "$(code $I install --kernel blob:uki-1)" 2 "blob: takes a content name only"
# without --dedup, install gives each slot independent physical data blocks (fault isolation)
head -c 3500000 /dev/urandom >"$T/iso.efi"
d_before=$(data_blocks)
want "$(code $I install --kernel "$T/iso.efi" --name CONFIG40)" 0 "install without dedup"
d_mid=$(data_blocks)
[ $((d_mid - d_before)) -ge 850 ] || fail "slot 40 took $((d_mid - d_before)) blocks"
want "$(code $I install --kernel "$T/iso.efi" --name CONFIG41)" 0 "install identical file without dedup"
d_after=$(data_blocks)
[ $((d_after - d_mid)) -ge 850 ] || fail "slot 41 took $((d_after - d_mid)) blocks (expected full copy)"
iso_h=$(sha256sum "$T/iso.efi" | cut -d' ' -f1)
want "$(code $HFS "$T/i.store" cat EFI/RUNWAY/BLOBS/$iso_h)" 1 "no blob created without --dedup"
want "$($HFS "$T/i.store" cat EFI/RUNWAY/CONFIG40/KERNEL | sha256sum | cut -d' ' -f1)" "$iso_h" "slot 40 payload matches"
want "$($HFS "$T/i.store" cat EFI/RUNWAY/CONFIG41/KERNEL | sha256sum | cut -d' ' -f1)" "$iso_h" "slot 41 payload matches"
want "$(code $I remove CONFIG40)" 0 "remove CONFIG40"
want "$(code $I remove CONFIG41)" 0 "remove CONFIG41"
# install from a blob without --dedup creates an independent copy
printf blobcopy >"$T/bc"; bch=$(sha256sum "$T/bc" | cut -d' ' -f1)
want "$(code $I install --dedup --kernel "$T/bc" --name CONFIG42)" 0 "seed blob via install --dedup"
want "$($HFS "$T/i.store" cat EFI/RUNWAY/BLOBS/$bch)" blobcopy "seeded blob exists"
want "$(code $I install --kernel blob:$bch --name CONFIG43)" 0 "install from blob without dedup"
want "$($HFS "$T/i.store" cat EFI/RUNWAY/CONFIG43/KERNEL)" blobcopy "payload copied from blob"
want "$(code $I remove CONFIG42)" 0 "remove slot that created the blob"
want "$(code $HFS "$T/i.store" cat EFI/RUNWAY/BLOBS/$bch)" 1 "blob removed because CONFIG43 holds an own copy"
want "$($HFS "$T/i.store" cat EFI/RUNWAY/CONFIG43/KERNEL)" blobcopy "CONFIG43 payload remains intact after blob removed"
want "$(code $I remove CONFIG43)" 0 "remove CONFIG43"
$BUILD/fsck.runway "$T/i.store" >/dev/null 2>&1 || fail "fsck.runway after content blobs"
# failures leave no slot and nothing published
snap() { { $I status; $CTL --store "$T/i.store" stores; } 2>&1; }
before=$(snap)
want "$(code $I install --kernel "$T/none.efi")" 1 "install of a missing file"
want "$(code $I install --kernel blob:$(printf nope | sha256sum | cut -d" " -f1))" 1 "install of a missing blob"
want "$(code $I install --kernel blob:nope)" 2 "install of a blob that is not a content name"
want "$(code $I install --kernel blob:../x)" 2 "install of a bad blob name"
want "$(code $I install --name CONFIG1 --kernel "$T/k.efi")" 1 "install over an existing name"
want "$(code $I install --name CONFIG01 --kernel "$T/k.efi")" 6 "install with a non canonical name"
want "$(code $I install --name bogus --kernel "$T/k.efi")" 2 "install with a bad name"
want "$(code $I install --kernel "$T/k.efi" --tries abc)" 6 "install with an unparsable TRIES"
want "$(code $I install)" 2 "install without a kernel"
want "$(code $I install --kernel)" 2 "install option without a value"
want "$(code $I install --bogus x --kernel "$T/k.efi")" 2 "install with an unknown option"
want "$(snap)" "$before" "failed installs changed the store"
# a payload larger than the free space: no slot, no new generation
truncate -s 100M "$T/huge.efi"
want "$(code $I install --kernel "$T/huge.efi")" 7 "install without space"
want "$(snap)" "$before" "install without space changed the store"
$BUILD/fsck.runway "$T/i.store" >/dev/null 2>&1 || fail "fsck.runway after failed installs"
# the slot limit
mk --uuid $UUID1 "$T/l.store"; $HFS "$T/l.store" mkdir EFI/RUNWAY || fail seed
n=0; while [ $n -lt 64 ]; do $CTL --store "$T/l.store" install --kernel "$T/k.efi" >/dev/null 2>&1 || fail "install $n of 64"; n=$((n+1)); done
want "$(code $CTL --store "$T/l.store" install --kernel "$T/k.efi")" 7 "the 65th slot"
$CTL --store "$T/l.store" status >"$T/out"; want "$(grep -c '^CONFIG' "$T/out")" 64 "no 65th slot"
# encrypted store
mk --encrypt --passphrase hunter2 --uuid $UUID1 "$T/ie.store"; HOST_FS_PASSPHRASE=hunter2 seed "$T/ie.store"
want "$(code $CTL --store "$T/ie.store" install --kernel "$T/k.efi")" 4 "install on a locked store"
want "$(code $CTL --store "$T/ie.store" --passphrase-file "$T/pass" install --kernel "$T/k.efi")" 0 "install on an encrypted store"
# mirror: the newest copy gets the slot
cp "$T/i.store" "$T/i2.store"; $I install --kernel "$T/k.efi" >/dev/null
sumlag=$(sha256sum "$T/i2.store")
want "$(code $CTL --store "$T/i2.store" --store "$T/i.store" install --kernel "$T/k.efi")" 0 "install on a mirror"
want "$(sha256sum "$T/i2.store")" "$sumlag" "lagging copy untouched by install"

# --- 8. check, recovery set, the SYSTEM actor -------------------------------------------------------
mk --uuid $UUID1 "$T/c.store"; seed "$T/c.store"
C="$CTL --store $T/c.store"
want "$(code $C check)" 0 "check on a clean store"
has "$T/out" "check: OK"
sumc=$(sha256sum "$T/c.store"); $C check >/dev/null 2>&1; want "$(sha256sum "$T/c.store")" "$sumc" "check wrote"
want "$(code $CTL --store "$T/a.store" check)" 0 "check with an incomplete slot"
has "$T/out" "1 incomplete"
mk --uuid $UUID1 "$T/n.store"
want "$(code $CTL --store "$T/n.store" check)" 0 "check without EFI/RUNWAY"
has "$T/err" "EFI/RUNWAY is absent"
# a counter behind the slots: check reports it, repair needs the explicit unlock, then fixes it
HOST_FS_PRIV=1 HOST_FS_SYSTEM=1 $HFS "$T/c.store" put EFI/RUNWAY/REVISION "1
" || fail "counter fixture"
want "$(code $C check)" 8 "check with a bad counter"
has "$T/out" "CORRUPT"
sumc=$(sha256sum "$T/c.store")
want "$(code $C check --repair)" 2 "repair without --unlock"
want "$(code $C check --unlock)" 2 "--unlock without --repair"
want "$(sha256sum "$T/c.store")" "$sumc" "refused repair wrote"
want "$(code $C check --repair --unlock)" 0 "repair"
has "$T/out" "REVISION fixed"
want "$(code $C check)" 0 "check after repair"
want "$($C get REVISION)" 2 "counter after repair"
$BUILD/fsck.runway "$T/c.store" >/dev/null 2>&1 || fail "fsck.runway after repair"
# a damaged state record: status says so, check reports it, repair writes an empty one
python3 -I - "$T/c.store" <<'PY' || fail "damage fixture"
import struct, sys
f = open(sys.argv[1], "r+b")
f.seek(140)
rec = struct.unpack("<I", f.read(4))[0]
for blk in (rec, rec + 1):
    f.seek(blk * 4096 + 100); b = f.read(1); f.seek(blk * 4096 + 100); f.write(bytes([b[0] ^ 0xff]))
PY
$C status >"$T/out"; has "$T/out" "RECORD=damaged"
want "$(code $C check)" 8 "check with a damaged record"
has "$T/out" "state record damaged"
sumc=$(sha256sum "$T/c.store")
want "$(code $C check --repair)" 2 "repair of the record without --unlock"
want "$(sha256sum "$T/c.store")" "$sumc" "refused repair wrote"
want "$(code $C check --repair --unlock)" 0 "repair of the record"
has "$T/out" "state record written"
want "$(code $C check)" 0 "check after the record repair"
$C status >"$T/out"; has "$T/out" "RECORD=ok"
$BUILD/fsck.runway "$T/c.store" >/dev/null 2>&1 || fail "fsck.runway after the record repair"
# a removed slot leaves its record entry: a warning, and repair drops it
mk --uuid $UUID1 "$T/s.store"; seed "$T/s.store"
S="$CTL --store $T/s.store"
want "$(code $S remove CONFIG2)" 0 "remove a slot"
want "$(code $S check)" 0 "check with a stale record entry"
has "$T/out" "entries of removed slots"
want "$(code $S check --repair --unlock)" 0 "repair of the stale entry"
has "$T/out" "state record written"
want "$(code $S check)" 0 "check after dropping it"
! grep -q "removed slots" "$T/out" || fail "stale warning after repair"
# a FAILED slot is listed with its cause
mk --uuid $UUID1 "$T/f.store"; seed "$T/f.store"
F="$CTL --store $T/f.store"
want "$(code $F fail CONFIG2)" 0 "fail a slot"
want "$(code $F check)" 0 "check with a FAILED slot"
has "$T/out" "info: CONFIG2 FAILED (operator)"
$BUILD/fsck.runway "$T/f.store" >"$T/out" 2>&1 || fail "fsck.runway with a FAILED slot"
has "$T/out" "info: EFI/RUNWAY: CONFIG2 FAILED (operator)"
# content blobs: --blobs hashes them, --repair marks the slots that link a bad one, never touching the blob
mk --uuid $UUID1 "$T/z.store"; seed "$T/z.store"
Z="$CTL --store $T/z.store"
printf goodkernel >"$T/zg"; printf wrongdata >"$T/zw"
fake=$(printf nope | sha256sum | cut -d' ' -f1); real=$(sha256sum "$T/zw" | cut -d' ' -f1)
want "$(code $Z install --dedup --kernel "$T/zg" --name CONFIG3)" 0 "install a good blob"
SYS="env HOST_FS_SYSTEM=1 $HFS $T/z.store"
$SYS put EFI/RUNWAY/BLOBS/tmp wrongdata && $SYS link EFI/RUNWAY/BLOBS/tmp EFI/RUNWAY/BLOBS/$fake &&
    $HFS "$T/z.store" mkdir EFI/RUNWAY/CONFIG4 && $SYS link EFI/RUNWAY/BLOBS/$fake EFI/RUNWAY/CONFIG4/KERNEL && $SYS rm EFI/RUNWAY/BLOBS/tmp &&
    $HFS "$T/z.store" put EFI/RUNWAY/CONFIG4/STATE "INSTALLED
" || fail "bad blob fixture"
want "$(code $Z check)" 0 "check without --blobs does not hash"
want "$(code $Z check --blobs --repair)" 2 "--blobs --repair without --unlock"
sumz=$(sha256sum "$T/z.store")
want "$(code $Z check --blobs)" 8 "check --blobs finds the bad blob"
has "$T/out" "ERROR: BLOBS/$fake: content is $real"
grep -q "BLOBS/$(sha256sum "$T/zg" | cut -d' ' -f1)" "$T/out" && fail "the good blob was reported"
want "$(sha256sum "$T/z.store")" "$sumz" "check --blobs wrote"
want "$(code $Z check --blobs --repair --unlock)" 8 "repair marks the slot and the blob stays bad"
has "$T/out" "repair: CONFIG4 FAILED (bad_blob), it links BLOBS/$fake"
want "$($Z get CONFIG4 STATE)" FAILED "CONFIG4 FAILED"
want "$($Z get CONFIG4 CAUSE)" bad_blob "CONFIG4 cause"
want "$($Z get CONFIG3 STATE)" INSTALLED "the slot of the good blob is left"
want "$($HFS "$T/z.store" cat EFI/RUNWAY/BLOBS/$fake)" wrongdata "the blob is not rewritten"
want "$(code $Z log verify)" 0 "the journal chain holds"
want "$(code $Z check)" 0 "check lists the FAILED slot"
has "$T/out" "info: CONFIG4 FAILED (bad_blob)"
want "$(code $Z remove CONFIG4)" 0 "remove the slot of the bad blob"
want "$(code $Z check --blobs)" 0 "check --blobs after the removal"
has "$T/out" "check: OK"
# staging files: reported by check and fsck.runway, removed by --repair
$SYS put EFI/RUNWAY/BLOBS/stage.rwyctl junk && $SYS put 'EFI/RUNWAY/BLOBS/.staging%2F1.2.x' junk || fail "staging fixture"
want "$(code $Z check)" 0 "check with staging files"
has "$T/out" "warn: BLOBS/stage.rwyctl: staging file left behind"
has "$T/out" "warn: BLOBS/.staging%2F1.2.x: staging file left behind"
$BUILD/fsck.runway "$T/z.store" >"$T/out" 2>&1 || fail "fsck.runway with staging files"
has "$T/out" "warn: EFI/RUNWAY: BLOBS/.staging%2F1.2.x: staging file left behind"
want "$(code $Z check --repair --unlock)" 0 "repair removes the staging files"
has "$T/out" "repair: 2 orphan blobs and staging files removed"
want "$(code $HFS "$T/z.store" cat EFI/RUNWAY/BLOBS/.staging%2F1.2.x)" 1 "the staging file is gone"
want "$(code $Z check)" 0 "check after the repair"
grep -q "staging" "$T/out" && fail "staging still reported"
$BUILD/fsck.runway "$T/z.store" >/dev/null 2>&1 || fail "fsck.runway after the repair"
# a violation is not repairable: the store stays untouched
$HFS "$T/c.store" booted elsewhere || fail "violation fixture"
want "$(code $C check)" 8 "check with a violation"
sumc=$(sha256sum "$T/c.store")
want "$(code $C check --repair --unlock)" 8 "repair refused on a violation"
want "$(sha256sum "$T/c.store")" "$sumc" "refused repair wrote"
# recovery set
mk --uuid $UUID1 "$T/rs.store"; seed "$T/rs.store"
RS="$CTL --store $T/rs.store"
printf 'rkernel' >"$T/rk"; printf 'rinitrd' >"$T/ri"
want "$(code $RS recovery set --kernel "$T/rk")" 2 "recovery set without --unlock"
want "$(code $RS recovery set --unlock)" 2 "recovery set without --kernel"
want "$(code $RS recovery set --unlock --kernel "$T/rk" --entry "rec.conf")" 2 "recovery set refuses --entry"
want "$(code $RS recovery set --unlock --kernel "$T/rk" --meta "version 1")" 2 "recovery set refuses --meta"
want "$(code $RS recovery set --unlock --kernel "$T/rk" --initrd "$T/ri" --args 'single' --label 'rescue-sys')" 0 "recovery set"
$RS status >"$T/out"; has "$T/out" "RECOVERY=valid"
want "$($RS get RECOVERY ARGS)" "single" "RECOVERY ARGS"
want "$($RS get RECOVERY LABEL)" "rescue-sys" "RECOVERY LABEL"
$RS check >/dev/null 2>&1 || fail "check after recovery set"
$BUILD/fsck.runway "$T/rs.store" >/dev/null 2>&1 || fail "fsck.runway after recovery set"
# a second set replaces it in one commit; USER still cannot touch it; the unlock closes
printf 'rk2' >"$T/rk2"
want "$(code $RS recovery set --unlock --kernel "$T/rk2")" 0 "recovery set again"
want "$(code $RS get RECOVERY ARGS)" 1 "old ARGS is gone"
want "$(code $RS get RECOVERY LABEL)" 1 "old LABEL is gone"
want "$(code $RS confirm RECOVERY)" 6 "USER after recovery set"
want "$(code $RS remove RECOVERY)" 6 "USER remove RECOVERY"
# a bad kernel path leaves the old RECOVERY
sumc=$(sha256sum "$T/rs.store")
want "$(code $RS recovery set --unlock --kernel "$T/none")" 1 "recovery set with a bad kernel"
$RS status >"$T/out"; has "$T/out" "RECOVERY=valid"
$BUILD/fsck.runway "$T/rs.store" >/dev/null 2>&1 || fail "fsck.runway after failed recovery set"
# wdt set
mk --uuid $UUID1 "$T/ws.store"; seed "$T/ws.store"
WS="$CTL --store $T/ws.store"
printf 'drvA' >"$T/A.EFI"; printf 'drvB' >"$T/B.EFI"; printf 'x' >"$T/bad.bin"
printf 'drvC' >"$T/C.EFI"
want "$(code $WS wdt set "$T/A.EFI")" 2 "wdt set without --unlock"
want "$(code $WS wdt set --unlock)" 2 "wdt set without an application"
want "$(code $WS wdt set --unlock "$T/A.EFI" "$T/C.EFI")" 0 "wdt set"
want "$($HFS "$T/ws.store" cat EFI/RUNWAY/WDT/A.EFI)" drvA "WDT copy"
want "$($HFS "$T/ws.store" cat EFI/RUNWAY/WDT/C.EFI)" drvC "WDT hardlink"
want "$(code $WS wdt set --unlock blob:$(sha256sum "$T/C.EFI" | cut -d' ' -f1))" 6 "wdt set refuses blob:"
$BUILD/fsck.runway "$T/ws.store" >/dev/null 2>&1 || fail "fsck.runway after wdt set"
# a second set replaces the content; a bad name keeps the old one
want "$(code $WS wdt set --unlock "$T/B.EFI")" 0 "wdt set again"
want "$(code $HFS "$T/ws.store" cat EFI/RUNWAY/WDT/A.EFI)" 1 "old application is gone"
want "$(code $WS wdt set --unlock "$T/bad.bin")" 6 "wdt set with a bad name"
want "$($HFS "$T/ws.store" cat EFI/RUNWAY/WDT/B.EFI)" drvB "failed wdt set kept the old application"
$BUILD/fsck.runway "$T/ws.store" >/dev/null 2>&1 || fail "fsck.runway after failed wdt set"

# --- 8b. mkstore --slots ---------------------------------------------------------------------------
mk --uuid $UUID1 --slots 3 "$T/sl.store"
want "$($HFS "$T/sl.store" cat EFI/RUNWAY/SLOTS)" 3 "mkstore --slots writes SLOTS"
want "$($HFS "$T/sl.store" cat EFI/RUNWAY/VERSION)" 1 "...and VERSION"
want "$(code $BUILD/fsck.runway "$T/sl.store")" 0 "fsck.runway accepts it"
for n in 1 65 abc 0; do
    want "$(code $BUILD/mkstore --size 64 --slots $n "$T/bad.store")" 2 "mkstore --slots $n"
done
has "$T/err" "--slots"
mk "$T/noslots.store"
$HFS "$T/noslots.store" cat EFI/RUNWAY/SLOTS >/dev/null 2>&1 && fail "no --slots, no EFI/RUNWAY/SLOTS"
$HFS "$T/noslots.store" cat EFI/RUNWAY/VERSION >/dev/null 2>&1 && fail "no --slots, no EFI/RUNWAY"

# --- 8b2. mkstore --quota, set and get QUOTA ---------------------------------------------------------
mk --uuid $UUID1 --quota "$T/q.store"
want "$($HFS "$T/q.store" cat EFI/RUNWAY/QUOTA)" 80 "mkstore --quota defaults to 80"
mk --uuid $UUID1 --quota=60 --slots 3 "$T/q.store"
want "$($HFS "$T/q.store" cat EFI/RUNWAY/QUOTA)" 60 "mkstore --quota=60 writes QUOTA"
want "$($HFS "$T/q.store" cat EFI/RUNWAY/SLOTS)" 3 "...next to SLOTS"
want "$(code $BUILD/fsck.runway "$T/q.store")" 0 "fsck.runway accepts it"
for n in 9 96 abc 0; do
    want "$(code $BUILD/mkstore --size 64 --quota=$n "$T/bad.store")" 2 "mkstore --quota=$n"
done
has "$T/err" "--quota"
Q="$CTL --store $T/q.store"
want "$($Q get QUOTA)" 60 "get QUOTA"
$Q set QUOTA 75 || fail "set QUOTA 75"
want "$($Q get QUOTA)" 75 "set QUOTA"
want "$(code $Q set QUOTA 96)" 6 "set QUOTA 96 is refused"
want "$($Q get QUOTA)" 75 "...and keeps the old value"
$Q status | grep -qx "QUOTA=75" || fail "status shows QUOTA"
want "$(code $Q set QUOTA off)" 0 "set QUOTA off"
$Q get QUOTA >/dev/null 2>&1 && fail "QUOTA still set after off"
$Q status | grep -qx "QUOTA=-" || fail "status shows QUOTA cleared"
want "$(code $Q set QUOTA off)" 0 "set QUOTA off again"
want "$(code $Q set QUOTA 70)" 0 "set QUOTA after off"
want "$($Q get QUOTA)" 70 "...and it is kept"

# --- 8c. SLOTS and update --------------------------------------------------------------------------
mk --uuid $UUID1 --slots 2 "$T/u.store"
U="$CTL --store $T/u.store"
for v in a b c d e f; do echo "kernel-$v" >"$T/k-$v"; done
want "$($U get SLOTS)" 2 "get SLOTS"
$U status >"$T/out"; has "$T/out" "SLOTS=2"
want "$(code $U update --kernel "$T/k-a")" 0 "update, first slot"
want "$(awk '{print $1}' "$T/out")" CONFIG1 "update creates the first slot while fewer than SLOTS exist"
want "$(code $U update --kernel "$T/k-b")" 0 "update, second slot"
want "$(awk '{print $1}' "$T/out")" CONFIG2 "...and the second"
sum=$(sha256sum "$T/u.store")
want "$(code $U update --kernel "$T/k-c")" 6 "update with both slots pending"
has "$T/err" "no slot can be recycled"
want "$(code $U install --kernel "$T/k-c")" 7 "install with SLOTS slots in use"
has "$T/err" "no more slot may be removed to meet QUOTA or SLOTS"
want "$(sha256sum "$T/u.store")" "$sum" "refused commands leave the store untouched"
want "$(code $U update --name CONFIG1 --kernel "$T/k-c")" 2 "update takes no --name"
want "$(code $U update)" 2 "update needs --kernel"
$U confirm CONFIG1 >/dev/null; $U confirm CONFIG2 >/dev/null
want "$(code $U update --kernel "$T/k-c" --args "quiet")" 0 "update recycles the oldest OK slot"
want "$(cat "$T/out")" "CONFIG1 3" "...CONFIG1, new revision"
want "$($HFS "$T/u.store" cat EFI/RUNWAY/CONFIG1/KERNEL)" kernel-c "...with the new payload"
want "$($U get CONFIG1 STATE)" INSTALLED "...INSTALLED"
want "$($U get CONFIG1 ARGS)" quiet "...and its ARGS"
want "$($U get CONFIG2 STATE)" OK "the other slot is untouched"
want "$(code $U update --kernel "$T/k-d")" 6 "the new slot is pending and CONFIG2 is the newest OK"
$U confirm CONFIG1 >/dev/null
want "$(code $U update --kernel "$T/k-d" --label "updated-slot")" 0 "round robin: next the other slot"
want "$(cat "$T/out")" "CONFIG2 4" "...CONFIG2"
want "$($HFS "$T/u.store" cat EFI/RUNWAY/CONFIG2/KERNEL)" kernel-d "...with its payload"
want "$($U get CONFIG2 LABEL)" "updated-slot" "LABEL set by update"
$U fail CONFIG2 >/dev/null
$U confirm CONFIG1 >/dev/null
want "$(code $U update --kernel "$T/k-e")" 0 "a FAILED slot is recycled"
want "$(cat "$T/out")" "CONFIG2 5" "...CONFIG2"
want "$($U get CONFIG1 STATE)" OK "...and the OK slot stays"
want "$(code $BUILD/fsck.runway "$T/u.store")" 0 "fsck.runway after the rotation"
# a failed update keeps the old slot
$U confirm CONFIG2 >/dev/null
usnap() { { $U status; $CTL --store "$T/u.store" stores; } 2>&1; }
before=$(usnap)
want "$(code $U update --kernel "$T/nosuch")" 1 "update with a missing kernel"
want "$(usnap)" "$before" "...changes nothing"
# SLOTS: set, range, lowering, growing
want "$(code $U set SLOTS 0)" 6 "set SLOTS 0"
want "$(code $U set SLOTS 65)" 6 "set SLOTS 65"
want "$(code $U set SLOTS x)" 6 "set SLOTS text"
want "$($U get SLOTS)" 2 "a refused set keeps the value"
want "$(code $U set SLOTS 3)" 0 "raise SLOTS"
want "$(code $U install --kernel "$T/k-f")" 0 "install may grow up to SLOTS"
want "$(awk '{print $1}' "$T/out")" CONFIG3 "...as CONFIG<max+1>"
want "$(code $U set SLOTS 1)" 0 "lower SLOTS under the slot count"
want "$(code $U install --kernel "$T/k-f")" 7 "install is refused: the slots cannot go"
$U confirm CONFIG3 >/dev/null
want "$(code $U update --kernel "$T/k-f")" 7 "update is refused: the newest OK slot stays, so SLOTS 1 cannot be met"
want "$(code $U set SLOTS 3)" 0 "set SLOTS back to the count"
want "$(code $U update --kernel "$T/k-f")" 0 "update at SLOTS recycles"
# set SLOTS off: no cap, install grows past the old count, and SLOTS can be set again
mk --uuid $UUID1 --slots 2 "$T/so.store"
SO="$CTL --store $T/so.store"
want "$(code $SO set SLOTS off)" 0 "set SLOTS off"
$SO status | grep -qx "SLOTS=-" || fail "status shows SLOTS cleared"
for n in 1 2 3; do want "$(code $SO install --kernel "$T/k-a")" 0 "install past the old SLOTS, $n"; done
want "$(code $BUILD/fsck.runway "$T/so.store")" 0 "fsck.runway with no SLOTS"
want "$(code $SO set SLOTS 3)" 0 "set SLOTS after off"
want "$($SO get SLOTS)" 3 "...and it is kept"

# SLOTS is a cap: install and update remove old OK slots to stay within it
mk --uuid $UUID1 --slots 8 "$T/sh.store"
SH="$CTL --store $T/sh.store"
for n in 1 2 3 4 5 6; do $SH install --kernel "$T/k-a" --label s$n >/dev/null; $SH confirm CONFIG$n >/dev/null; done
$SH set SLOTS 3
want "$(code $SH install --kernel "$T/k-b")" 0 "install over SLOTS removes the old OK slots"
want "$($SH slots | wc -l)" 3 "...and stays at SLOTS"
want "$($SH slots | tr '\n' ' ')" "CONFIG5 CONFIG6 CONFIG7 " "...the oldest went"
$SH confirm CONFIG7 >/dev/null
want "$(code $SH update --kernel "$T/k-c")" 0 "update at SLOTS"
want "$($SH slots | wc -l)" 3 "...stays at SLOTS"
want "$(code $BUILD/fsck.runway "$T/sh.store")" 0 "fsck.runway after the cap"
# QUOTA set, SLOTS below the count, only INSTALLED slots: the refusal names the cause
mk --uuid $UUID1 --slots 8 --quota=90 "$T/cap.store"
CP="$CTL --store $T/cap.store"
for v in a b c; do $CP install --kernel "$T/k-$v" >/dev/null; done
$CP confirm CONFIG1 >/dev/null; $CP confirm CONFIG2 >/dev/null; $CP set SLOTS 2
want "$(code $CP update --kernel "$T/k-d")" 7 "update over SLOTS with no slot that may go"
has "$T/err" "no more slot may be removed to meet QUOTA or SLOTS"
# old INSTALLED slots go last, a pinned one stays
mk --uuid $UUID1 --slots 8 "$T/ev.store"
EV="$CTL --store $T/ev.store"
for n in 1 2 3 4 5 6; do $EV install --kernel "$T/k-a" >/dev/null; done
$EV confirm CONFIG1 >/dev/null; $EV confirm CONFIG2 >/dev/null; $EV set SLOTS 4
$EV set CONFIG3 PIN 1
sum=$(sha256sum "$T/ev.store")
want "$(code $EV install --kernel "$T/k-b")" 7 "only pinned and protected slots are left"
has "$T/err" "no more slot may be removed to meet QUOTA or SLOTS"
want "$(sha256sum "$T/ev.store")" "$sum" "...and nothing was removed"
$EV set CONFIG3 PIN 0
want "$(code $EV install --kernel "$T/k-b")" 0 "install evicts old INSTALLED slots"
want "$($EV slots | tr '\n' ' ')" "CONFIG1 CONFIG2 CONFIG6 CONFIG7 " "...oldest first, OK, newest INSTALLED and the new slot stay"
want "$(code $BUILD/fsck.runway "$T/ev.store")" 0 "fsck.runway after evicting INSTALLED slots"
# without SLOTS: install grows as before, update has nothing to rotate over
mk --uuid $UUID1 "$T/ns.store"; seed "$T/ns.store"
N="$CTL --store $T/ns.store"
want "$(code $N update --kernel "$T/k-a")" 6 "update without SLOTS"
has "$T/err" "SLOTS"
want "$(code $N install --kernel "$T/k-a")" 0 "install without SLOTS"
want "$(code $N get SLOTS)" 1 "get SLOTS when unset"
# an incomplete slot counts, and is recycled first
mk --uuid $UUID1 --slots 2 "$T/inc.store"
I2="$CTL --store $T/inc.store"
$I2 update --kernel "$T/k-a" >/dev/null; $I2 confirm CONFIG1 >/dev/null
$HFS "$T/inc.store" mkdir EFI/RUNWAY/CONFIG7
want "$(code $I2 update --kernel "$T/k-b")" 0 "update with an incomplete slot"
want "$(cat "$T/out")" "CONFIG7 2" "...takes its place first"
want "$(code $BUILD/fsck.runway "$T/inc.store")" 0 "fsck.runway"

# --- 9. TPM key source (swtpm on a unix socket) ----------------------------------------------------
if command -v swtpm >/dev/null; then
    S=$(mktemp -d /var/tmp/rwy-rwyctl-tpm.XXXXXX)
    trap 'kill $SPID 2>/dev/null; rm -rf "$T" "$S"' EXIT
    swtpm socket --tpmstate dir="$S" --server type=unixio,path="$S/sock" --ctrl type=unixio,path="$S/ctrl" \
          --tpm2 --flags not-need-init,startup-clear >"$S/log" 2>&1 &
    SPID=$!
    for i in $(seq 1 50); do [ -S "$S/sock" ] && break; sleep 0.1; done
    mk --encrypt --passphrase hunter2 --dek-out "$T/dek4" --uuid $UUID1 "$T/t.store"; HOST_FS_PASSPHRASE=hunter2 seed "$T/t.store"
    $BUILD/tpmtest "$S/sock" enroll "$T/dek4" "$T/t.store" || fail "enroll the TPM keyslot"
    TS="$CTL --store $T/t.store"
    want "$(code $TS --tpm "$S/sock" get CONFIG1 STATE)" 0 "TPM unseal opens the store"
    want "$(RWY_TPM="$S/sock" $TS get CONFIG1 STATE </dev/null)" OK "RWY_TPM"
    want "$(code $TS --tpm "$S/sock" confirm CONFIG2)" 0 "write through the TPM key"
    want "$(code $TS --tpm "$T/nosuch" get CONFIG1 STATE)" 4 "no such TPM device"
    has "$T/err" "trying the next key source"
    want "$(code $TS get CONFIG1 STATE)" 4 "no TPM given, none at the default path"
    # TPM-only store: no passphrase keyslot, so a failed unseal leaves nothing to fall back on
    mk --encrypt --passphrase hunter2 --dek-out "$T/dek5" --uuid $UUID1 "$T/o.store"; HOST_FS_PASSPHRASE=hunter2 seed "$T/o.store"
    $BUILD/tpmtest "$S/sock" enroll-only "$T/dek5" "$T/o.store" || fail "enroll the TPM-only store"
    TO="$CTL --store $T/o.store"
    want "$(code $TO --tpm "$S/sock" get CONFIG1 STATE)" 0 "TPM-only store opens through the TPM"
    want "$(code $TO --tpm "$S/sock" confirm CONFIG2)" 0 "TPM-only store written through the TPM"
    want "$(code $TO --passphrase-file "$T/pass" get CONFIG1 STATE)" 4 "TPM-only store has no passphrase"
    want "$(code $TO get CONFIG1 STATE)" 4 "TPM-only store, no TPM, no terminal"
    $BUILD/tpmtest "$S/sock" extend || fail "extend PCR 7"
    want "$(code $TO --tpm "$S/sock" get CONFIG1 STATE)" 4 "TPM-only store, PCR 7 changed"
    has "$T/err" "TPM unseal failed"
    want "$(code $TS --tpm "$S/sock" get CONFIG1 STATE)" 4 "PCR 7 changed"
    has "$T/err" "TPM unseal failed"
    grep -q "SECURITY ALERT" "$T/err" && fail "a failed unseal is not a security alert"
    want "$(code $TS --tpm "$S/sock" --passphrase-file "$T/pass" get CONFIG1 STATE)" 0 "passphrase after a failed unseal"
    # transport corners: retried commands, a reply split in pieces, never-ending retries, a character device
    px() { n=$1; shift; python3 tests/host/tpmproxy.py "$S/$n" "$S/sock" "$@" & PXPID="$PXPID $!"; set -- "$n"; for i in $(seq 1 50); do [ -S "$S/$1" ] && break; sleep 0.1; done; }
    PXPID=""; trap 'kill $SPID $PXPID 2>/dev/null; rm -rf "$T" "$S"' EXIT
    $BUILD/tpmtest "$S/sock" enroll "$T/dek4" "$T/t.store" || fail "enroll again after the PCR change"
    px p-retry retry=5; px p-split split=3; px p-stuck retry=-1
    want "$(code $TS --tpm "$S/p-retry" get CONFIG1 STATE)" 0 "unseal through TPM_RC_RETRY"
    want "$(code $TS --tpm "$S/p-split" get CONFIG1 STATE)" 0 "unseal with replies split in pieces"
    want "$(code $TS --tpm "$S/p-stuck" get CONFIG1 STATE)" 4 "TPM that only answers retry"
    has "$T/err" "TPM unseal failed"
    want "$(code $TS --tpm /dev/null get CONFIG1 STATE)" 4 "character device that never answers"
    has "$T/err" "TPM unseal failed"
    # mirror: each member holds its own sealed blob of the same key
    cp "$T/t.store" "$T/m1.store"; cp "$T/t.store" "$T/m2.store"
    $BUILD/tpmtest "$S/sock" enroll "$T/dek4" "$T/m1.store" && $BUILD/tpmtest "$S/sock" enroll "$T/dek4" "$T/m2.store" || fail "enroll mirror"
    TM="$CTL --store $T/m1.store --store $T/m2.store"
    want "$(code $TM --tpm "$S/sock" confirm CONFIG2)" 0 "mirror, two blobs, both unseal"
    # the first blob was sealed before the PCR change, so only the second unseals
    cp "$T/t.store" "$T/m1.store"; cp "$T/t.store" "$T/m2.store"
    $BUILD/tpmtest "$S/sock" enroll "$T/dek4" "$T/m1.store" || fail "enroll m1"
    $BUILD/tpmtest "$S/sock" extend || fail "extend PCR 7 again"
    $BUILD/tpmtest "$S/sock" enroll "$T/dek4" "$T/m2.store" || fail "enroll m2"
    want "$(code $TM --tpm "$S/sock" get CONFIG1 STATE)" 0 "mirror, first blob stale, second unseals"
    has "$T/err" "TPM unseal failed"
    want "$(code $TM --tpm "$S/sock" confirm CONFIG2)" 0 "mirror write after one stale blob"
else
    echo "rwyctl TPM tests: SKIP (no swtpm)"
fi

# --- 10. Anti-rollback anchor: enroll, status, reset (fresh swtpm) -----------------------------------
if command -v swtpm >/dev/null; then
    A=$(mktemp -d /var/tmp/rwy-rwyctl-anchor.XXXXXX)
    trap 'kill $SPID $PXPID $APID 2>/dev/null; rm -rf "$T" "$S" "$A"' EXIT
    swtpm socket --tpmstate dir="$A" --server type=unixio,path="$A/sock" --ctrl type=unixio,path="$A/ctrl" \
          --tpm2 --flags not-need-init,startup-clear >"$A/log" 2>&1 &
    APID=$!
    for i in $(seq 1 50); do [ -S "$A/sock" ] && break; sleep 0.1; done
    mk --encrypt --passphrase hunter2 --uuid $UUID1 "$T/an.store"
    AN="$CTL --store $T/an.store --passphrase-file $T/pass --tpm $A/sock"
    st() { $AN anchor status 2>/dev/null | sed -n "s/^$1=//p"; }
    want "$(code $AN anchor status)" 0 "anchor status before enroll"
    want "$(st ENROLLED)" no "not enrolled"
    want "$(st ANCHOR)" off "anchor is off"
    mk --uuid $UUID2 "$T/plain.store"
    want "$(code $CTL --store "$T/plain.store" --tpm "$A/sock" anchor enroll)" 6 "enroll refuses an unencrypted store"
    want "$(code $CTL --store "$T/an.store" --tpm "$A/sock" anchor enroll)" 4 "enroll needs the store key"
    want "$(code $AN --tpm "$T/nosuch" anchor enroll)" 1 "enroll without a TPM"
    want "$(st ENROLLED)" no "a failed enroll leaves the store as it was"
    want "$(code $AN anchor enroll)" 0 "enroll"
    NV=$(st NV-INDEX)
    want "$(st ENROLLED)" yes "enrolled"
    want "$(st ANCHOR)" ok "epoch equals the counter"
    want "$(st EPOCH)" "$($BUILD/tpmtest "$A/sock" nv-read "$NV")" "epoch is the counter value at enroll"
    want "$(code $AN anchor enroll)" 6 "enroll twice"
    want "$(code $AN init)" 0 "init on an enrolled store"
    want "$(code $BUILD/fsck.runway --key hunter2 "$T/an.store")" 0 "fsck.runway after enroll"
    has "$T/out" "counter not checked (no --tpm)"
    TC="$BUILD/fsck.runway --key hunter2 --tpm $A/sock $T/an.store"
    want "$(code $TC)" 0 "fsck.runway with the TPM"
    has "$T/out" "epoch"
    has "$T/out" "equals the counter"
    # a restored copy: the counter moved on without the record
    $BUILD/tpmtest "$A/sock" nv-incr "$NV" || fail "raise the counter"
    want "$(st ANCHOR)" rolled-back "record behind the counter"
    want "$(code $TC)" 4 "fsck.runway on a record behind the counter"
    has "$T/out" "store rolled back (epoch"
    want "$(code $AN anchor reset)" 0 "reset after a restored copy"
    want "$(st ANCHOR)" ok "ok after reset"
    want "$(st EPOCH)" "$($BUILD/tpmtest "$A/sock" nv-read "$NV")" "epoch follows the new counter"
    # PCR 7 changed: the counter cannot be read; reset (owner auth) defines it again for the new value
    $BUILD/tpmtest "$A/sock" extend || fail "extend PCR 7"
    want "$(st ANCHOR)" unavailable "PCR 7 changed"
    want "$(code $TC)" 0 "fsck.runway with PCR 7 changed is a warning"
    has "$T/out" "PCR 7 differs"
    $BUILD/tpmtest "$A/sock" nv-undef "$NV" || fail "remove the counter"
    want "$(code $TC)" 4 "fsck.runway with the counter gone"
    has "$T/out" "is gone"
    want "$(code $AN anchor reset)" 0 "reset after PCR 7 changed"
    want "$(st ANCHOR)" ok "ok again"
    # offline: the policy comes from the PCR 7 value given, not from the TPM
    want "$(code $AN anchor reset --pcr7 zz)" 2 "--pcr7 needs 64 hex digits"
    want "$(code $AN anchor reset --pcr7 $(printf '%064d' 0))" 0 "reset with a PCR 7 value that is not the TPM's"
    want "$(st ANCHOR)" unavailable "the policy does not match the live PCR 7"
    want "$(code $AN anchor reset --pcr7 $($BUILD/tpmtest "$A/sock" pcr))" 0 "reset with the live PCR 7 value"
    want "$(st ANCHOR)" new "the policy matches; the first increment is left to the device"
    want "$(code $AN anchor reset)" 0 "reset with the live PCR 7"
    # reset on a store that was never enrolled enrolls it
    mk --encrypt --passphrase hunter2 --uuid $UUID2 "$T/an2.store"
    want "$(code $CTL --store "$T/an2.store" --passphrase-file "$T/pass" --tpm "$A/sock" anchor reset)" 0 "reset enrolls"
    want "$($CTL --store "$T/an2.store" --passphrase-file "$T/pass" --tpm "$A/sock" anchor status 2>/dev/null | sed -n 's/^ENROLLED=//p')" yes "enrolled by reset"
    # an enroll cut after it defined the counter: running it again finishes it
    mk --encrypt --passphrase hunter2 --uuid $UUID3 "$T/an3.store"
    cp "$T/an3.store" "$T/an3.pre"
    A3="$CTL --store $T/an3.store --passphrase-file $T/pass --tpm $A/sock"
    want "$(code $A3 anchor enroll --pcr7 $($BUILD/tpmtest "$A/sock" pcr))" 0 "enroll, counter defined"
    cp "$T/an3.pre" "$T/an3.store"
    want "$($A3 anchor status 2>/dev/null | sed -n 's/^ENROLLED=//p')" no "the cut left the record not enrolled"
    want "$(code $A3 anchor enroll)" 0 "enroll again finishes the one that was cut"
    want "$($A3 anchor status 2>/dev/null | sed -n 's/^ANCHOR=//p')" ok "enrolled and in step with the counter"
    want "$(code $AN anchor)" 2 "anchor needs a subcommand"
    # slots on an enrolled store: the epoch and the counter rise together, once per commit
    echo kimg >"$T/ak"
    ep() { st EPOCH; }
    nvc() { $BUILD/tpmtest "$A/sock" nv-read "$NV"; }
    rose() {   # rose WHAT BY: epoch moved by BY and the counter agrees
        want "$(ep)" "$((E0 + $2))" "$1: epoch"
        want "$(nvc)" "$(ep)" "$1: counter follows the epoch"
        E0=$(ep)
    }
    want "$(st ANCHOR)" ok "ok before the first install"
    want "$($AN status 2>/dev/null | sed -n 's/^ANCHOR=//p')" ok "status shows the anchor"
    E0=$(ep)
    want "$(code $AN install --kernel "$T/ak")" 0 "install"
    rose install 1
    want "$(code $AN install --kernel "$T/ak")" 0 "second install"
    rose "second install" 1
    want "$(code $AN confirm CONFIG1)" 0 "confirm"
    rose confirm 0
    want "$(code $AN fail CONFIG1)" 0 "fail"
    rose fail 1
    want "$(code $AN fail CONFIG1)" 0 "fail again"
    rose "fail again" 0
    want "$(code $AN set CONFIG2 STATE FAILED)" 0 "set STATE FAILED"
    rose "set STATE FAILED" 1
    want "$(code $AN remove CONFIG2)" 0 "remove"
    rose remove 1
    want "$(code $AN set SLOTS 1)" 0 "set SLOTS"
    rose "set SLOTS" 0
    want "$(code $AN update --kernel "$T/ak")" 0 "update recycles a slot"
    rose "update (remove and finalize in one commit)" 1
    # with every slot taken and OK, an update would evict one; once the anchor cannot be trusted it must not
    $AN set SLOTS 3 || fail "set SLOTS"
    while [ "$($AN status 2>/dev/null | grep -c '^CONFIG')" -lt 3 ]; do
        want "$(code $AN install --kernel "$T/ak")" 0 "install to fill SLOTS"
    done
    for c in $($AN status 2>/dev/null | awk '/^CONFIG/ {print $1}'); do $AN confirm "$c" >/dev/null; done
    # the anchor cannot be trusted: the commands that raise it refuse, the others go on
    $BUILD/tpmtest "$A/sock" extend || fail "extend PCR 7"
    N0=$($AN status 2>/dev/null | awk '/^CONFIG/ {printf "%s ", $1}')
    for c in "install --kernel $T/ak" "update --kernel $T/ak" "remove CONFIG1" "fail CONFIG1" "set CONFIG1 STATE FAILED"; do
        want "$(code $AN $c)" 6 "$c with PCR 7 changed"
        has "$T/err" "anchor"
    done
    want "$($AN status 2>/dev/null | sed -n 's/^ANCHOR=//p')" unavailable "status says unavailable"
    want "$($AN status 2>/dev/null | awk '/^CONFIG/ {printf "%s ", $1}')" "$N0" "no slot was written or evicted"
    want "$(code $AN confirm CONFIG1)" 0 "confirm needs no anchor"
    want "$(code $AN anchor reset)" 0 "reset"
    $AN set SLOTS 4 || fail "set SLOTS"
    E0=$(ep)
    want "$(code $AN install --kernel "$T/ak")" 0 "install works again after reset"
    rose "install after reset" 1
    $BUILD/tpmtest "$A/sock" nv-incr "$NV" || fail "raise the counter"
    want "$(code $AN remove CONFIG1)" 6 "remove on a rolled back store"
    want "$(code $AN anchor reset)" 0 "reset after the rollback"
    want "$(code $CTL --store "$T/an.store" --passphrase-file "$T/pass" --tpm "$T/nosuch" install --kernel "$T/ak")" 6 "install with no TPM on an enrolled store"
    # a store that is not enrolled never touches the TPM
    mk --uuid $UUID2 "$T/an3.store"
    N3="$CTL --store $T/an3.store --tpm $T/nosuch"
    want "$(code $N3 init)" 0 "init"
    want "$(code $N3 install --kernel "$T/ak")" 0 "install with no TPM on a store that is not enrolled"
    want "$(code $N3 fail CONFIG1)" 0 "fail"
    want "$(code $N3 remove CONFIG1)" 0 "remove"
    want "$($N3 status 2>/dev/null | sed -n 's/^ANCHOR=//p')" off "status says off"
else
    echo "rwyctl anchor tests: SKIP (no swtpm)"
fi

# --- 13. reencrypt: DEK rotation on encrypted stores -------------------------------------------
mk --uuid $UUID1 "$T/plain_re.store"
want "$(code $CTL --store "$T/plain_re.store" reencrypt --new-passphrase hunter3)" 6 "reencrypt refuses unencrypted store"
has "$T/err" "encrypted"

mk --encrypt --passphrase hunter2 --uuid $UUID1 "$T/re_enc.store"
HOST_FS_PASSPHRASE=hunter2 seed "$T/re_enc.store"
want "$(code $CTL --store "$T/re_enc.store" --passphrase-file "$T/pass" reencrypt --new-passphrase hunter3 --new-dek-out "$T/newdek")" 0 "reencrypt rotates key"
printf "hunter3\n" >"$T/pass3"
want "$(code $CTL --store "$T/re_enc.store" --passphrase-file "$T/pass" status)" 4 "old passphrase rejected after reencrypt"
want "$(code $CTL --store "$T/re_enc.store" --passphrase-file "$T/pass3" status)" 0 "new passphrase accepted after reencrypt"
want "$($CTL --store "$T/re_enc.store" --passphrase-file "$T/pass3" get CONFIG1 STATE)" OK "content preserved after reencrypt"
want "$(code $CTL --store "$T/re_enc.store" --dek-file "$T/newdek" get CONFIG2 STATE)" 0 "new DEK accepted after reencrypt"
want "$(code $BUILD/fsck.runway --key hunter3 "$T/re_enc.store")" 0 "fsck.runway clean after reencrypt"

want "$(code $CTL --store "$T/re_enc.store" reencrypt --old-key hunter3 --new-key hunter4)" 0 "reencrypt with --old-key and --new-key"
printf "hunter4\n" >"$T/pass4"
want "$(code $CTL --store "$T/re_enc.store" --passphrase-file "$T/pass3" status)" 4 "previous passphrase rejected"
want "$(code $CTL --store "$T/re_enc.store" --passphrase-file "$T/pass4" status)" 0 "latest passphrase accepted"

want "$(code $CTL --store "$T/re_enc.store" reencrypt --old-key wrongpass --new-key foo)" 4 "wrong old key refuses"

mk --encrypt --passphrase hunter2 --uuid $UUID1 "$T/mr1.store"
cp "$T/mr1.store" "$T/mr2.store"
HOST_FS_PASSPHRASE=hunter2 seed "$T/mr1.store"
HOST_FS_PASSPHRASE=hunter2 seed "$T/mr2.store"
want "$(code $CTL --store "$T/mr1.store" --store "$T/mr2.store" --passphrase-file "$T/pass" reencrypt --new-passphrase hunter5)" 0 "mirror reencrypt"
printf "hunter5\n" >"$T/pass5"
want "$(code $CTL --store "$T/mr1.store" --passphrase-file "$T/pass5" status)" 0 "mirror member 1 updated"
want "$(code $CTL --store "$T/mr2.store" --passphrase-file "$T/pass5" status)" 0 "mirror member 2 updated"
want "$(code $CTL --store "$T/mr1.store" --passphrase-file "$T/pass" status)" 4 "mirror member 1 old key rejected"
want "$(code $CTL --store "$T/mr2.store" --passphrase-file "$T/pass" status)" 4 "mirror member 2 old key rejected"

cp "$T/mr1.store" "$T/mr3.store"
HOST_FS_PASSPHRASE=hunter5 slot "$T/mr1.store" 5 INSTALLED
want "$(code $CTL --store "$T/mr1.store" --store "$T/mr3.store" --passphrase-file "$T/pass5" reencrypt --new-passphrase hunter6)" 6 "mirror reencrypt refuses degraded mirror"
has "$T/err" "healthy"

mk --encrypt --passphrase hunter2 --uuid $UUID1 "$T/full_enc.store"
dd if=/dev/zero of="$T/36m.bin" bs=1M count=36 2>/dev/null
HOST_FS_PASSPHRASE=hunter2 $HFS "$T/full_enc.store" put /BIG.BIN "@$T/36m.bin" || fail "put big file"
rm -f "$T/36m.bin"
sum_before=$(sha256sum "$T/full_enc.store")
want "$(code $CTL --store "$T/full_enc.store" --passphrase-file "$T/pass" reencrypt --new-passphrase hunter7)" 7 "reencrypt NOSPC error"
want "$(sha256sum "$T/full_enc.store")" "$sum_before" "NOSPC leaves image unchanged"
want "$(code $CTL --store "$T/full_enc.store" --passphrase-file "$T/pass" status)" 0 "old key still mounts cleanly after NOSPC"

# --- 14. flight and log: inspection, raw mode, verification, and tamper detection ----------------
mk --uuid $UUID1 "$T/fl1.store"
want "$(code $CTL --store "$T/fl1.store" flight show)" 0 "flight show on empty store"
[ ! -s "$T/out" ] || fail "expected empty flight show output"
mk --flight-records 0 --uuid $UUID1 "$T/nofl.store"
want "$(code $CTL --store "$T/nofl.store" flight show)" 6 "flight show without flight area"
has "$T/err" "no flight recorder area"

# Encrypted store: recover-next records to plaintext flight recorder
mk --encrypt --passphrase hunter2 --uuid $UUID1 "$T/fl_enc.store"
HOST_FS_PASSPHRASE=hunter2 seed "$T/fl_enc.store"
HOST_FS_SYSTEM=1 HOST_FS_PASSPHRASE=hunter2 $HFS "$T/fl_enc.store" mkdir EFI/RUNWAY/RECOVERY &&
HOST_FS_SYSTEM=1 HOST_FS_PASSPHRASE=hunter2 $HFS "$T/fl_enc.store" put EFI/RUNWAY/RECOVERY/KERNEL rk || fail "rec fixture"
want "$(code $CTL --store "$T/fl_enc.store" --passphrase-file "$T/pass" recover-next)" 0 "recover-next records flight event"
# Unmounted / locked raw mode: reads without key!
want "$(code $CTL --store "$T/fl_enc.store" flight show)" 0 "flight show on locked store without key"
has "$T/out" "RECOVER_NEXT"
has "$T/out" "RECOVERY"
# Raw flag test
want "$(code $CTL --store "$T/fl_enc.store" flight show --raw)" 0 "flight show raw mode"
has "$T/out" "event=RECOVER_NEXT"
has "$T/out" "slot=RECOVERY"
# Positional syntax without key
want "$(code $CTL flight show "$T/fl_enc.store")" 0 "flight show positional target without key"
has "$T/out" "RECOVER_NEXT"
# A word after the command is an argument of the command, not a store
for args in "get CONFIG1 STATE" "install --kernel $T/nokernel" "recovery area show"; do
    code $CTL $args >/dev/null
    grep -q "missing or not a Runway store" "$T/err" && fail "'$args': an argument was taken for a store"
done

# Update journal: log show on plain store
mk --uuid $UUID1 "$T/j1.store"
$CTL --store "$T/j1.store" init >/dev/null
echo k1 >"$T/k1"
want "$(code $CTL --store "$T/j1.store" install --kernel "$T/k1" --label "v1.0-factory")" 0 "install with label"
want "$(code $CTL --store "$T/j1.store" confirm CONFIG1)" 0 "confirm CONFIG1"
want "$(code $CTL --store "$T/j1.store" log show)" 0 "log show on plain store"
has "$T/out" "INSTALL"
has "$T/out" "CONFIG1"
has "$T/out" "v1.0-factory"
has "$T/out" "CONFIRM"
# Raw flag test
want "$(code $CTL --store "$T/j1.store" log show --raw)" 0 "log show raw on plain store"
has "$T/out" "event=INSTALL"
has "$T/out" "slot=CONFIG1"
has "$T/out" "label=\"v1.0-factory\""
has "$T/out" "event=CONFIRM"
want "$(code $CTL log show "$T/j1.store")" 0 "log show positional target"
has "$T/out" "v1.0-factory"

# remove keeps the label and the payload hash in the REMOVE line, as the rmdir of a mounted slot does
echo k2 >"$T/k2"
want "$(code $CTL --store "$T/j1.store" install --kernel "$T/k2" --label "v2.0-remove")" 0 "install the slot to remove"
want "$(code $CTL --store "$T/j1.store" remove CONFIG2)" 0 "remove with a label"
want "$(code $CTL --store "$T/j1.store" log show --raw)" 0 "log show raw after remove"
grep "event=REMOVE" "$T/out" >"$T/rm.line"
has "$T/rm.line" 'slot=CONFIG2'
has "$T/rm.line" 'label="v2.0-remove"'
grep -Eq 'payload_hash=[0-9a-f]{64}' "$T/rm.line" || fail "REMOVE line has no payload hash"
# the blob payload gives the same hash at install, confirm and remove
grep "event=INSTALL" "$T/out" | grep 'slot=CONFIG2' | grep -o 'payload_hash=[0-9a-f]*' >"$T/h.install"
grep -o 'payload_hash=[0-9a-f]*' "$T/rm.line" >"$T/h.remove"
cmp -s "$T/h.install" "$T/h.remove" || fail "REMOVE hash differs from INSTALL hash"
want "$(code $CTL --store "$T/j1.store" log verify)" 0 "log verify after remove"
want "$(code $BUILD/fsck.runway "$T/j1.store")" 0 "fsck.runway after removing an installed slot as USER"

# Store without journal area
mk --journal-records 0 --uuid $UUID1 "$T/nojl.store"
want "$(code $CTL --store "$T/nojl.store" log show)" 6 "log show without journal area"
has "$T/err" "no update journal area"
want "$(code $CTL --store "$T/nojl.store" log verify)" 6 "log verify without journal area"
has "$T/err" "no update journal area"

# Encrypted store: journal locked without key, accessible with key
mk --encrypt --passphrase hunter2 --uuid $UUID1 "$T/je.store"
$CTL --store "$T/je.store" --passphrase-file "$T/pass" init >/dev/null
$CTL --store "$T/je.store" --passphrase-file "$T/pass" install --kernel "$T/k1" --label "enc-payload" >/dev/null
want "$(code $CTL --store "$T/je.store" log show)" 4 "log show on encrypted store without key is locked"
want "$(code $CTL --store "$T/je.store" --passphrase-file "$T/pass" log show)" 0 "log show on encrypted store with key"
has "$T/out" "enc-payload"

# log verify
want "$(code $CTL --store "$T/j1.store" log verify)" 0 "log verify plain store"
has "$T/out" "log verify: OK"
want "$(code $CTL --store "$T/je.store" --passphrase-file "$T/pass" log verify)" 0 "log verify encrypted store"
has "$T/out" "log verify: OK"
want "$(code $CTL --passphrase-file "$T/pass" log verify "$T/je.store")" 0 "log verify positional syntax"
has "$T/out" "log verify: OK"

# fsck.runway: verifies geometry and hash chain
want "$(code $BUILD/fsck.runway "$T/j1.store")" 0 "fsck.runway on store with journal"
has "$T/out" "flight area: 2 x 1 blocks"
has "$T/out" "journal area: 2 x 32 blocks"
has "$T/out" "journal: hash chain verified"
want "$(code $BUILD/fsck.runway "$T/je.store")" 0 "unauthenticated fsck.runway on encrypted store"
has "$T/out" "flight area: copy A: empty"
has "$T/out" "note: flight area is empty"
want "$(code $BUILD/fsck.runway "$T/fl_enc.store")" 0 "unauthenticated fsck.runway on encrypted store with flight records"
has "$T/out" "flight area: copy A: valid"

# Tamper detection: corrupt an entry in the journal of j1.store
for i in 2 3 4 5 6 7 8 9 10 11; do
    $CTL --store "$T/j1.store" install --kernel "$T/k1" --name "CONFIG$i" >/dev/null
done
cp "$T/j1.store" "$T/j_bad.store"
python3 -I - "$T/j_bad.store" <<'PY' || fail "tamper fixture"
import struct, sys
f = open(sys.argv[1], "r+b")
f.seek(176)
jstart = struct.unpack("<I", f.read(4))[0]
jblocks = struct.unpack("<I", f.read(4))[0]
for copy in (0, 1):
    f.seek((jstart + copy * jblocks + 1) * 4096 + 256)
    b = f.read(1)
    f.seek((jstart + copy * jblocks + 1) * 4096 + 256)
    f.write(bytes([b[0] ^ 0xff]))
PY
want "$(code $CTL --store "$T/j_bad.store" log verify)" 8 "log verify detects tampered journal"
has "$T/err" "broken or corrupted"
want "$(code $BUILD/fsck.runway "$T/j_bad.store")" 4 "fsck.runway detects tampered journal"
has "$T/out" "journal hash chain broken or corrupted"

# --- 15. network flight report settings: plaintext, no key needed --------------------------------
mk --uuid $UUID1 "$T/net.store"
want "$(code $CTL --store "$T/net.store" log settings)" 0 "log settings on an empty area"
has "$T/out" "LOG-SERVER=-"
has "$T/out" "LOG-TIMEOUT=5"
has "$T/out" "LOG-BACKLOG=8"
want "$(code $CTL --store "$T/net.store" log set LOG-SERVER 192.0.2.5:1514)" 0 "set server"
want "$(code $CTL --store "$T/net.store" log set LOG-INTERFACE 02:11:22:33:44:55)" 0 "set interface"
want "$(code $CTL --store "$T/net.store" log set LOG-ADDRESS 198.51.100.7/24,198.51.100.1)" 0 "set address"
want "$(code $CTL --store "$T/net.store" log set LOG-TIMEOUT 9)" 0 "set timeout"
want "$(code $CTL --store "$T/net.store" log set LOG-BACKLOG 0)" 0 "set backlog"
want "$(code $CTL --store "$T/net.store" log settings)" 0 "log settings after set"
has "$T/out" "LOG-SERVER=192.0.2.5:1514"
has "$T/out" "LOG-INTERFACE=02:11:22:33:44:55"
has "$T/out" "LOG-ADDRESS=198.51.100.7/24,198.51.100.1"
has "$T/out" "LOG-TIMEOUT=9"
has "$T/out" "LOG-BACKLOG=0"
want "$(code $CTL --store "$T/net.store" log set LOG-TIMEOUT 0)" 1 "timeout out of range"
has "$T/err" "invalid setting"
want "$(code $CTL --store "$T/net.store" log set LOG-SERVER 192.0.2.256)" 1 "bad server address"
want "$(code $CTL --store "$T/net.store" log set LOG-UNKNOWN x)" 2 "unknown setting"
want "$(code $CTL --store "$T/net.store" log unset LOG-INTERFACE)" 0 "unset interface"
want "$(code $CTL --store "$T/net.store" log settings)" 0 "log settings after unset"
has "$T/out" "LOG-INTERFACE=-"
want "$(code $BUILD/fsck.runway "$T/net.store")" 0 "fsck.runway with settings"
has "$T/out" "settings area: 2 x 1 blocks"
has "$T/out" "settings area: copy A: valid"
has "$T/out" "LOG-SERVER=192.0.2.5:1514"

# Host interfaces: a name gives the permanent MAC, copy-address reads the running address, log test needs a local MAC
want "$(code $CTL --store "$T/net.store" log set LOG-INTERFACE lo)" 2 "interface without a permanent address"
want "$(code $CTL --store "$T/net.store" log set LOG-INTERFACE nosuchif0)" 2 "unknown interface name"
want "$(code $CTL --store "$T/net.store" log copy-address nosuchif0)" 2 "copy address of an unknown interface"
want "$(code $CTL --store "$T/net.store" log copy-address lo)" 0 "copy address of loopback"
want "$(code $CTL --store "$T/net.store" log settings)" 0 "log settings after copy-address"
has "$T/out" "LOG-ADDRESS=127.0.0.1/8"
want "$(code $CTL --store "$T/net.store" log test)" 6 "log test without an interface"
has "$T/err" "must all be set"
want "$(code $CTL --store "$T/net.store" log set LOG-INTERFACE 02:11:22:33:44:55)" 0 "set interface again"
want "$(code $CTL --store "$T/net.store" log test)" 8 "log test without a local interface"
has "$T/err" "no local interface"

# The settings are plaintext: an encrypted store can show and change them without the key
mk --encrypt --passphrase hunter2 --uuid $UUID1 "$T/net_enc.store"
want "$(code $CTL --store "$T/net_enc.store" log set LOG-TIMEOUT 12)" 0 "set settings on a locked store"
want "$(code $CTL --store "$T/net_enc.store" log settings)" 0 "show settings on a locked store"
has "$T/out" "LOG-TIMEOUT=12"
want "$(code $BUILD/fsck.runway "$T/net_enc.store")" 0 "unauthenticated fsck.runway with settings"
has "$T/out" "LOG-TIMEOUT=12"

# --- put and cat: whole-file write and read, overwrite in place ------------------------------------
mk "$T/pc.store"
printf 'hello\nworld\n' > "$T/pc.in"
want "$(code $CTL --store "$T/pc.store" put READY "$T/pc.in")" 0 "put creates a file"
$CTL --store "$T/pc.store" cat READY > "$T/pc.out"
cmp "$T/pc.in" "$T/pc.out" || fail "cat does not return the bytes of put"
printf 'second\n' > "$T/pc.in2"
want "$(code $CTL --store "$T/pc.store" put READY "$T/pc.in2")" 0 "put replaces a file"
$CTL --store "$T/pc.store" cat READY > "$T/pc.out"
cmp "$T/pc.in2" "$T/pc.out" || fail "cat does not return the replaced bytes"
want "$(code $CTL --store "$T/pc.store" cat MISSING)" 1 "cat of a missing file fails"
want "$(code $CTL --store "$T/pc.store" put)" 2 "put needs a path and a source"
want "$(code $CTL --store "$T/pc.store" put X "$T/missing")" 1 "put of a missing source fails"
# --- --io-stats: the counters of one command, on standard error -----------------------------------
want "$(code $CTL --io-stats --store "$T/pc.store" cat READY)" 0 "cat with --io-stats"
has "$T/err" "core read meta: "
has "$T/err" "core read data: 1 calls (4K 1, "
has "$T/err" "core flushes: 0"
has "$T/err" "dev: reads "
cmp "$T/pc.in2" "$T/out" || fail "--io-stats changed the output"
want "$(code $CTL --io-stats --store "$T/pc.store" put READY "$T/pc.in")" 0 "put with --io-stats"
has "$T/err" "core write meta: "
has "$T/err" "core flushes: 2"
want "$(code $CTL --store "$T/pc.store" cat READY)" 0 "no --io-stats"
grep -q "core " "$T/err" && fail "counters printed without --io-stats"

# --- 15. mirror health: a writing command records the member it left out -----------------------------
mk --uuid $UUID1 "$T/h0.store"; seed "$T/h0.store"
cp "$T/h0.store" "$T/h1.store"
H="$CTL --store $T/h0.store --store $T/h1.store"
muuid() { $BUILD/fsck.runway "$1" | sed -n 's/^slot [AB]: member uuid //p' | sort -u; }
want "$(code $H set CONFIG2 STATE OK)" 0 "write to a mirror in step"
U1=$(muuid "$T/h1.store"); [ -n "$U1" ] || fail "the mirror member was not named"
want "$($HFS "$T/h0.store" health "$U1")" none "a healthy mirror records nothing"
slot "$T/h0.store" 3 INSTALLED   # h0 is ahead of h1
want "$(code $H set CONFIG3 STATE OK)" 0 "write to a mirror with a lagging member"
want "$($HFS "$T/h0.store" health "$U1")" "lagging=1 sync_failed=0 write_failed=0 missing=0" "the lagging member is counted"
want "$(code $H set CONFIG3 TRIES 2)" 6 "a refused write"
want "$($HFS "$T/h0.store" health "$U1")" "lagging=2 sync_failed=0 write_failed=0 missing=0" "counted again by the next writer"
want "$(code $H get CONFIG3 STATE)" 0 "a read"
want "$($HFS "$T/h0.store" health "$U1")" "lagging=2 sync_failed=0 write_failed=0 missing=0" "a read writes nothing"

# status shows the counters and the member state; a clean store shows none
want "$(code $CTL --store $T/h0.store status)" 0 "status of a store with counters"
$CTL --store "$T/h0.store" status > "$T/st"
has "$T/st" "MIRROR-MEMBER=$U1 STATE=absent LAGGING=2 SYNC_FAILED=0 WRITE_FAILED=0 MISSING=0 LAST=lagging@"
$H status > "$T/st"
has "$T/st" "MIRROR-MEMBER=$U1 STATE=lagging LAGGING=2"
$CTL --store "$T/h1.store" status > "$T/st"
grep -Eq "(LAGGING|SYNC_FAILED|WRITE_FAILED|MISSING)=[1-9]" "$T/st" && fail "status shows a counter that is only in the other member"
# fsck.runway warns about the counters and is clean after a clear
$BUILD/fsck.runway "$T/h0.store" > "$T/fsck"; fcode=$?
want "$fcode" 0 "fsck.runway with counters still exits 0"
has "$T/fsck" "warn: mirror member $U1: lagging 2, sync failed 0, write failed 0, missing 0 (last: lagging at record"
# mirror clear needs a named store and drops the counters from every member
want "$(code $H mirror)" 2 "mirror without clear"
want "$(code $H mirror clear)" 0 "mirror clear"
want "$($HFS "$T/h0.store" health "$U1")" none "the counters are cleared"
want "$($HFS "$T/h1.store" health "$U1")" none "on the other member too"
$H status > "$T/st"
grep -Eq "(LAGGING|SYNC_FAILED|WRITE_FAILED|MISSING)=[1-9]" "$T/st" && fail "status shows counters after a clear"
$BUILD/fsck.runway "$T/h0.store" | grep -q "mirror member" && fail "fsck.runway warns after a clear"

# --- 16. member roster: a writer lists the members, counts one that is gone, and forget stops it -------
mk --uuid $UUID1 "$T/r0.store"; seed "$T/r0.store"
cp "$T/r0.store" "$T/r1.store"
R="$CTL --store $T/r0.store --store $T/r1.store"
want "$(code $R set CONFIG2 STATE OK)" 0 "a writer lists both members"
RU1=$(muuid "$T/r1.store"); [ -n "$RU1" ] || fail "the roster member was not named"
$CTL --store "$T/r0.store" status > "$T/st"
has "$T/st" "MIRROR-MEMBER=$RU1 STATE=absent LAGGING=0 SYNC_FAILED=0 WRITE_FAILED=0 MISSING=0 LAST=none@0"
want "$($HFS "$T/r0.store" health "$RU1")" none "a stable mirror counts nothing"
want "$(code $CTL --store $T/r0.store set CONFIG2 STATE OK)" 0 "a writer on one member"
want "$($HFS "$T/r0.store" health "$RU1")" "lagging=0 sync_failed=0 write_failed=0 missing=1" "the member that is gone is counted"
want "$(code $CTL --store $T/r0.store get CONFIG2 STATE)" 0 "a read"
want "$($HFS "$T/r0.store" health "$RU1")" "lagging=0 sync_failed=0 write_failed=0 missing=1" "a read writes nothing"
want "$(code $R mirror forget 00000000-0000-0000-0000-000000000001)" 2 "forget of a member that is not listed"
want "$(code $R mirror forget)" 2 "forget needs a UUID"
want "$(code $R mirror forget $RU1)" 0 "forget the member"
want "$(code $CTL --store $T/r0.store set CONFIG2 STATE OK)" 0 "a writer after the forget"
$HFS "$T/r0.store" health "$RU1" | grep -q "missing=1\$" || fail "a forgotten member is not counted again"

# --- 17. plaintext loader area: show, set, commit, --wdt, mirror, json -----------------------
echo "fake-loader-binary-1" > "$T/ldr1"
want "$(code $BUILD/mkstore --size 64 --loader "$T/ldr1" "$T/l0.store")" 0 "mkstore with --loader"
want "$(code $CTL --store "$T/l0.store" loader show)" 0 "loader show initial"
has "$T/out" "state record: seq 1, good A, trial none, tries 0, timeout 300s, bad none, reason none, wdt_armed none"
has "$T/out" "copy A: valid, sequence 1"
has "$T/out" "LOADER 21 bytes sha256 $(sha256sum <"$T/ldr1" | cut -d' ' -f1)"
has "$T/out" "copy B: empty"

# JSON format
want "$(code $CTL --store "$T/l0.store" loader show --json)" 0 "loader show --json"
has "$T/out" '"good": "A"'
has "$T/out" '"trial": "none"'
has "$T/out" '"consistent": true'
has "$T/out" '"status": "valid"'

# Commit without a pending trial is refused by policy
want "$(code $CTL --store "$T/l0.store" loader commit)" 6 "loader commit without pending trial"
has "$T/err" "no trial loader copy is pending"

# Stage update with loader set
echo "fake-loader-binary-2-larger" > "$T/ldr2"
want "$(code $CTL --store "$T/l0.store" loader set "$T/ldr2" --timeout 150 --tries 4)" 0 "loader set on trial"
has "$T/out" "loader set on trial (copy B, 4 tries)"
want "$(code $CTL --store "$T/l0.store" loader show)" 0 "loader show after set"
has "$T/out" "state record: seq 2, good A, trial B, tries 4, timeout 150s, bad none, reason none"
has "$T/out" "copy A: valid, sequence 1"
has "$T/out" "copy B: valid, sequence 2"
has "$T/out" "LOADER 28 bytes sha256 $(sha256sum <"$T/ldr2" | cut -d' ' -f1)"

# Commit the trial loader
want "$(code $CTL --store "$T/l0.store" loader commit)" 0 "loader commit"
has "$T/out" "loader committed (copy B is good)"
want "$(code $CTL --store "$T/l0.store" loader show)" 0 "loader show after commit"
has "$T/out" "state record: seq 3, good B, trial none, tries 0, timeout 150s, bad none, reason none"

# Setting another loader now targets copy A
echo "fake-loader-binary-3" > "$T/ldr3"
mkdir -p "$T/wdts"
echo "wdt-driver-alpha" > "$T/wdts/01-alpha.efi"
echo "wdt-driver-beta" > "$T/wdts/02-beta.efi"
want "$(code $CTL --store "$T/l0.store" loader set "$T/ldr3" --wdt "$T/wdts")" 0 "loader set with --wdt"
has "$T/out" "loader set on trial (copy A, 3 tries)"
want "$(code $CTL --store "$T/l0.store" loader show)" 0 "loader show with WDT drivers"
has "$T/out" "state record: seq 4, good B, trial A, tries 3, timeout 300s, bad none, reason none"
has "$T/out" "copy A: valid, sequence 3"
has "$T/out" "LOADER 21 bytes"
has "$T/out" "WDT 17 bytes"
has "$T/out" "WDT 16 bytes"

# Mirror: matching and divergent members
cp "$T/l0.store" "$T/l1.store"
want "$(code $CTL --store "$T/l0.store" --store "$T/l1.store" loader show)" 0 "mirror loader show matches"
has "$T/out" "members hold the same loader state and bundles"

# Tamper with l1.store's copy A bundle header
LSTART=$($CTL --store "$T/l1.store" loader show | sed -n 's/.*loader area 2 x [0-9]* blocks at \([0-9]*\).*/\1/p')
dd if=/dev/zero of="$T/l1.store" bs=4096 seek=$((LSTART + 2)) count=1 conv=notrunc 2>/dev/null
want "$(code $CTL --store "$T/l0.store" --store "$T/l1.store" loader show)" 8 "mirror loader show detects difference"
has "$T/out" "warning: members hold different loader records or bundles"

# Plain store without loader area
mk --uuid $UUID3 "$T/plain.store"
want "$(code $CTL --store "$T/plain.store" loader show)" 0 "loader show without area"
has "$T/out" "no loader area"
want "$(code $CTL --store "$T/plain.store" loader set "$T/ldr1")" 6 "loader set refuses store without area"
has "$T/err" "no loader area"

echo "RWYCTL: PASS"
