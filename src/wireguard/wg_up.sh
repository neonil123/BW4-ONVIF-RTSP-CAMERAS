#!/bin/sh
# wg_up.sh -- bring up a userspace WireGuard client (boringtun + wg) on the cam.
# Busybox-only (no iproute2/iptables/awk/sed/tail). Consumes a standard wg-quick
# .conf (or .ini): the [Interface]/[Peer] section headers are ignored; keys are
# read by name. The camera dials OUT to the server, so it traverses any NAT.
#
#   [Interface]  PrivateKey, Address (e.g. 10.9.0.2/32), [MTU]
#   [Peer]       PublicKey, [PresharedKey], Endpoint (host:port, e.g.
#                203.0.113.4:51820), AllowedIPs (e.g. 10.9.0.0/24),
#                [PersistentKeepalive]
# See docs/WIREGUARD.md for a full example config -- all addresses here are
# placeholders; substitute your own tunnel subnet and server endpoint.
set -e
# Default config is the RAM copy the boot wrapper extracts from flash (keys never
# sit on the SD). An explicit path arg still overrides (e.g. for testing).
INI="${1:-/tmp/wireguard.conf}"
DEV=wg0
BASE="$(dirname "$0")"
# locate boringtun + wg (SD kit preferred; also alongside this script). The 1 MB
# boringtun binary lives on the SD -- the camera has no free flash for it.
BT=""; for c in "$BT_OVR" "$BASE/boringtun-cli" "$BASE/bt" /mnt/sda0/boringtun-cli /mnt/sda0/bt; do [ -n "$c" ] && [ -x "$c" ] && { BT="$c"; break; }; done
WG=""; for c in "$WG_OVR" "$BASE/wg" "$BASE/wgx" /mnt/sda0/wg /mnt/sda0/wgx; do [ -n "$c" ] && [ -x "$c" ] && { WG="$c"; break; }; done
LOG=/tmp/wg0.log
RUN=/tmp/wg

[ -f "$INI" ] || { echo "no config: $INI"; exit 1; }
[ -x "$BT" ] || { echo "missing boringtun ($BT)"; exit 1; }
[ -x "$WG" ] || { echo "missing wg ($WG)"; exit 1; }

# --- ini/conf value by key (case-insensitive, tolerant of spaces around '='),
#     no tail/sed/awk (busybox). Takes the last matching line. ---
getv(){
  val=""
  while IFS= read -r line; do
    case "$line" in
      [Ss][Kk][Ii][Pp]) : ;;
    esac
    # match "  key = ..." ignoring case and surrounding spaces
    k="${line%%=*}"; k="$(printf '%s' "$k" | tr -d ' \t\r')"
    lk="$(printf '%s' "$k" | tr 'A-Z' 'a-z')"
    want="$(printf '%s' "$1" | tr 'A-Z' 'a-z')"
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
mask(){ case "$1" in 32) echo 255.255.255.255;; 30) echo 255.255.255.252;; 24) echo 255.255.255.0;; 16) echo 255.255.0.0;; 8) echo 255.0.0.0;; *) echo 255.255.255.0;; esac; }

PRIV="$(getv PrivateKey)"
ADDR="$(getv Address)"
SPUB="$(getv PublicKey)"; [ -n "$SPUB" ] || SPUB="$(getv ServerPublicKey)"
PSK="$(getv PresharedKey)"
EP="$(getv Endpoint)"
AIPS="$(getv AllowedIPs)"
KA="$(getv PersistentKeepalive)"; [ -n "$KA" ] || KA="$(getv Keepalive)"; [ -n "$KA" ] || KA=25
MTU="$(getv MTU)"; [ -n "$MTU" ] || MTU=1420

IP="${ADDR%%/*}"; PFX="${ADDR#*/}"; [ "$PFX" = "$ADDR" ] && PFX=32
[ -n "$AIPS" ] || AIPS="${IP%.*}.0/24"
ANET="${AIPS%%/*}"; ABITS="${AIPS#*/}"

[ -n "$PRIV" ] && [ -n "$SPUB" ] && [ -n "$EP" ] && [ -n "$IP" ] || { echo "config incomplete (need PrivateKey/PublicKey/Endpoint/Address)"; exit 1; }

# NOTE: the image's cloud-block reject routes (0.0.0.0/1 + 128.0.0.0/1) are LEFT
# in place -- they keep the camera off the vendor cloud. WireGuard punches exactly
# one hole (the endpoint host route below); the WG subnet is reached via the more-
# specific wg0 route added later. Net posture: LAN + your WG server only.

# --- pin a host route to the server endpoint via the real default gateway, so
#     WG's own encrypted packets always reach the server (standard wg-quick) ---
EPIP="${EP%%:*}"
set -- $(route -n 2>/dev/null | while read d g m f met ref use ifc; do
  if [ "$d" = "0.0.0.0" ] && [ "$m" = "0.0.0.0" ]; then echo "$g $ifc"; break; fi
done)
GW="$1"; GWIF="$2"
case "$EPIP" in
  *[0-9].[0-9]*) [ -n "$GW" ] && [ -n "$GWIF" ] && route add -host "$EPIP" gw "$GW" dev "$GWIF" 2>/dev/null || true ;;
esac

# --- start boringtun under a supervisor (creates wg0 + userspace uapi socket) ---
# Run boringtun in the FOREGROUND (-f) inside a trap-protected restart loop, NOT
# via its own daemonize (which is unstable on this kernel). The supervisor ignores
# HUP so it survives its parent exiting, and restarts boringtun on crash/WiFi flap.
#   --disable-connected-udp is REQUIRED: the connected-UDP path aborts on 3.10.
#   --disable-multi-queue for the old tun driver.
if ! pidof boringtun-cli bt >/dev/null 2>&1; then
  echo "starting boringtun on $DEV ..."
  cat > /tmp/wg_sup.sh <<EOS
#!/bin/sh
trap "" HUP INT TERM
while true; do
  "$BT" --disable-drop-privileges --disable-multi-queue --disable-connected-udp -t 1 -v info -l "$LOG" -f "$DEV"
  sleep 2
done
EOS
  chmod 0755 /tmp/wg_sup.sh
  sh /tmp/wg_sup.sh >/dev/null 2>&1 &
  i=0; while ! ifconfig "$DEV" >/dev/null 2>&1; do i=$((i+1)); [ $i -gt 30 ] && { echo "wg0 never appeared"; exit 1; }; sleep 1; done
fi

# --- address + MTU (busybox ifconfig) ---
ifconfig "$DEV" "$IP" netmask "$(mask "$PFX")" mtu "$MTU" up

# --- crypto/peer via wg (talks to boringtun's uapi socket) ---
mkdir -p "$RUN"; umask 077
printf '%s\n' "$PRIV" > "$RUN/priv"
"$WG" set "$DEV" private-key "$RUN/priv"
if [ -n "$PSK" ]; then
  printf '%s\n' "$PSK" > "$RUN/psk"
  "$WG" set "$DEV" peer "$SPUB" preshared-key "$RUN/psk" endpoint "$EP" allowed-ips "$AIPS" persistent-keepalive "$KA"
  rm -f "$RUN/psk"
else
  "$WG" set "$DEV" peer "$SPUB" endpoint "$EP" allowed-ips "$AIPS" persistent-keepalive "$KA"
fi
rm -f "$RUN/priv"

# --- route the WG subnet into wg0 (needed because Address is a /32) ---
if [ "$ANET" != "$IP" ] && [ "$ABITS" != "32" ]; then
  route add -net "$ANET" netmask "$(mask "$ABITS")" dev "$DEV" 2>/dev/null || true
fi

echo "=== wg0 up ==="
ifconfig "$DEV" | grep -iE "inet addr|MTU"
echo "--- wg show ---"
"$WG" show "$DEV"
