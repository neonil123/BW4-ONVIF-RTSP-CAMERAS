#!/usr/bin/env bash
# Clone BOTH kernel-module sources (shallow) and survey T23 config + WireGuard:
#   1. thingino-linux           -- the Ingenic 3.10.14 kernel tree to build against
#   2. wireguard-linux-compat   -- the out-of-tree WireGuard module for 3.10-5.5
# Both land in $SRCDIR (default $HOME/kmod), which prep_kernel.sh and
# build_module.sh then read. Run this FIRST:
#   bash src/wireguard/kmod/clone_kernel.sh
set -e
. "$HOME/.cargo/env" 2>/dev/null || true
W="${SRCDIR:-$HOME/kmod}"; mkdir -p "$W"; cd "$W"
if [ ! -d thingino-linux ]; then
  echo "=== cloning thingino-linux (shallow) ==="
  git clone --depth 1 https://github.com/themactep/thingino-linux.git 2>&1 | tail -3
fi
if [ ! -d wireguard-linux-compat ]; then
  echo "=== cloning wireguard-linux-compat (shallow) ==="
  # Out-of-tree WireGuard for kernels 3.10-5.5; bundles its own udp_tunnel/crypto
  # compat. Upstream is git.zx2c4.com; the GitHub mirror is the fallback.
  git clone --depth 1 https://git.zx2c4.com/wireguard-linux-compat 2>&1 | tail -3 \
    || git clone --depth 1 https://github.com/WireGuard/wireguard-linux-compat.git 2>&1 | tail -3
fi
[ -d wireguard-linux-compat/src ] \
  && echo "wireguard-linux-compat/src OK" \
  || echo "WARN: wireguard-linux-compat/src missing -- build_module.sh will fail"
cd thingino-linux
echo "=== kernel version ==="; head -5 Makefile
echo "=== T23 defconfigs ==="; ls arch/mips/configs/ 2>/dev/null | grep -iE 't23|isvp' | head
echo "=== wireguard in tree? ==="
ls -d drivers/net/wireguard 2>/dev/null && echo "(wireguard dir present)" || echo "(no drivers/net/wireguard)"
grep -rl "WIREGUARD" arch/mips/configs/ 2>/dev/null | head
echo "=== any wireguard config symbol ==="; grep -rn "CONFIG_WIREGUARD" arch/mips/configs/*t23* 2>/dev/null | head
echo "=== net/ipv4 udp_tunnel present (WG dep)? ==="; ls net/ipv4/udp_tunnel.c 2>/dev/null || echo "(no udp_tunnel.c)"
echo DONE
