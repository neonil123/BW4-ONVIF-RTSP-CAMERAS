#!/bin/bash
# Build okam_onvifd for the O-KAM QC3 (Ingenic T23N, mipsel, uClibc 0.9.33 on
# the device) using the WSL musl MIPSEL cross-toolchain.
#
# Run from WSL:
#   bash /mnt/h/projects/Repos/NeoSystems/O-KamProHack/builds/features/onvif_rtsp/build.sh
#
# Produces a STATIC binary (okam_onvifd). See README.md "Why static, not
# dynamic against uClibc" for why a true dynamic-against-the-device's-uClibc
# build was evaluated and not shipped (no uClibc cross-toolchain is available
# in this dev environment, and a musl-object/uClibc-runtime "frankenlink"
# needs hand-written MIPS o32 CRT startup matching uClibc's
# __libc_start_main ABI -- unverifiable without hardware/emulation access,
# which this task does not have).
set -e
CC=~/x-tools/mipsel-linux-musl-cross/bin/mipsel-linux-musl-gcc
STRIP=~/x-tools/mipsel-linux-musl-cross/bin/mipsel-linux-musl-strip
READELF=~/x-tools/mipsel-linux-musl-cross/bin/mipsel-linux-musl-readelf
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/src"
OUT="$HERE/okam_onvifd"

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
