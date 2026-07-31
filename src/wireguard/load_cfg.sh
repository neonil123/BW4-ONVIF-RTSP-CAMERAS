#!/bin/sh
# load_cfg.sh -- extract the baked WiFi + WireGuard config from FLASH into RAM.
# The config blob lives in our mtd4 partition at 0x58000 -- the last 32 KB of the
# 0x60000 partition, i.e. free space after the /system squashfs (the vendor never
# touches mtd4). Keys therefore never sit on the SD. Written to /tmp so the shims
# (aic_wifi, wg_up) read them.
#
# Blob format (plain text, terminated by ###END###; the builders zero-pad mtd4 out
# to 0x60000, so on a freshly built image the rest of the region reads 0x00 --
# either way parsing stops at ###END###):
#   ###BW4CFG1###
#   ###WGDEFAULT=on###        (or off)
#   ###WIFI###
#   SSID=...
#   PASSWORD=...
#   ###WG###
#   [Interface] ... [Peer] ...   (a standard wg-quick .conf)
#   ###END###
BLOB=/tmp/cfgblob
: > /tmp/wifi.ini
: > /tmp/wireguard.conf
echo on > /tmp/wg_default

# read the config block at 0x58000 (= 4096*88, the last 32 KB of mtd4), 8 KB
dd if=/dev/mtd4 bs=4096 skip=88 count=2 2>/dev/null > "$BLOB" || exit 0
[ -s "$BLOB" ] || exit 0

sect=""
while IFS= read -r line; do
  line="$(printf '%s' "$line" | tr -d '\r')"   # strip CR (Windows .conf CRLF)
  case "$line" in
    '###END###') break ;;
    '###BW4CFG1###') continue ;;
    '###WGDEFAULT='*) d="${line#\#\#\#WGDEFAULT=}"; d="${d%\#\#\#}"; echo "$d" > /tmp/wg_default; continue ;;
    '###WIFI###') sect=wifi; continue ;;
    '###WG###') sect=wg; continue ;;
  esac
  case "$sect" in
    wifi) printf '%s\n' "$line" >> /tmp/wifi.ini ;;
    wg)   printf '%s\n' "$line" >> /tmp/wireguard.conf ;;
  esac
done < "$BLOB"
rm -f "$BLOB"
chmod 600 /tmp/wifi.ini /tmp/wireguard.conf 2>/dev/null
