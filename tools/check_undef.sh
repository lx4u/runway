#!/bin/sh
# Fail when a shared object has undefined symbols; touch $2 when clean.
if nm -D -u "$1" | grep -q ' U '; then
	echo "error: undefined symbols in $1" >&2
	exit 1
fi
: > "$2"
