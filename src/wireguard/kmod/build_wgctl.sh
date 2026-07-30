#!/usr/bin/env bash
set -e
CC="$HOME/x-tools/mipsel-linux-musl-cross/bin/mipsel-linux-musl-gcc"
ST="$HOME/x-tools/mipsel-linux-musl-cross/bin/mipsel-linux-musl-strip"
D=$REPO/src/wireguard/kmod
"$CC" -Os -static -no-pie -march=mips32r2 -mabi=32 -EL -o "$D/wgctl" "$D/wgctl.c"
"$ST" "$D/wgctl"
ls -l "$D/wgctl"; md5sum "$D/wgctl"
"$HOME/x-tools/mipsel-linux-musl-cross/bin/mipsel-linux-musl-readelf" -h "$D/wgctl" | grep -E 'Type|Machine'
echo DONE
