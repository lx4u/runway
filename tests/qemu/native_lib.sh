# tests/qemu/native_lib.sh — helpers for the native-boot cases of the encryption,
# mirror and multi-volume suites. Source it; the caller defines BUILD and fail().
# SPDX-License-Identifier: Apache-2.0
#   nat_fs STORE CMD ARGS...    one host_fs commit (HOST_FS_PASSPHRASE in the environment opens an
#                               encrypted store)
#   nat_cat STORE PATH          file content from the store
#   nat_seed STORE              EFI/RUNWAY with BLOBS/uki.efi (the fake UKI)
#   nat_slot STORE N ARGS STATE [TRIES]   a slot installed through the finalize protocol
export PATH="$PATH:/sbin:/usr/sbin"
NAT_T=EFI/RUNWAY
NAT_KERNEL=$BUILD/NATIVE_PAYLOAD.EFI
nat_fs() { st=$1; shift; $BUILD/host_fs "$st" "$@" || fail "host_fs $*"; }
nat_cat() { $BUILD/host_fs "$1" cat "$2"; }
nat_seed() {
    nat_fs "$1" mkdir $NAT_T
    nat_fs "$1" mkdir $NAT_T/BLOBS
    nat_fs "$1" put $NAT_T/BLOBS/uki.efi @$NAT_KERNEL
}
nat_slot() {
    nat_fs "$1" mkdir $NAT_T/CONFIG$2
    nat_fs "$1" link $NAT_T/BLOBS/uki.efi $NAT_T/CONFIG$2/KERNEL
    nat_fs "$1" put $NAT_T/CONFIG$2/ARGS "$3"
    [ -n "$5" ] && nat_fs "$1" put $NAT_T/CONFIG$2/TRIES "$5"
    nat_fs "$1" put $NAT_T/CONFIG$2/STATE "INSTALLED
"
    [ "$4" != INSTALLED ] && nat_fs "$1" put $NAT_T/CONFIG$2/STATE "$4
"
    return 0
}
# log checks: expect LOG TEXT / refuse LOG TEXT
nat_expect() { grep -qF -- "$2" "$1" || fail "missing: $2" "$1"; }
nat_refuse() { grep -qF -- "$2" "$1" && fail "unexpected: $2" "$1"; return 0; }
