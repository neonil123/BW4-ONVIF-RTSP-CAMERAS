#!/bin/sh
# load_cfg.sh -- extract the baked WiFi + WireGuard config from FLASH into RAM.
# The config blob lives in our mtd4 partition at 0x40000 (block 8, the free space
# after the /system squashfs -- the vendor never touches mtd4). Keys therefore
# never sit on the SD. Written to /tmp so the shims (aic_wifi, wg_up) read them.
#
# Blob format (plain text, terminated by ###END###, rest of the block is 0xFF):
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

# read block 8 (0x40000 = 4096*64), 8 KB is plenty for both configs
dd if=/dev/mtd4 bs=4096 skip=64 count=2 2>/dev/null > "$BLOB" || exit 0
[ -s "$BLOB" ] || exit 0

sect=""
while IFS= read -r line; do
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
