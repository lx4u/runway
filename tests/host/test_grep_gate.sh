#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

# Grep gate: verify zero occurrences of the legacy name across tracked files.

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "GREP GATE: skip (not inside a git repository)"
    exit 0
fi

T="tfat"

matches=$(git grep -n -i "$T" -- \
    ':!tests/host/test_grep_gate.sh' || true)

if [ -n "$matches" ]; then
    echo "GREP GATE FAIL: found legacy name occurrences in tracked files:" >&2
    echo "$matches" >&2
    exit 1
fi

echo "GREP GATE: PASS (zero legacy name occurrences found)"
