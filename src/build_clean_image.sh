#!/bin/bash
# build_clean_image.sh -- assemble a PUBLIC-SAFE flashable mtd4 (/system) overlay
# for the BW4. Unlike build_integrated.sh (which unsquashes the vendor stock
# /system and adds our files, thereby carrying the vendor voice-prompt media),
# this builds /system FROM SCRATCH with ONLY our own files plus tiny neutral
# text stubs. No vendor .opus/.g711a/logo assets, no per-unit secrets.
#
# Tradeoff vs the stock-derived image: the camera loses its spoken voice prompts
# ("wifi link success", power on/off chimes). Everything else -- video, ONVIF,
# RTSP, mic audio, the QoL shims -- is unaffected. If you want the voice prompts
# back, use build_integrated.sh against your OWN camera's stock /system dump
# (see docs/STOCK_SYSTEM.md).
#
# Requires: mksquashfs + unsquashfs (squashfs-tools), python3. All inputs come
# from this repo's own bin/ directory -- no vendor firmware dump is needed.
#
# Run from the repo root (or anywhere -- the script locates itself):
#   bash src/build_clean_image.sh mycam.bin
# On Windows, run it inside WSL:
#   wsl -- bash ./src/build_clean_image.sh mycam.bin
set -e
# Locate the repo from this script's own path (src/ -> repo root), so the build
# works from any checkout location.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # .../src
ROOT="$(cd "$DIR/.." && pwd)"                          # repo root
BIN="$ROOT/bin"                                        # prebuilt shims/daemon
MTD4_SZ=$((0x60000))            # 393216
W=$DIR/_wk_clean

CAMWEB=$BIN/camweb.so
BAT=$BIN/battery_osd.so
WIFI=$BIN/wifi_sd.so
PIR=$BIN/pir_sleep.so
ONVIFD=$BIN/cam_onvifd
MICCAP=$BIN/mic_capture.so
# NOTE: speaker_feed.so (talk-back) is intentionally NOT built into this image.
# It manipulates the shared jz-inner-codec (IMP_AO SetPubAttr/Enable) and was
# found to corrupt the 8 kHz microphone into a robotic/metallic signal. The
# talk-back work lives on the `talkback-experimental` branch until the codec
# speaker-enable is solved. See docs/AUDIO.md.
# Per-unit creds. Defaults to placeholders so the committed/public image carries
# NO secret. For a WORKING image on YOUR camera, pass your unit's values:
#   DEVPW=xxxxxxxx VUID=VQxxxxxxxXXXX bash build_clean_image.sh my_image.bin
# (how to read them off your own unit: docs/ARCHITECTURE.md -> Device password).
# NEVER commit an image built with real creds to a public repo.
DEVPW=${DEVPW:-CHANGE_ME}
VUID=${VUID:-CHANGE_ME}

for f in "$CAMWEB" "$BAT" "$WIFI" "$PIR" "$ONVIFD" "$MICCAP"; do
  [ -f "$f" ] || { echo "ERROR missing $f (expected in $BIN -- rebuild it, or restore the prebuilt from the repo)"; exit 1; }
done

rm -rf "$W"; mkdir -p "$W/sys/bin" "$W/sys/lib" "$W/sys/etc" "$W/sys/www" "$W/sys/version"

echo "=== our libs -> /system/lib ==="
cp "$CAMWEB" "$W/sys/lib/camweb.so"
cp "$WIFI"    "$W/sys/lib/wifi_sd.so"
cp "$PIR"     "$W/sys/lib/pir_sleep.so"
cp "$BAT"     "$W/sys/lib/battery_osd.so"
cp "$MICCAP"  "$W/sys/lib/mic_capture.so"
chmod 0755 "$W/sys/lib/"*.so

echo "=== daemon -> /system/bin + conf (CHANGE_ME) ==="
cp "$ONVIFD" "$W/sys/bin/cam_onvifd"; chmod 0755 "$W/sys/bin/cam_onvifd"
cat > "$W/sys/etc/cam_onvifd.conf" <<EOF
# per-unit device creds for the local livestream.cgi handshake.
# REPLACE these with your unit's values before flashing (see docs/FLASHING.md).
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

echo "=== neutral text stubs (no vendor media) ==="
printf 'EN\n'  > "$W/sys/www/language.txt"
: > "$W/sys/www/upgrade.txt"
printf 'community_overlay:bw4-onvif-rtsp\n' > "$W/sys/version/version.ini"
# NOTE: `|| true` -- without it the failed test would abort the script under `set -e`.
[ "$DEVPW" = "CHANGE_ME" ] && echo "WARN: building with placeholder devpw -- the :81 handshake will fail and there will be NO video. Set DEVPW=/VUID= for a real cam." || true

echo "=== /system/bin/vp_project wrapper (unified LD_PRELOAD chain) ==="
cat > "$W/sys/bin/vp_project" <<'EOS'
#!/bin/sh
# UNIFIED BW4 shim wrapper. /system/bin is first on PATH so this shadows
# /usr/bin/vp_project; the real binary is exec'd by absolute path (no recursion).
#
# 1) NOP the create_web onboarding gate in the RAM copy so camweb can bind :81
#    (REQUIRED on an onboarded cam; stored pw != "888888" so create_web self-bails).
#    On-disk /usr/bin/vp_project is NEVER modified -- this dd hits the live copy only.
printf '\000\000\000\000' | dd of=/usr/bin/vp_project bs=1 seek=514932 count=4 conv=notrunc 2>/dev/null
# 1b) FULL-RATE MIC: dev1/chn0 emits 25 fps of 16 kHz audio, but vp_project's own
#     audio thread AND our mic_capture both call the *consuming* IMP_AI_GetFrame,
#     so each gets every other frame (seq delta 2 = 12.5 fps = chunky audio after
#     correct-pitch 2:1 decimation in the daemon). Neutralize the vendor consumer
#     so mic_capture gets ALL frames (contiguous 16k -> continuous 8k PCMU):
#       @0x4c3908 (file 801032): vendor fetch's `jal GetFrame` -> `li v0,-2` so it
#         Polls dev1 (keeps pacing) but SKIPS GetFrame and returns -2, which its
#         audio thread treats as "transient, keep looping" (stays alive; any other
#         <0 would tear down the AI channel we read).
#       @0x454c50 (file 347216): NOP the now-per-iteration error log (no tmpfs spam).
printf '\376\377\002\044' | dd of=/usr/bin/vp_project bs=1 seek=801032 count=4 conv=notrunc 2>/dev/null
printf '\000\000\000\000' | dd of=/usr/bin/vp_project bs=1 seek=347216 count=4 conv=notrunc 2>/dev/null
# 2) OTA block: once vnet0 has an IP, keep multicast (WS-Discovery) + LAN /24,
#    reject all other (non-LAN) routes so the cam can't reach the cloud/OTA.
( n=0
  while [ $n -lt 60 ]; do
    if ifconfig vnet0 2>/dev/null | grep -q "inet addr"; then
      route add -net 224.0.0.0 netmask 240.0.0.0 dev vnet0 2>/dev/null
      route add -net 0.0.0.0   netmask 128.0.0.0 reject 2>/dev/null
      route add -net 128.0.0.0 netmask 128.0.0.0 reject 2>/dev/null
      break
    fi
    n=$((n+1)); sleep 3
  done ) &
# 3) on-device ONVIF/RTSP daemon: wait for camweb to bind :81, then start it.
( n=0
  while [ $n -lt 90 ]; do
    if netstat -ltn 2>/dev/null | grep -q "0.0.0.0:81 "; then
      cp /system/bin/cam_onvifd /tmp/cam_onvifd 2>/dev/null
      chmod 0755 /tmp/cam_onvifd 2>/dev/null
      /tmp/cam_onvifd --conf /system/etc/cam_onvifd.conf > /tmp/cam_onvifd.log 2>&1
      break
    fi
    n=$((n+1)); sleep 2
  done ) &
# 4) ONE LD_PRELOAD list for ALL features. camweb.so leads: its constructor
#    unsetenv("LD_PRELOAD")s so busybox children (udhcpc) don't inherit the chain.
export LD_PRELOAD="/system/lib/camweb.so /system/lib/wifi_sd.so /system/lib/pir_sleep.so /system/lib/battery_osd.so /system/lib/mic_capture.so "
# 5) hand off to the real binary.
exec /usr/bin/vp_project "$@"
EOS
chmod 0755 "$W/sys/bin/vp_project"

echo "=== mksquashfs (xz, block 131072, all-root) ==="
rm -f "$W/sys.squashfs"
mksquashfs "$W/sys" "$W/sys.squashfs" -comp xz -b 131072 -noappend -all-root 2>&1 | tail -3
NEW=$(stat -c%s "$W/sys.squashfs")
echo "squashfs size=$NEW budget=$MTD4_SZ fits=$([ $NEW -le $MTD4_SZ ] && echo YES || echo NO)"
[ $NEW -le $MTD4_SZ ] || { echo "ERROR: exceeds mtd4 budget"; exit 1; }

OUT=${1:-$ROOT/mtd4_integrated_clean.bin}
python3 - "$W/sys.squashfs" "$OUT" $MTD4_SZ <<'PY'
import sys, hashlib
src, out, sz = sys.argv[1], sys.argv[2], int(sys.argv[3])
d = open(src,'rb').read()
d = d + b'\x00'*(sz-len(d))
open(out,'wb').write(d)
print("wrote", out, "len", len(d), "md5", hashlib.md5(d).hexdigest())
PY

echo "=== VERIFY tree (must contain NO vendor media) ==="
rm -rf "$W/verify"; unsquashfs -d "$W/verify" "$OUT" >/dev/null 2>&1
find "$W/verify" -type f | sed "s#$W/verify##" | sort
echo "--- secret scan: conf must still hold CHANGE_ME placeholders ---"
if grep -q '^devpw=CHANGE_ME' "$W/verify/etc/cam_onvifd.conf" && grep -q '^vuid=CHANGE_ME' "$W/verify/etc/cam_onvifd.conf"; then
  echo "CLEAN: no per-unit secrets (placeholders intact)"
else
  echo "WARN: conf no longer holds CHANGE_ME placeholders -- do NOT publish this image"
fi
echo "--- vendor media scan (must be empty) ---"
find "$W/verify" \( -name '*.opus' -o -name '*.g711a' -o -name '*.gif' \) -print | grep . && echo "WARN vendor media present" || echo "CLEAN: no vendor media"
echo DONE
