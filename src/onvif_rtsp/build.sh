#!/bin/bash
# Build cam_onvifd for the BW4 (Ingenic T23N, mipsel, uClibc 0.9.33 on
# the device) using a musl MIPSEL cross-toolchain.
#
# Run from the repo root (or anywhere -- the script locates itself):
#   bash src/onvif_rtsp/build.sh
# On Windows, run it inside WSL.
#
# Produces a STATIC binary (cam_onvifd). See README.md "Why static, not
# dynamic against uClibc" for why a true dynamic-against-the-device's-uClibc
# build was evaluated and not shipped (no uClibc cross-toolchain is available
# in this dev environment, and a musl-object/uClibc-runtime "frankenlink"
# needs hand-written MIPS o32 CRT startup matching uClibc's
# __libc_start_main ABI -- unverifiable without hardware/emulation access,
# which this task does not have).
set -e
# Cross-toolchain: a mipsel little-endian musl GCC. Two easy sources:
#   * https://musl.cc/  -> mipsel-linux-musl-cross.tgz (prebuilt, unpack anywhere)
#   * crosstool-NG      -> ./ct-ng mipsel-unknown-linux-musl && ./ct-ng build
# Default location is $TOOLCHAIN (an unpacked musl.cc tarball in $HOME/x-tools).
# Override any of these from the environment, e.g.:
#   TOOLCHAIN=/opt/mipsel-linux-musl-cross bash src/onvif_rtsp/build.sh
#   CC=mipsel-linux-musl-gcc STRIP=... READELF=... bash src/onvif_rtsp/build.sh
# No specific toolchain version is required; anything targeting mips32r2/o32 EL
# with a static-capable musl works.
TOOLCHAIN="${TOOLCHAIN:-$HOME/x-tools/mipsel-linux-musl-cross}"
CC="${CC:-$TOOLCHAIN/bin/mipsel-linux-musl-gcc}"
STRIP="${STRIP:-$TOOLCHAIN/bin/mipsel-linux-musl-strip}"
READELF="${READELF:-$TOOLCHAIN/bin/mipsel-linux-musl-readelf}"
command -v "$CC" >/dev/null 2>&1 || [ -x "$CC" ] || {
  echo "ERROR: cross compiler not found: $CC" >&2
  echo "Install a mipsel-linux-musl toolchain (see musl.cc) and set TOOLCHAIN= or CC=." >&2
  exit 1
}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/src"
OUT="$HERE/cam_onvifd"

CFLAGS="-Os -Wall -Wextra -std=gnu99 -static -no-pie -fno-pie -march=mips32r2 -mabi=32 -EL -D_GNU_SOURCE -pthread"

echo "=== compiling ($CC) ==="
"$CC" $CFLAGS -o "$OUT" "$SRC"/*.c -lpthread
cp "$OUT" "$OUT.unstripped"
"$STRIP" "$OUT"

echo
echo "=== built: $OUT ==="
ls -l "$OUT" "$OUT.unstripped"
md5sum "$OUT" "$OUT.unstripped" 2>/dev/null || md5 "$OUT" "$OUT.unstripped"

echo
echo "=== ELF type / arch (expect: EXEC, MIPS R3000 == mips32, 2's complement LE) ==="
"$READELF" -h "$OUT" | grep -E "Type|Machine|Data"

echo
echo "=== dynamic section (expect: none -- static binary) ==="
"$READELF" -d "$OUT" 2>&1 | head -3
