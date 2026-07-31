#!/usr/bin/env bash
# Build wgctl: the tiny static netlink helper that creates + configures the
# wireguard netdev without iproute2 or the 220 KB `wg` tool.
#
# Run from anywhere -- the script locates the repo from its own path:
#   bash src/wireguard/kmod/build_wgctl.sh
# Toolchain: a mipsel musl cross-GCC (https://musl.cc -> mipsel-linux-musl-cross,
# or crosstool-NG mipsel-unknown-linux-musl). Override with TOOLCHAIN=/path.
# Result is written to <repo>/bin/wgctl.
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # src/wireguard/kmod
REPO="$(cd "$HERE/../../.." && pwd)"                   # repo root
TOOLCHAIN="${TOOLCHAIN:-$HOME/x-tools/mipsel-linux-musl-cross}"
CC="${CC:-$TOOLCHAIN/bin/mipsel-linux-musl-gcc}"
ST="${STRIP:-$TOOLCHAIN/bin/mipsel-linux-musl-strip}"
READELF="${READELF:-$TOOLCHAIN/bin/mipsel-linux-musl-readelf}"
OUT="$REPO/bin/wgctl"
mkdir -p "$REPO/bin"
"$CC" -Os -static -no-pie -march=mips32r2 -mabi=32 -EL -o "$OUT" "$HERE/wgctl.c"
"$ST" "$OUT"
ls -l "$OUT"; md5sum "$OUT"
"$READELF" -h "$OUT" | grep -E 'Type|Machine'
echo DONE
