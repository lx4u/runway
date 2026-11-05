#!/bin/sh
# demo/fetch.sh — download the pinned Alpine packages (kernel with modules, static busybox) into build/demo
# (sha256 checked) and unpack them there.
set -eu
DIR=${BUILD:-build}/demo
URL=https://dl-cdn.alpinelinux.org/alpine/v3.24/main/x86_64
mkdir -p "$DIR"
while read -r sum name; do
    if ! { [ -f "$DIR/$name" ] && echo "$sum  $DIR/$name" | sha256sum -c --status; }; then
        curl -fsSL --retry 3 -o "$DIR/$name" "$URL/$name"
        echo "$sum  $DIR/$name" | sha256sum -c --status || { echo "demo-fetch: bad sha256 for $name" >&2; rm -f "$DIR/$name"; exit 1; }
    fi
done <<SUMS
02ba9491d4cc110707de64ed03dd7e2fa5c93fdd851979c244ceb018785a9eea linux-lts-6.18.55-r0.apk
b0d6cfc585d7fdd12df359cc5d38d5e45894e9e9e78c4059eb41577d022ec734 busybox-static-1.37.0-r31.apk
SUMS
rm -rf "$DIR/pkg"; mkdir -p "$DIR/pkg"
for p in linux-lts busybox-static; do
    tar -xzif "$DIR/$p"-*.apk -C "$DIR/pkg" 2>&1 | grep -v 'unknown extended header' || true
done
echo "demo-fetch: OK ($DIR)"
