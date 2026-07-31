#!/bin/bash
# build_integrated.sh -- assemble the UNIFIED BW4 deployable: ONE mtd4
# (/system) XZ-squashfs carrying all four LD_PRELOAD shims + a single
# /system/bin/vp_project wrapper, deployed exactly like the proven camweb v8
# image (standard mksquashfs -- NO jzlzma; the mtd3 jzlzma reflash path is dead,
# see memory cam-jzlzma: T23 HW-LZMA rejects the encoder -> boot loop).
#
# REQUIRES a dump of YOUR OWN camera's stock /system (mtd4). That vendor blob is
# deliberately NOT in this repo (see NOTICE.md), so you must supply it:
#   STOCK_MTD4=/path/to/mtd4_stock_backup.bin bash src/build_integrated.sh
# Take the dump on the camera itself, BEFORE flashing anything:
#   cat /dev/mtd4 > /mnt/sda0/mtd4_stock_backup.bin
# If you don't have (or don't want) the vendor media, use build_clean_image.sh
# instead -- it builds /system from scratch out of this repo's bin/ alone.
#
# Run from the repo root (or anywhere -- the script locates itself):
#   STOCK_MTD4=... bash src/build_integrated.sh
# On Windows, run it inside WSL.
set -e
# Locate the repo from this script's own path (src/ -> repo root).
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # .../src
ROOT="$(cd "$DIR/.." && pwd)"                          # repo root
BIN="$ROOT/bin"                                        # prebuilt shims/daemon
STOCK_MTD4=${STOCK_MTD4:-}                             # REQUIRED, user-supplied
MTD4_SZ=$((0x60000))            # 393216
W=$DIR/_wk

CAMWEB=$BIN/camweb.so       # unsetenv build (protects busybox/udhcpc children)
BAT=$BIN/battery_osd.so
WIFI=$BIN/wifi_sd.so
PIR=$BIN/pir_sleep.so
ONVIFD=$BIN/cam_onvifd      # on-device standalone RTSP:554 + ONVIF:80 daemon (now with G.711 audio track)
MICCAP=$BIN/mic_capture.so  # persistent pure-read of IMP AI dev1 -> UDP 127.0.0.1:5599 (feeds cam_onvifd audio); zero state change, WiFi-safe
# devpw/vuid are PER-UNIT -- pass real values via env for a real build; the public
# repo ships CHANGE_ME placeholders (never commit a real device password).
DEVPW=${DEVPW:-CHANGE_ME}
VUID=${VUID:-CHANGE_ME}

if [ -z "$STOCK_MTD4" ]; then
  cat >&2 <<'MSG'
ERROR: STOCK_MTD4 is not set.

This build starts from the VENDOR /system image, which is copyrighted and is NOT
shipped in this repo. Dump it from your OWN camera first:

    cat /dev/mtd4 > /mnt/sda0/mtd4_stock_backup.bin

then re-run:

    STOCK_MTD4=/path/to/mtd4_stock_backup.bin bash src/build_integrated.sh

Or use src/build_clean_image.sh, which needs no vendor dump (it builds /system
from scratch and loses only the vendor voice prompts).
MSG
  exit 1
fi
[ -f "$STOCK_MTD4" ] || { echo "ERROR: STOCK_MTD4=$STOCK_MTD4 does not exist" >&2; exit 1; }

for f in "$CAMWEB" "$BAT" "$WIFI" "$PIR" "$ONVIFD" "$MICCAP"; do
  [ -f "$f" ] || { echo "ERROR missing $f (expected in $BIN)"; exit 1; }
done

rm -rf "$W"; mkdir -p "$W"
echo "=== unsquashfs stock /system ==="
unsquashfs -d "$W/sys" "$STOCK_MTD4" >/dev/null 2>&1
echo "stock top-level:"; ls "$W/sys"

echo "=== add /system/lib/{camweb,wifi_sd,pir_sleep,battery_osd}.so ==="
mkdir -p "$W/sys/lib" "$W/sys/bin"
cp "$CAMWEB" "$W/sys/lib/camweb.so"
cp "$WIFI"    "$W/sys/lib/wifi_sd.so"
cp "$PIR"     "$W/sys/lib/pir_sleep.so"
cp "$BAT"     "$W/sys/lib/battery_osd.so"
cp "$MICCAP"  "$W/sys/lib/mic_capture.so"
chmod 0755 "$W/sys/lib/"*.so

echo "=== add /system/bin/cam_onvifd + /system/etc/cam_onvifd.conf ==="
cp "$ONVIFD" "$W/sys/bin/cam_onvifd"; chmod 0755 "$W/sys/bin/cam_onvifd"
mkdir -p "$W/sys/etc"
cat > "$W/sys/etc/cam_onvifd.conf" <<EOF
# per-unit device creds for the local livestream.cgi handshake
devpw=$DEVPW
vuid=$VUID
cam_host=127.0.0.1
cam_port=81
streamid=10
substream=2
rtsp_port=554
rtsp_name=live
onvif_port=80
log_level=2
EOF
# NOTE: `|| true` -- without it the failed test would abort the script under `set -e`.
[ "$DEVPW" = "CHANGE_ME" ] && echo "WARN: building with placeholder devpw (set DEVPW=... for a real cam)" || true

echo "=== write single /system/bin/vp_project wrapper (unified LD_PRELOAD chain) ==="
cat > "$W/sys/bin/vp_project" <<'EOS'
#!/bin/sh
# UNIFIED BW4 shim wrapper. /system/bin is first on PATH so this shadows
# /usr/bin/vp_project; the real binary is exec'd by absolute path (no recursion).
#
# 1) NOP the create_web onboarding gate in the RAM copy so camweb can bind :81
#    (REQUIRED on an onboarded cam; stored pw != "888888" so create_web self-bails).
#    On-disk /usr/bin/vp_project is NEVER modified -- this dd hits the live copy only.
printf '\000\000\000\000' | dd of=/usr/bin/vp_project bs=1 seek=514932 count=4 conv=notrunc 2>/dev/null
# 2) OTA block: once vnet0 has an IP, keep multicast (WS-Discovery) + LAN /24,
#    reject all other (non-LAN) routes so the cam can't reach the cloud/OTA.
( n=0
  while [ $n -lt 60 ]; do
    if ifconfig vnet0 2>/dev/null | grep -q "inet addr"; then
      route add -net 224.0.0.0 netmask 240.0.0.0 dev vnet0 2>/dev/null  # multicast egress (WS-Discovery)
      route add -net 0.0.0.0   netmask 128.0.0.0 reject 2>/dev/null
      route add -net 128.0.0.0 netmask 128.0.0.0 reject 2>/dev/null
      break
    fi
    n=$((n+1)); sleep 3
  done ) &
# 3) on-device ONVIF/RTSP daemon: wait for camweb to bind :81, then start it
#    (it self-reconnects to the local H.264 source thereafter). Replaces the PC proxy.
( n=0
  while [ $n -lt 90 ]; do
    if netstat -ltn 2>/dev/null | grep -q "0.0.0.0:81 "; then
      # /system is a read-only squashfs (and the packaged binary may lack +x),
      # so stage into tmpfs and make it executable there before running.
      cp /system/bin/cam_onvifd /tmp/cam_onvifd 2>/dev/null
      chmod 0755 /tmp/cam_onvifd 2>/dev/null
      /tmp/cam_onvifd --conf /system/etc/cam_onvifd.conf > /tmp/cam_onvifd.log 2>&1
      break
    fi
    n=$((n+1)); sleep 2
  done ) &
# 4) ONE LD_PRELOAD list for ALL features -- no wrapper/rcS clobber. camweb.so
#    leads: its constructor unsetenv("LD_PRELOAD")s so busybox children (udhcpc)
#    don't inherit the chain and fail to resolve pthread_create (which would kill WiFi).
export LD_PRELOAD="/system/lib/camweb.so /system/lib/wifi_sd.so /system/lib/pir_sleep.so /system/lib/battery_osd.so /system/lib/mic_capture.so "
# 5) hand off to the real binary.
exec /usr/bin/vp_project "$@"
EOS
chmod 0755 "$W/sys/bin/vp_project"
echo "wrapper:"; cat "$W/sys/bin/vp_project"

echo "=== mksquashfs (xz, block 131072, all-root) ==="
rm -f "$W/sys.squashfs"
mksquashfs "$W/sys" "$W/sys.squashfs" -comp xz -b 131072 -noappend -all-root 2>&1 | tail -4
NEW=$(stat -c%s "$W/sys.squashfs")
echo "squashfs size=$NEW  budget=$MTD4_SZ  fits=$([ $NEW -le $MTD4_SZ ] && echo YES || echo NO)"
[ $NEW -le $MTD4_SZ ] || { echo "ERROR: exceeds mtd4 budget -- trim /system/www voice prompts like v8"; exit 1; }

OUT=${1:-$ROOT/mtd4_integrated.bin}
python3 - "$W/sys.squashfs" "$OUT" $MTD4_SZ <<'PY'
import sys, hashlib
src, out, sz = sys.argv[1], sys.argv[2], int(sys.argv[3])
d = open(src,'rb').read()
d = d + b'\x00'*(sz-len(d))
open(out,'wb').write(d)
print("wrote", out, "len", len(d), "md5", hashlib.md5(d).hexdigest())
PY

echo "=== VERIFY: re-unsquashfs the built image ==="
rm -rf "$W/verify"
unsquashfs -d "$W/verify" "$OUT" >/dev/null 2>&1
echo "--- /system/bin/vp_project present ---"; ls -la "$W/verify/bin/vp_project"
echo "--- /system/lib/*.so present + md5 match source ---"
for pair in "camweb.so:$CAMWEB" "wifi_sd.so:$WIFI" "pir_sleep.so:$PIR" "battery_osd.so:$BAT"; do
  n=${pair%%:*}; src=${pair##*:}
  a=$(md5sum "$W/verify/lib/$n" | awk '{print $1}')
  b=$(md5sum "$src" | awk '{print $1}')
  echo "  $n  in-image=$a  source=$b  match=$([ "$a" = "$b" ] && echo YES || echo NO)"
done
echo "--- LD_PRELOAD line in wrapper ---"; grep LD_PRELOAD "$W/verify/bin/vp_project"
echo DONE
