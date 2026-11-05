#!/bin/sh
# Link the given files into one directory, so the scripts find them under $BUILD.
# SPDX-License-Identifier: Apache-2.0
# Stores are copied, because the scripts write to them. With -d, RUNWAY_DEBUG.EFI
# takes the place of RUNWAY.EFI.
debug=
[ "$1" = -d ] && { debug=1; shift; }
dir=$1 stamp=$2
shift 2
mkdir -p "$dir"
for f in "$@"; do
	name=$(basename "$f")
	if [ -n "$debug" ]; then
		[ "$name" = rwy-loader.efi ] && continue
		[ "$name" = RUNWAY_DEBUG.EFI ] && {
			ln -sf "$(realpath "$f")" "$dir/rwy-loader.efi"
			ln -sf "$(realpath "$f")" "$dir/RUNWAY_DEBUG.EFI"
			continue
		}
	fi
	case $name in
	*.STORE|*.DAT) cp --sparse=always "$f" "$dir/$name" ;;
	*) ln -sf "$(realpath "$f")" "$dir/$name" ;;
	esac
done
: > "$stamp"
