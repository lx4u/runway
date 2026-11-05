#!/bin/sh
# provision.sh runs its dialogs inside $( ): a whiptail call that does not send its screen to
# standard error draws into the capture and never shows. Host only: reads the script.
# SPDX-License-Identifier: Apache-2.0
set -u
fail() { echo "PROVISION WHIPTAIL FAIL: $*" >&2; exit 1; }
SCRIPT=tests/qemu/provision.sh
calls=$(sed -e ':a' -e '/\\$/N' -e 's/\\\n//' -e 'ta' "$SCRIPT" | grep -E 'whiptail[ ]' | grep -v 'command -v')
[ -n "$calls" ] || fail "no whiptail call found"
printf '%s\n' "$calls" | while IFS= read -r line; do
    case "$line" in
    *'>&2'*) ;;
    *) printf '%s\n' "$line" >&2; exit 1 ;;
    esac
done || fail "a whiptail call does not send its screen to standard error"
echo "PROVISION WHIPTAIL OK"
