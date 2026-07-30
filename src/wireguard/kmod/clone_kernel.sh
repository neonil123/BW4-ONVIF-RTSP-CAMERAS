#!/usr/bin/env bash
# Clone the thingino Ingenic 3.10 kernel (shallow) and survey T23 config + WireGuard.
set -e
. "$HOME/.cargo/env" 2>/dev/null || true
W="$HOME/kmod"; mkdir -p "$W"; cd "$W"
if [ ! -d thingino-linux ]; then
  echo "=== cloning thingino-linux (shallow) ==="
  git clone --depth 1 https://github.com/themactep/thingino-linux.git 2>&1 | tail -3
fi
cd thingino-linux
echo "=== kernel version ==="; head -5 Makefile
echo "=== T23 defconfigs ==="; ls arch/mips/configs/ 2>/dev/null | grep -iE 't23|isvp' | head
echo "=== wireguard in tree? ==="
ls -d drivers/net/wireguard 2>/dev/null && echo "(wireguard dir present)" || echo "(no drivers/net/wireguard)"
grep -rl "WIREGUARD" arch/mips/configs/ 2>/dev/null | head
echo "=== any wireguard config symbol ==="; grep -rn "CONFIG_WIREGUARD" arch/mips/configs/*t23* 2>/dev/null | head
echo "=== net/ipv4 udp_tunnel present (WG dep)? ==="; ls net/ipv4/udp_tunnel.c 2>/dev/null || echo "(no udp_tunnel.c)"
echo DONE
