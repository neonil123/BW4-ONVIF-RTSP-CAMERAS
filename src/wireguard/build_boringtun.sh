#!/usr/bin/env bash
# Cross-compile boringtun-cli for mipsel-unknown-linux-musl (tier-3 -> build-std).
# Build in $HOME (persistent) NOT /tmp (WSL wipes tmpfs when the VM idles down).
#
# Run from anywhere -- the script locates the repo from its own path:
#   bash src/wireguard/build_boringtun.sh
# Toolchain: a mipsel musl cross-GCC (https://musl.cc -> mipsel-linux-musl-cross,
# or crosstool-NG mipsel-unknown-linux-musl). Override with TOOLCHAIN=/path.
# Result is copied to <repo>/bin/boringtun-cli.
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # src/wireguard
REPO="$(cd "$HERE/../.." && pwd)"                      # repo root
. "$HOME/.cargo/env"
TOOLCHAIN="${TOOLCHAIN:-$HOME/x-tools/mipsel-linux-musl-cross}"
TC="$TOOLCHAIN/bin"
export PATH="$TC:$PATH"
export CARGO_TARGET_MIPSEL_UNKNOWN_LINUX_MUSL_LINKER="mipsel-linux-musl-gcc"
export CC_mipsel_unknown_linux_musl="mipsel-linux-musl-gcc"
export AR_mipsel_unknown_linux_musl="mipsel-linux-musl-ar"
# Device is uClibc with NO musl loader -> MUST be fully static + non-PIE (else
# the ELF requests /lib/ld-musl-mipsel.so.1 and exec fails with "not found").
# boringtun uses portable-atomic(fallback) so no libatomic needed.
# Static via the EXTERNAL musl gcc (which links -static fine, as `wg` proved):
#  - link-self-contained=no       -> gcc supplies crt (tier-3 has no self-contained crt)
#  - panic=immediate-abort        -> removes ALL unwinding, so no external -lunwind
#    (this toolchain has no libunwind; plain panic=abort still links the unwind crate)
# std hard-references -lunwind even with immediate-abort; since immediate-abort
# removes all unwind CODE, an empty libunwind.a satisfies the linker with nothing
# left to resolve. (Escalate to build-std-features=llvm-libunwind if syms appear.)
STUB="$HOME/btbuild/stublib"; mkdir -p "$STUB"
# Satisfy -lunwind with the toolchain's libgcc_eh.a (provides _Unwind_* symbols),
# so both immediate-abort (dummy ref) and panic=abort (real refs) link.
LGE="$(find "$TOOLCHAIN" -name 'libgcc_eh.a' 2>/dev/null | head -1)"
if [ -n "$LGE" ]; then cp "$LGE" "$STUB/libunwind.a"; else "$TC/mipsel-linux-musl-ar" rcs "$STUB/libunwind.a"; fi
# NOTE: panic=abort (diagnostic) prints the panic message before aborting;
# switch back to `immediate-abort` for the smaller production binary once stable.
export RUSTFLAGS="-C target-feature=+crt-static -C link-self-contained=no -C link-arg=-static -C link-arg=-no-pie -C panic=abort -C link-arg=-L$STUB"

WORK="$HOME/btbuild"
mkdir -p "$WORK"; cd "$WORK"
[ -d boringtun ] || git clone --depth 1 https://github.com/cloudflare/boringtun.git
cd boringtun

# --- PATCH: boringtun hardcodes the generic-Linux TUNSETIFF (0x400454ca), but
# MIPS uses BSD-style ioctl direction bits, so TUNSETIFF is 0x800454ca there.
# Wrong value -> kernel returns EBADFD and the tunnel never comes up. Make it
# arch-aware. Idempotent (guarded on the cfg marker). ---
TUNSRC="boringtun/src/device/tun_linux.rs"
if ! grep -q 'target_arch = "mips"' "$TUNSRC"; then
  sed -i 's|^const TUNSETIFF: u64 = 0x4004_54ca;|#[cfg(any(target_arch = "mips", target_arch = "mips64"))]\nconst TUNSETIFF: u64 = 0x8004_54ca; // MIPS: BSD-style ioctl dir bits\n#[cfg(not(any(target_arch = "mips", target_arch = "mips64")))]\nconst TUNSETIFF: u64 = 0x4004_54ca;|' "$TUNSRC"
  echo "PATCHED TUNSETIFF for MIPS:"; grep -n 'TUNSETIFF: u64' "$TUNSRC"
else
  echo "TUNSETIFF already patched"
fi

# --- PATCH: make the IPv6 listen socket best-effort (this kernel has no IPv6,
# so the unconditional AF_INET6 socket() fails EAFNOSUPPORT and kills startup) ---
python3 "$HERE/patch_boringtun.py" boringtun/src/device/mod.rs

# panic_immediate_abort was renamed; just build-std normally (size is fine).
BIN=target/mipsel-unknown-linux-musl/release/boringtun-cli
rm -f "$BIN"   # so a failed link can't masquerade as success on a stale binary
cargo +nightly build --release -p boringtun-cli \
  --target mipsel-unknown-linux-musl \
  -Z build-std=std,panic_abort 2>&1 | tail -25

echo "=== result ==="
if [ -f "$BIN" ]; then
  ls -l "$BIN"
  "$TC/mipsel-linux-musl-readelf" -h "$BIN" | grep -E 'Machine|Type'
  echo "-- must be static, no INTERP, no NEEDED --"
  "$TC/mipsel-linux-musl-readelf" -l "$BIN" | grep -iE 'INTERP|interpreter' || echo "no INTERP (good)"
  "$TC/mipsel-linux-musl-readelf" -d "$BIN" 2>/dev/null | grep -iE 'NEEDED' || echo "no NEEDED (good, static)"
  "$TC/mipsel-linux-musl-strip" "$BIN" -o "$WORK/boringtun-cli.stripped"
  ls -l "$WORK/boringtun-cli.stripped"
  md5sum "$WORK/boringtun-cli.stripped"
  mkdir -p "$REPO/bin"
  cp "$WORK/boringtun-cli.stripped" "$REPO/bin/boringtun-cli"
  echo "BORINGTUN_OK"
else
  echo "BORINGTUN_BUILD_FAILED"
fi
