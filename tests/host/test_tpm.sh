#!/bin/sh
# TPM command builders (rwy_tpm_cmd.c) against swtpm on a unix socket. Skips without swtpm.
# SPDX-License-Identifier: Apache-2.0
set -u
BUILD=${BUILD:-build}
command -v swtpm >/dev/null || { echo "TPM host test: SKIP (no swtpm)"; exit 0; }
T=$(mktemp -d /var/tmp/rwy-tpm.XXXXXX)
trap 'kill $PID 2>/dev/null; rm -rf "$T"' EXIT
swtpm socket --tpmstate dir="$T" --server type=unixio,path="$T/sock" --ctrl type=unixio,path="$T/ctrl" \
      --tpm2 --flags not-need-init,startup-clear >"$T/log" 2>&1 &
PID=$!
for i in $(seq 1 50); do [ -S "$T/sock" ] && break; sleep 0.1; done
[ -S "$T/sock" ] || { cat "$T/log"; echo "TPM host test: swtpm did not start"; exit 1; }
$BUILD/tpmtest "$T/sock"
