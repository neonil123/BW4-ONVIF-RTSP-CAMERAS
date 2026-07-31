#!/usr/bin/env bash
# Cross-compile the `wg` config tool (wireguard-tools) for mipsel-musl, static.
#
# Run from anywhere -- the script locates the repo from its own path:
#   bash src/wireguard/build_wgtools.sh
# Toolchain: a mipsel musl cross-GCC (https://musl.cc -> mipsel-linux-musl-cross,
# or crosstool-NG mipsel-unknown-linux-musl). Override with TOOLCHAIN=/path.
# Result is copied to <repo>/bin/wg.
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # src/wireguard
REPO="$(cd "$HERE/../.." && pwd)"                      # repo root
TOOLCHAIN="${TOOLCHAIN:-$HOME/x-tools/mipsel-linux-musl-cross}"
CC="${CC:-$TOOLCHAIN/bin/mipsel-linux-musl-gcc}"
# Build under $HOME, not /tmp: WSL wipes tmpfs when the VM idles down.
WORK="${WORK:-$HOME/wgbuild}"
mkdir -p "$WORK"; cd "$WORK"
VER=1.0.20210914
if [ ! -d wireguard-tools ]; then
  if ! git clone --depth 1 https://git.zx2c4.com/wireguard-tools 2>/dev/null; then
    echo "git clone failed, trying snapshot tarball"
    curl -L -o wgt.tar.xz "https://git.zx2c4.com/wireguard-tools/snapshot/wireguard-tools-$VER.tar.xz"
    tar xf wgt.tar.xz && mv "wireguard-tools-$VER" wireguard-tools
  fi
fi
cd wireguard-tools/src
make clean >/dev/null 2>&1 || true
# Build only the `wg` binary, static musl. Pass CFLAGS/LDFLAGS via ENV (not the
# make command line) so the Makefile's own `+= -idirafter uapi` still applies;
# also force `-I uapi` so the bundled, complete linux/wireguard.h wins.
export CFLAGS="-O2 -march=mips32r2 -mabi=32 -EL -I uapi"
export LDFLAGS="-static"
make wg CC="$CC"
echo "=== result ==="
ls -l wg
"$TOOLCHAIN/bin/mipsel-linux-musl-readelf" -h wg | grep -E 'Machine|Type'
file wg 2>/dev/null || true
md5sum wg
mkdir -p "$REPO/bin"
cp wg "$REPO/bin/wg"
echo "WGTOOLS_DONE"
