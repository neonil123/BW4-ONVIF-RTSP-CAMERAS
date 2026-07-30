#!/bin/sh
# wg_up_kmod.sh -- bring up IN-KERNEL WireGuard: load wireguard.ko, create +
# configure wg0 via wgctl (netlink), set address + routes with busybox. The
# module + wgctl live in the /system image; config comes from flash (load_cfg
# wrote /tmp/wireguard.conf). Keeps the image's cloud-block reject routes and
# punches one host-route hole to the server endpoint.
set -e
INI="${1:-/tmp/wireguard.conf}"
DEV=wg0
KO=""; for c in "$KO_OVR" /system/lib/wireguard.ko /mnt/sda0/wireguard.ko; do [ -n "$c" ] && [ -f "$c" ] && { KO="$c"; break; }; done
CTL=""; for c in "$CTL_OVR" /system/bin/wgctl /mnt/sda0/wgctl; do [ -n "$c" ] && [ -x "$c" ] && { CTL="$c"; break; }; done

[ -f "$INI" ] || { echo "no config: $INI"; exit 1; }
[ -n "$KO" ] || { echo "wireguard.ko not found"; exit 1; }
[ -n "$CTL" ] || { echo "wgctl not found"; exit 1; }

getv(){
  val=""
  while IFS= read -r line; do
    k="${line%%=*}"; k="$(printf '%s' "$k" | tr -d ' \t\r')"
    lk="$(printf '%s' "$k" | tr 'A-Z' 'a-z')"; want="$(printf '%s' "$1" | tr 'A-Z' 'a-z')"
    if [ "$lk" = "$want" ]; then
      v="${line#*=}"; v="$(printf '%s' "$v" | tr -d '\r')"
      while [ "${v# }" != "$v" ]; do v="${v# }"; done
      while [ "${v%	}" != "$v" ]; do v="${v%	}"; done
      while [ "${v% }" != "$v" ]; do v="${v% }"; done
      val="$v"
    fi
  done < "$INI"
  printf '%s' "$val"
}
mask(){ case "$1" in 32) echo 255.255.255.255;; 24) echo 255.255.255.0;; 16) echo 255.255.0.0;; 8) echo 255.0.0.0;; *) echo 255.255.255.0;; esac; }

ADDR="$(getv Address)"; EP="$(getv Endpoint)"; AIPS="$(getv AllowedIPs)"; MTU="$(getv MTU)"; [ -n "$MTU" ] || MTU=1420
IP="${ADDR%%/*}"; PFX="${ADDR#*/}"; [ "$PFX" = "$ADDR" ] && PFX=32
[ -n "$AIPS" ] || AIPS="${IP%.*}.0/24"; ANET="${AIPS%%/*}"; ABITS="${AIPS#*/}"
[ -n "$IP" ] && [ -n "$EP" ] || { echo "config incomplete (Address/Endpoint)"; exit 1; }

# 1) load the kernel module
grep -q '^wireguard ' /proc/modules || insmod "$KO" || { echo "insmod wireguard.ko failed"; exit 1; }

# 2) pin a host route to the server endpoint via the real gateway (keep cloud-block)
EPIP="${EP%%:*}"
set -- $(route -n 2>/dev/null | while read d g m f met ref use ifc; do
  if [ "$d" = "0.0.0.0" ] && [ "$m" = "0.0.0.0" ]; then echo "$g $ifc"; break; fi
done)
GW="$1"; GWIF="$2"
case "$EPIP" in *[0-9].[0-9]*) [ -n "$GW" ] && [ -n "$GWIF" ] && route add -host "$EPIP" gw "$GW" dev "$GWIF" 2>/dev/null || true ;; esac

# 3) create + configure wg0 via netlink (crypto/peer)
"$CTL" "$DEV" "$INI"

# 4) address + MTU + subnet route
ifconfig "$DEV" "$IP" netmask "$(mask "$PFX")" mtu "$MTU" up
if [ "$ANET" != "$IP" ] && [ "$ABITS" != "32" ]; then
  route add -net "$ANET" netmask "$(mask "$ABITS")" dev "$DEV" 2>/dev/null || true
fi
echo "=== wg0 up (kernel) ==="
ifconfig "$DEV" | grep -iE "inet addr|MTU"
