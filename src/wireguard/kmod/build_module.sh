#!/usr/bin/env bash
# Fix the gcc11-vs-3.10 header gap, finish modules_prepare, build wireguard.ko.
set -e
TC="$HOME/x-tools/mipsel-linux-musl-cross/bin"
export PATH="$TC:$PATH"
export ARCH=mips CROSS_COMPILE=mipsel-linux-musl- LOCALVERSION=-Archon
K="$HOME/kmod/thingino-linux"; WG="$HOME/kmod/wireguard-linux-compat/src"; cd "$K"

echo "=== existing compiler-gccN.h ==="; ls include/linux/compiler-gcc*.h
HI=$(ls include/linux/compiler-gcc*.h | grep -oE 'gcc[0-9]+' | grep -oE '[0-9]+' | sort -n | tail -1)
echo "highest present: gcc$HI"
for v in 6 7 8 9 10 11 12; do
  [ -f "include/linux/compiler-gcc$v.h" ] || cp "include/linux/compiler-gcc$HI.h" "include/linux/compiler-gcc$v.h"
done
echo "created compiler-gcc{6..12}.h from gcc$HI"

# relax new-gcc warnings that 3.10 code trips
export KCFLAGS="-Wno-error -Wno-attribute-alias -Wno-address-of-packed-member -Wno-array-bounds -Wno-stringop-overflow -Wno-stringop-truncation -Wno-misleading-indentation -Wno-maybe-uninitialized -Wno-unused-but-set-variable"

echo "=== modules_prepare ==="
make -j"$(nproc)" modules_prepare 2>&1 | tail -15
ls scripts/mod/modpost >/dev/null 2>&1 && echo "MODPOST_OK" || { echo "MODPOST_MISSING - stop"; exit 1; }

echo "=== build wireguard.ko ==="
make -j"$(nproc)" -C "$K" M="$WG" modules 2>&1 | tail -35
BIN="$WG/wireguard.ko"
if [ -f "$BIN" ]; then
  echo "=== wireguard.ko built ==="
  ls -l "$BIN"
  "$TC/mipsel-linux-musl-strip" --strip-debug "$BIN" -o /tmp/wireguard.ko
  ls -l /tmp/wireguard.ko
  "$TC/mipsel-linux-musl-readelf" -p .modinfo /tmp/wireguard.ko | grep -iE 'vermagic|depends|name'
  cp /tmp/wireguard.ko $REPO/src/wireguard/kmod/wireguard.ko
  md5sum /tmp/wireguard.ko
  echo "KO_OK"
else
  echo "KO_BUILD_FAILED"
fi
