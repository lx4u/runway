#!/bin/sh
# Crypto backends (OpenSSL and built-in) and the seeded random source. Host only.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
T=$(mktemp -d /var/tmp/rwy-cryptobe.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail() { echo "CRYPTOBE FAIL: $*" >&2; exit 1; }
want() { [ "$1" = "$2" ] || fail "$3: got '$1', want '$2'"; }   # got want what
code() { "$@" >"$T/out" 2>"$T/err" </dev/null; echo $?; }
has() { grep -qF -- "$2" "$1" || { cat "$1" >&2; fail "missing '$2' in $1"; }; }

MK=$BUILD/mkstore
CK=$BUILD/fsck.runway
HF=$BUILD/host_fs
U=11111111-2222-4333-8444-555555555555
OPTS="--size 64 --encrypt --random-seed s1 --uuid $U"

# Both backends make the same encrypted image, byte for byte
want "$(code $MK $OPTS "$T/o.store")" 0 "mkstore with OpenSSL"
want "$(RWY_CRYPTO=builtin code $MK $OPTS "$T/b.store")" 0 "mkstore with the built-in code"
cmp -s "$T/o.store" "$T/b.store" || fail "the two backends made different images"

# What one backend writes the other reads, in both directions
RWY_CRYPTO=builtin HOST_FS_PASSPHRASE=runway-test-passphrase $HF "$T/o.store" put A.TXT "from builtin" || fail "write with built-in"
want "$(HOST_FS_PASSPHRASE=runway-test-passphrase $HF "$T/o.store" cat A.TXT)" "from builtin" "OpenSSL reads built-in data"
HOST_FS_PASSPHRASE=runway-test-passphrase $HF "$T/o.store" put B.TXT "from openssl" || fail "write with OpenSSL"
want "$(RWY_CRYPTO=builtin HOST_FS_PASSPHRASE=runway-test-passphrase $HF "$T/o.store" cat B.TXT)" "from openssl" "built-in reads OpenSSL data"
want "$(RWY_CRYPTO=builtin code $CK --key runway-test-passphrase "$T/o.store")" 0 "check with the built-in code"
want "$(code $CK --key runway-test-passphrase "$T/o.store")" 0 "check with OpenSSL"

# rwyctl reads the same store with either backend
CTL="$BUILD/rwyctl --store $T/o.store --passphrase-file $T/pass"
echo runway-test-passphrase >"$T/pass"
want "$(code $CTL stores)" 0 "rwyctl stores with OpenSSL"
want "$(RWY_CRYPTO=builtin code $CTL stores)" 0 "rwyctl stores with the built-in code"

# Random source: the OS by default, a seed on request
mk() { code $MK --size 64 "$@"; }
want "$(mk "$T/r1.store")" 0 "unseeded store 1"
want "$(mk "$T/r2.store")" 0 "unseeded store 2"
cmp -s "$T/r1.store" "$T/r2.store" && fail "two unseeded stores have the same UUID"
want "$(mk --random-seed x "$T/s1.store")" 0 "seeded store 1"
has "$T/err" "random source seeded"
sleep 1   # The clock moves on: a seeded image must not depend on it
want "$(mk --random-seed x "$T/s2.store")" 0 "seeded store 2"
cmp -s "$T/s1.store" "$T/s2.store" || fail "the same seed made different images"
want "$(mk --random-seed y "$T/s3.store")" 0 "seeded store 3"
cmp -s "$T/s1.store" "$T/s3.store" && fail "another seed made the same image"
want "$(RWY_CRYPTO=builtin mk "$T/n.store")" 2 "built-in code has no random source"
has "$T/err" "no random source"
want "$(RWY_CRYPTO=builtin mk --random-seed x "$T/s4.store")" 0 "built-in code with a seed"
cmp -s "$T/s1.store" "$T/s4.store" || fail "the seed gave other bytes with the built-in code"

# The data key and the keyslot salt come from the random source, never from the passphrase
want "$(mk --encrypt --uuid $U "$T/k1.store")" 0 "unseeded encrypted store 1"
want "$(mk --encrypt --uuid $U "$T/k2.store")" 0 "unseeded encrypted store 2"
cmp -s -n 8192 "$T/k1.store" "$T/k2.store" && fail "two encrypted stores have the same keyslot header"
want "$(RWY_CRYPTO=builtin mk --encrypt --uuid $U "$T/k3.store")" 2 "encrypted store needs a random source"
has "$T/err" "no random source"
want "$(mk --encrypt --uuid $U --dek-out "$T/k.dek" "$T/k4.store")" 0 "key written for the caller"
[ "$(wc -c <"$T/k.dek")" = 32 ] || fail "the key file is not 32 bytes"
# A caller-provided data key gives the same key back and can be reused for another image
printf '0123456789abcdef0123456789abcdef' > "$T/given.dek"
want "$(mk --encrypt --uuid $U --dek-in "$T/given.dek" --dek-out "$T/given.out" "$T/k5.store")" 0 "key supplied by the caller"
cmp -s "$T/given.dek" "$T/given.out" || fail "mkstore did not use the supplied key"
want "$(mk --encrypt --uuid $U --dek-in "$T/given.out" "$T/k6.store")" 0 "the same key makes a second store"
printf 'short' > "$T/bad.dek"
want "$(mk --encrypt --uuid $U --dek-in "$T/bad.dek" "$T/k7.store")" 2 "a short key is refused"
has "$T/err" "32 bytes"

echo "cryptobe: PASS"
