#!/usr/bin/env bash
# ============================================================================
# build.sh -- cross-compile the five LD_PRELOAD shims for the QC3
#             (VeePai BW6, Ingenic T23N, MIPS32r2 little-endian, uClibc 0.9.33)
#
# Run from WSL / Linux:
#   bash src/shims/build.sh                 # build all five into src/shims/
#   bash src/shims/build.sh camweb wifi_sd  # build a subset
#   CC=/path/to/mipsel-...-gcc bash src/shims/build.sh
#   OUTDIR=/tmp/out bash src/shims/build.sh
#
# ---------------------------------------------------------------------------
# WHAT THESE OBJECTS ARE (all five facts below were re-confirmed with
# `readelf -h/-d/-S/--dyn-syms` against the shipped binaries in ../../bin):
#
#   ELF32 / 2's complement, little endian / Type: DYN (Shared object file)
#   Machine: MIPS R3000
#   Flags:   0x70001007, noreorder, pic, cpic, o32, mips32r2
#     -> -shared -fPIC -march=mips32r2 -mabi=32 -EL
#
#   FREESTANDING. `readelf -d` on every shipped .so shows ZERO NEEDED entries.
#   The libc functions they call (pthread_create, pthread_detach, sleep,
#   unsetenv, open, read, write, close, rename, socket, sendto, getenv, atoi,
#   __errno_location) are left UNDEFINED in .dynsym and are resolved at load
#   time out of the host process `vp_project`, which already has uClibc 0.9.33
#   (and libpthread) mapped. That is why there is no libc-ABI coupling and why
#   a musl cross-toolchain can legitimately build objects that run against
#   uClibc: no musl code is linked in at all.
#     -> -nostdlib -ffreestanding -fno-stack-protector
#
#   Every .so has a 4-byte .init_array (INIT_ARRAY / INIT_ARRAYSZ 4) holding
#   the single `__attribute__((constructor))` entry point.
#
#   Built -Os: sizes are 2080 .. 5376 bytes.
#
# ---------------------------------------------------------------------------
# BYTE-IDENTICAL REBUILDS ARE NOT A REQUIREMENT -- but you do get them.
# What MUST match is the *shape*: ELF DYN / MIPS o32 mips32r2 / no NEEDED /
# non-empty .init_array / UND symbol set within the expected libc allowlist.
# verify.sh asserts exactly that and nothing more, so any mipsel gcc is fine.
#
# As a bonus, with the gcc 11.2.1 toolchain named below this script does in
# fact reproduce all five shipped bin/*.so bit-for-bit (md5 + size confirmed):
#     camweb.so       2080 B  53c59f013e7d1098edec541e330a16fb
#     wifi_sd.so      4916 B  f3c6c2ab296d5c5cdfbb91fd0d4da53e
#     pir_sleep.so    2600 B  0cf8afb9053a70c59edaa7471331f8e0
#     battery_osd.so  3032 B  119900999bf56f65b1c652a79a0e2a8b
#     mic_capture.so  5376 B  bc49c932a6a84f49a51492c487846d1f
# Do not treat those md5s as a gate -- a different gcc will produce different
# bytes that are equally correct. They are recorded only as provenance.
#
# Note that bin/camweb.so is the RELEASE build of camweb.c: its .rodata holds
# just the 12-byte "LD_PRELOAD" string. The author's earlier dev build (not in
# this repo) additionally carried "/tmp/okabweb.log", "start\n", "poke\n",
# "GOTFD\n", "giveup\n" and UND open/write/close for that file logging, so it
# has a different md5. camweb.c here is the release source and matches bin/.
#
# The shipped .so files also differ from one another in cosmetic link details,
# because they were originally built by four separate per-feature scripts.
# Those differences are preserved in EXTRA_LDFLAGS below so this script stays
# faithful to what actually produced bin/*.so, and are *not* asserted by
# verify.sh because none of them affect loading:
#   battery_osd.so  SONAME present, not stripped
#   mic_capture.so  SONAME present, not stripped
#   pir_sleep.so    SONAME present, not stripped
#   wifi_sd.so      no SONAME, BIND_NOW + FLAGS_1 NOW, not stripped
#   camweb.so       no SONAME, stripped
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTDIR="${OUTDIR:-$HERE}"

# ---------------------------------------------------------------------------
# Toolchain. Same one src/onvif_rtsp/build.sh uses. Override with CC=...
# The shipped objects' .comment reads "GCC: (GNU) 11.2.1 20211120", which is
# the gcc in this musl-cross-make toolchain.
# ---------------------------------------------------------------------------
CC="${CC:-$HOME/x-tools/mipsel-linux-musl-cross/bin/mipsel-linux-musl-gcc}"

if ! command -v "$CC" >/dev/null 2>&1 && [ ! -x "$CC" ]; then
    cat >&2 <<EOF
ERROR: MIPS cross-compiler not found.

  tried: $CC

These shims MUST be cross-compiled for MIPS32r2 little-endian; a host compiler
cannot produce them. Install a mipsel cross-toolchain and either put it on PATH
or point CC at it, e.g.:

  # musl-cross-make prebuilt (what this project used):
  #   https://musl.cc/mipsel-linux-musl-cross.tgz
  mkdir -p ~/x-tools && tar -C ~/x-tools -xzf mipsel-linux-musl-cross.tgz
  CC=~/x-tools/mipsel-linux-musl-cross/bin/mipsel-linux-musl-gcc bash \$0

Any mipsel gcc works -- no libc from the toolchain is linked in (-nostdlib),
so the toolchain's libc flavour is irrelevant to the output.
EOF
    exit 1
fi

# Derive the matching binutils from the gcc name (mipsel-linux-musl-gcc ->
# mipsel-linux-musl-readelf). Fall back to host readelf, which also reads MIPS.
TOOLPREFIX="${CC%gcc}"
READELF="${READELF:-${TOOLPREFIX}readelf}"
if ! command -v "$READELF" >/dev/null 2>&1 && [ ! -x "$READELF" ]; then
    READELF="$(command -v readelf || true)"
fi
export READELF

# ---------------------------------------------------------------------------
# Flags common to all five. Derived from the ELF evidence documented above.
# ---------------------------------------------------------------------------
CFLAGS_COMMON="-shared -fPIC -Os -march=mips32r2 -mabi=32 -EL"
CFLAGS_COMMON="$CFLAGS_COMMON -nostdlib -ffreestanding -fno-stack-protector"
CFLAGS_COMMON="$CFLAGS_COMMON -fno-builtin -Wall -Wextra"

# Per-shim link details, matching how each shipped .so was actually produced.
extra_ldflags_for() {
    case "$1" in
        camweb)      echo "-s" ;;                                      # stripped, no soname
        wifi_sd)     echo "-Wl,--build-id=none -Wl,-z,now" ;;          # BIND_NOW, no soname
        pir_sleep)   echo "-Wl,--no-undefined-version -Wl,-soname,pir_sleep.so" ;;
        battery_osd) echo "-Wl,--hash-style=sysv -Wl,-soname,battery_osd.so" ;;
        mic_capture) echo "-Wl,--hash-style=sysv -Wl,-soname,mic_capture.so" ;;
        *)           echo "" ;;
    esac
}

ALL_SHIMS="camweb wifi_sd pir_sleep battery_osd mic_capture"
SHIMS="${*:-$ALL_SHIMS}"

# Validate requested names before doing any work.
for s in $SHIMS; do
    case " $ALL_SHIMS " in
        *" $s "*) ;;
        *) echo "ERROR: unknown shim '$s' (known: $ALL_SHIMS)" >&2; exit 1 ;;
    esac
    [ -f "$HERE/$s.c" ] || { echo "ERROR: missing source $HERE/$s.c" >&2; exit 1; }
done

mkdir -p "$OUTDIR"

echo "=== toolchain ==="
"$CC" --version | head -1
echo "CC      = $CC"
echo "READELF = $READELF"
echo "OUTDIR  = $OUTDIR"
echo

BUILT=""
for s in $SHIMS; do
    out="$OUTDIR/$s.so"
    # shellcheck disable=SC2046  # word splitting of the flag strings is intended
    echo "--- $s.c -> $s.so"
    "$CC" $CFLAGS_COMMON $(extra_ldflags_for "$s") \
          -I"$HERE" -o "$out" "$HERE/$s.c"
    BUILT="$BUILT $s"
done

echo
ls -l $(for s in $BUILT; do echo "$OUTDIR/$s.so"; done)
command -v md5sum >/dev/null 2>&1 && md5sum $(for s in $BUILT; do echo "$OUTDIR/$s.so"; done)

# ---------------------------------------------------------------------------
# Verification. Hard-fails the build if any object is malformed.
# ---------------------------------------------------------------------------
echo
if [ -f "$HERE/verify.sh" ]; then
    bash "$HERE/verify.sh" $(for s in $BUILT; do echo "$OUTDIR/$s.so"; done)
else
    echo "WARNING: $HERE/verify.sh not found -- outputs were NOT verified." >&2
    exit 1
fi
