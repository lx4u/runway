#!/bin/sh
# Recovery area: mkstore --recovery-size and --recovery, and the fsck.runway report. Host only.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
T=$(mktemp -d /var/tmp/rwy-recarea.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail() { echo "RECAREA FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }   # got want what
code() { "$@" >"$T/out" 2>"$T/err" </dev/null; echo $?; }
has() { grep -qF -- "$2" "$1" || { cat "$1" >&2; fail "missing '$2' in $1"; }; }

MK=$BUILD/mkstore
CK=$BUILD/fsck.runway

want "$(code $MK --size 64 --encrypt --recovery-size 2 "$T/a.store")" 0 "encrypted store with an area"
want "$(code $CK --key runway-test-passphrase "$T/a.store")" 0 "check of a store with an area"
has "$T/out" "recovery area: 2 x 512 blocks at "
has "$T/out" "check: OK"

want "$(code $MK --size 64 --encrypt "$T/b.store")" 0 "encrypted store without an area"
want "$(code $CK --key runway-test-passphrase "$T/b.store")" 0 "check without an area"
has "$T/out" "recovery area: none"

want "$(code $MK --size 64 --recovery-size 2 "$T/c.store")" 0 "an unencrypted store accepts an area"
want "$(code $CK "$T/c.store")" 0 "check of an unencrypted store with an area"
has "$T/out" "recovery area: 2 x 512 blocks at "
has "$T/out" "check: OK"
want "$(code $MK --size 64 --encrypt --recovery-size 17 "$T/c.store")" 2 "both copies over half the store"
has "$T/err" "half the store"

# Writes never touch the area; the store stays clean.
want "$(code $MK --size 64 --encrypt --recovery-size 2 "$T/e.store")" 0 "seeded store with an area"
for i in 1 2 3; do
    HOST_FS_PASSPHRASE=runway-test-passphrase $BUILD/host_fs "$T/e.store" put F$i.TXT "file $i" || fail "write F$i"
done
want "$(code $CK --key runway-test-passphrase "$T/e.store")" 0 "check after writes"
has "$T/out" "check: OK"
want "$(HOST_FS_PASSPHRASE=runway-test-passphrase $BUILD/host_fs "$T/e.store" cat F2.TXT)" "file 2" "read back"
# --recovery fills copy A in the clear; the size follows the bundle (50% headroom) unless given.
head -c 9000 /dev/urandom >"$T/kernel"
head -c 5000 /dev/urandom >"$T/initrd"
printf 'console=ttyS0 recovery=1' >"$T/args"
want "$(code $MK --size 64 --encrypt --recovery "$T/kernel" --initrd "$T/initrd" --args @"$T/args" "$T/r.store")" 0 "store with a bundle"
has "$T/out" "recovery area: 2 x 10 blocks (bundle 7 blocks)"
want "$(code $CK --key runway-test-passphrase "$T/r.store")" 0 "check of a store with a bundle"
has "$T/out" "flight area: 2 x 1 blocks at 76"
has "$T/out" "journal area: 2 x 32 blocks at 78"
has "$T/out" "recovery area: 2 x 10 blocks at 142"
has "$T/out" "check: OK"
START=$(sed -n 's/^recovery area: 2 x [0-9]* blocks at \([0-9]*\)$/\1/p' "$T/out")
dd if="$T/r.store" of="$T/kernel.out" bs=4096 skip=$((START + 1)) count=3 2>/dev/null
head -c 9000 "$T/kernel.out" | cmp - "$T/kernel" || fail "kernel in the area is not the plaintext file"
dd if="$T/r.store" of="$T/copyb" bs=4096 skip=$((START + 10)) count=1 2>/dev/null
want "$(tr -d '\0' <"$T/copyb" | wc -c)" 0 "copy B stays empty"
want "$(code $MK --size 64 --encrypt --recovery "$T/kernel" --recovery-size 1 "$T/r2.store")" 0 "a given size"
has "$T/out" "wrote "
want "$(code $MK --size 64 --encrypt --recovery "$T/kernel" --args "x" "$T/r3.store")" 0 "ARGS as text"

want "$(code $MK --size 64 --recovery "$T/kernel" "$T/u.store")" 0 "a bundle on an unencrypted store"
want "$(code $CK "$T/u.store")" 0 "check of an unencrypted store with a bundle"
has "$T/out" "recovery area: 2 x 6 blocks"
has "$T/out" "check: OK"
want "$(code $MK --size 64 --encrypt --initrd "$T/initrd" "$T/c.store")" 2 "an initrd without a kernel"
has "$T/err" "need --recovery"
head -c 4097 /dev/zero >"$T/bigargs"
want "$(code $MK --size 64 --encrypt --recovery "$T/kernel" --args @"$T/bigargs" "$T/c.store")" 2 "ARGS over 4 KiB"
has "$T/err" "not valid"
head -c 2097152 /dev/zero >"$T/big"
want "$(code $MK --size 64 --encrypt --recovery "$T/big" --recovery-size 1 "$T/c.store")" 2 "a bundle larger than the given size"
has "$T/err" "smaller than the bundle"
head -c 12582912 /dev/zero >"$T/huge"
want "$(code $MK --size 64 --encrypt --recovery "$T/huge" "$T/c.store")" 2 "a bundle over half the store"
has "$T/err" "half the store"
# --- rwyctl recovery area set / show: a locked store, no key, no TPM ---------------------------
CTL=$BUILD/rwyctl
AREA="recovery area set"
want "$(code $MK --size 64 --encrypt --recovery-size 2 "$T/s.store")" 0 "store for rwyctl"
want "$(code $CTL --store "$T/s.store" recovery area show)" 8 "show of an empty area"
has "$T/out" "copy A: empty"
has "$T/out" "copy B: empty"
has "$T/out" "no valid bundle"
cp "$T/s.store" "$T/s0.store"
want "$(code $CTL --store "$T/s.store" recovery area show)" 8 "show again"
cmp -s "$T/s.store" "$T/s0.store" || fail "show wrote to the store"

want "$(code $CTL --store "$T/s.store" recovery area set --kernel "$T/kernel" --initrd "$T/initrd" --args "console=ttyS0")" 0 "set on a locked store"
want "$(code $CTL --store "$T/s.store" recovery area show)" 0 "show after set"
has "$T/out" "copy A: valid, sequence 1"
has "$T/out" "copy B: empty"
has "$T/out" "using copy A"
has "$T/out" "KERNEL 9000 bytes sha256 $(sha256sum <"$T/kernel" | cut -d' ' -f1)"
has "$T/out" "INITRD 5000 bytes"
has "$T/out" "ARGS 13 bytes"
want "$(code $CK --key runway-test-passphrase "$T/s.store")" 0 "check after set"
has "$T/out" "check: OK"
want "$(HOST_FS_PASSPHRASE=runway-test-passphrase $BUILD/host_fs "$T/s.store" ls / 2>&1 | wc -l)" "$(HOST_FS_PASSPHRASE=runway-test-passphrase $BUILD/host_fs "$T/s0.store" ls / 2>&1 | wc -l)" "the tree is untouched"

# A second set goes to the other copy; the first one stays until the new header is written.
head -c 6000 /dev/urandom >"$T/kernel2"
want "$(code $CTL --store "$T/s.store" recovery area set --kernel "$T/kernel2")" 0 "second set"
want "$(code $CTL --store "$T/s.store" recovery area show)" 0 "show after the second set"
has "$T/out" "copy A: valid, sequence 1"
has "$T/out" "copy B: valid, sequence 2"
has "$T/out" "using copy B"
has "$T/out" "KERNEL 6000 bytes"

# A bundle that does not fit, or is not valid, writes nothing.
cp "$T/s.store" "$T/s1.store"
want "$(code $CTL --store "$T/s.store" recovery area set --kernel "$T/big")" 7 "a bundle larger than a copy"
want "$(code $CTL --store "$T/s.store" recovery area set --kernel "$T/kernel" --args @"$T/bigargs")" 2 "ARGS over 4 KiB"
want "$(code $CTL --store "$T/s.store" recovery area set --initrd "$T/initrd")" 2 "no kernel"
: >"$T/empty"
want "$(code $CTL --store "$T/s.store" recovery area set --kernel "$T/empty")" 2 "an empty part"
want "$(code $CTL --store "$T/s.store" recovery area set --kernel "$T/nofile")" 2 "a missing file"
want "$(code $CTL --store "$T/s.store" recovery area set)" 2 "no options"
want "$(code $CTL --store "$T/s.store" recovery area bogus)" 2 "an unknown action"
cmp -s "$T/s.store" "$T/s1.store" || fail "a refused bundle wrote to the store"

# No area to write.
want "$(code $CTL --store "$T/b.store" recovery area set --kernel "$T/kernel")" 6 "an encrypted store without an area"
has "$T/err" "no recovery area"
want "$(code $CTL --store "$T/b.store" recovery area show)" 0 "show without an area"
has "$T/out" "no recovery area"
mk_plain() { $MK --size 64 "$1" >/dev/null || fail "mkstore plain"; }
mk_plain "$T/p.store"
want "$(code $CTL --store "$T/p.store" recovery area set --kernel "$T/kernel")" 6 "an unencrypted store without an area"

# A mirror: every member is written, and members that differ are reported.
U=11111111-2222-3333-4444-555555555555
$MK --size 64 --encrypt --recovery-size 2 --uuid $U --recovery "$T/kernel" "$T/m1.store" >/dev/null || fail "mirror member 1"
$MK --size 64 --encrypt --recovery-size 2 --uuid $U --recovery "$T/kernel2" "$T/m2.store" >/dev/null || fail "mirror member 2"
want "$(code $CTL --store "$T/m1.store" --store "$T/m2.store" recovery area show)" 8 "members that differ"
has "$T/out" "members hold different bundles"
want "$(code $CTL --store "$T/m1.store" --store "$T/m2.store" recovery area set --kernel "$T/kernel2" --initrd "$T/initrd")" 0 "set on a mirror"
want "$(code $CTL --store "$T/m1.store" --store "$T/m2.store" recovery area show)" 0 "members that agree"
has "$T/out" "members hold the same bundle"
for m in m1 m2; do
    want "$(code $CTL --store "$T/$m.store" recovery area show)" 0 "show of $m"
    has "$T/out" "INITRD 5000 bytes"
done

# A member that cannot be written: the others are written, the command says degraded and fails.
if [ "$(id -u)" != 0 ]; then
    cp "$T/m1.store" "$T/d1.store"
    cp "$T/m2.store" "$T/d2.store"
    chmod 444 "$T/d2.store"
    want "$(code $CTL --store "$T/d1.store" --store "$T/d2.store" recovery area set --kernel "$T/kernel")" 1 "a member that cannot be written"
    has "$T/err" "degraded"
    want "$(code $CTL --store "$T/d1.store" recovery area show)" 0 "the writable member was written"
    has "$T/out" "KERNEL 9000 bytes"
    want "$(code $CTL --store "$T/d2.store" recovery area show)" 0 "the other member is as before"
    has "$T/out" "KERNEL 6000 bytes"
    chmod 644 "$T/d2.store"
fi

# A store held by another process is busy; nothing is written.
if command -v flock >/dev/null 2>&1; then
    cp "$T/m1.store" "$T/l.store"
    flock -x "$T/l.store" sleep 5 &
    sleep 1
    want "$(code $CTL --store "$T/l.store" recovery area set --kernel "$T/kernel")" 5 "a busy store"
    kill $! 2>/dev/null; wait 2>/dev/null
    want "$(code $CTL --store "$T/l.store" recovery area show)" 0 "show while it was busy"
fi
# --- fsck.runway: the payload of each copy, with and without the key, and the members -------------
want "$(code $MK --size 64 --encrypt --recovery-size 1 "$T/k.store")" 0 "store for the check"
want "$(code $CK "$T/k.store")" 0 "check of an empty reservation, no key"
has "$T/out" "note: recovery area is empty"
has "$T/out" "check: OK (0 errors, 0 warnings)"
want "$(code $CTL --store "$T/k.store" recovery area set --kernel "$T/kernel")" 0 "first bundle"
want "$(code $CK "$T/k.store")" 0 "check after one bundle, no key"
has "$T/out" "recovery area: copy A: valid, sequence 1"
has "$T/out" "recovery area: copy B: empty"
has "$T/out" "check: OK (0 errors, 0 warnings)"
want "$(code $CTL --store "$T/k.store" recovery area set --kernel "$T/kernel2")" 0 "second bundle"
want "$(code $CK --key runway-test-passphrase "$T/k.store")" 0 "check with the key"
has "$T/out" "recovery area: copy A: valid, sequence 1"
has "$T/out" "recovery area: copy B: valid, sequence 2"
has "$T/out" "check: OK"
START=$(sed -n 's/^recovery area: 2 x [0-9]* blocks at \([0-9]*\)$/\1/p' "$T/out")
flip() { printf '\377' | dd of="$1" bs=1 seek=$(($2 * 4096 + 5)) conv=notrunc 2>/dev/null; }   # flip: FILE BLOCK
# The newest copy is damaged in its payload: the older one is used and the check warns.
flip "$T/k.store" $((START + 256 + 1))
want "$(code $CK "$T/k.store")" 0 "a bad newest copy, no key"
has "$T/out" "warn: recovery area copy B is bad hash, copy A is used"
has "$T/out" "check: OK (0 errors, 1 warnings)"
want "$(code $CK --key runway-test-passphrase "$T/k.store")" 0 "a bad newest copy, with the key"
has "$T/out" "warn: recovery area copy B is bad hash, copy A is used"
# Both copies are damaged: the device could not recover, and that is an error.
flip "$T/k.store" $((START + 1))
want "$(code $CK "$T/k.store")" 4 "no valid copy, no key"
has "$T/out" "ERROR: recovery area has no valid bundle (copy A: bad hash, copy B: bad hash)"
want "$(code $CK --key runway-test-passphrase "$T/k.store")" 4 "no valid copy, with the key"
has "$T/out" "ERROR: recovery area has no valid bundle"

# Members: another store of the mirror is compared; a difference warns.
$MK --size 64 --encrypt --recovery-size 1 --uuid $U --recovery "$T/kernel" "$T/x1.store" >/dev/null || fail "member x1"
$MK --size 64 --encrypt --recovery-size 1 --uuid $U --recovery "$T/kernel2" "$T/x2.store" >/dev/null || fail "member x2"
want "$(code $CK --member "$T/x2.store" "$T/x1.store")" 0 "members that differ"
has "$T/out" "warn: recovery area differs from the one of $T/x2.store"
has "$T/out" "(0 errors, 1 warnings)"
want "$(code $CK --member "$T/x1.store" "$T/x1.store")" 0 "members that agree"
has "$T/out" "recovery area: same bundle as $T/x1.store"
want "$(code $CK --member "$T/nofile" "$T/x1.store")" 8 "a member that cannot be opened"

# Flight and journal area defaults and limits
want "$(code $MK --size 64 "$T/plain.store")" 0 "plain store defaults"
want "$(code $CK "$T/plain.store")" 0 "check plain store"
has "$T/out" "flight area: 2 x 1 blocks at 76"
has "$T/out" "journal area: 2 x 32 blocks at 78"
want "$(code $MK --size 64 --flight-records 0 --journal-records 0 "$T/zero.store")" 0 "zero records"
want "$(code $CK "$T/zero.store")" 0 "check zero records"
has "$T/out" "flight area: none"
has "$T/out" "journal area: none"
want "$(code $MK --size 64 --flight-records 1000000 "$T/badf.store")" 2 "flight records over limit"
has "$T/err" "area exceeds a quarter of the store"
want "$(code $MK --size 64 --journal-records 1000000 "$T/badj.store")" 2 "journal records over limit"
has "$T/err" "area exceeds a quarter of the store"

echo "recarea: PASS"
