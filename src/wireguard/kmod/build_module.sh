#!/usr/bin/env bash
# Fix the gcc11-vs-3.10 header gap, finish modules_prepare, build wireguard.ko.
#
# Prerequisites (in order):  clone_kernel.sh  ->  prep_kernel.sh  ->  this script.
# clone_kernel.sh fetches BOTH sources this needs: thingino-linux and
# wireguard-linux-compat. Result is copied to <repo>/bin/wireguard.ko.
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # src/wireguard/kmod
REPO="$(cd "$HERE/../../.." && pwd)"                   # repo root
TOOLCHAIN="${TOOLCHAIN:-$HOME/x-tools/mipsel-linux-musl-cross}"
TC="$TOOLCHAIN/bin"
export PATH="$TC:$PATH"
export ARCH=mips CROSS_COMPILE=mipsel-linux-musl- LOCALVERSION=-Archon
SRCDIR="${SRCDIR:-$HOME/kmod}"
K="$SRCDIR/thingino-linux"; WG="$SRCDIR/wireguard-linux-compat/src"
[ -d "$K" ] || { echo "ERROR: kernel tree missing: $K -- run clone_kernel.sh first" >&2; exit 1; }
[ -d "$WG" ] || { echo "ERROR: wireguard-linux-compat missing: $WG -- run clone_kernel.sh first" >&2; exit 1; }
cd "$K"

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
# wg_compat_fix.h is force-included ONLY for the module objects (not for
# modules_prepare above, which must build the kernel's own host tools clean).
make -j"$(nproc)" -C "$K" M="$WG" \
  KCFLAGS="$KCFLAGS -include $HERE/wg_compat_fix.h" modules 2>&1 | tail -35
BIN="$WG/wireguard.ko"
if [ -f "$BIN" ]; then
  echo "=== wireguard.ko built ==="
  ls -l "$BIN"
  "$TC/mipsel-linux-musl-strip" --strip-debug "$BIN" -o /tmp/wireguard.ko
  ls -l /tmp/wireguard.ko
  "$TC/mipsel-linux-musl-readelf" -p .modinfo /tmp/wireguard.ko | grep -iE 'vermagic|depends|name'
  mkdir -p "$REPO/bin"
  cp /tmp/wireguard.ko "$REPO/bin/wireguard.ko"
  md5sum /tmp/wireguard.ko
  echo "KO_OK"
else
  echo "KO_BUILD_FAILED"
fi
