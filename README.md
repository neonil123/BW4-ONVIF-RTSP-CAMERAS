# BW4 → native ONVIF / RTSP camera (cloud-free, local)

Turn a **BW4** battery/solar Wi-Fi camera (Ingenic **T23N**) — *your own hardware* — into
a **standards ONVIF/RTSP camera** that any NVR pulls directly on your LAN, and retire the vendor /
Eye4 cloud. The camera runs an on-device ONVIF/RTSP daemon, so Synology Surveillance Station,
Frigate, Blue Iris, Hikvision NVRs, etc. add it like any other IP camera:

```
rtsp://<cam-ip>:554/live      # H.264 2304×1296 15 fps + G.711 μ-law audio
http://<cam-ip>:80/onvif/...  # ONVIF Device/Media/Events + WS-Discovery
```

No cloud, no vendor app, no PC in the media path. The change ships as **one flashable overlay
touching only the `/system` partition (`mtd4`)** — the vendor app binary is never modified and the
whole thing is fully reversible.

> **Right-to-repair / authorized-use only.** This documents work on the owner's **own** cameras.
> Nothing here contacts any cloud service; the camera is deliberately kept **off** the internet so
> it can't OTA itself back to a cloud-only state. Don't point this at hardware that isn't yours.

---

## What works / what doesn't

Legend: ✅ verified live on the camera · 🟡 built & host-verified, needs a bench gate · ⚠️ open · ❌ dead end

| Capability | Status | Notes |
|---|---|---|
| **RTSP `:554` H.264** (main 2304×1296 / sub 640×360) | ✅ | primary media path, on-device; self-heals on source EOF |
| **ONVIF `:80`** (Device/Media/Events + WS-Discovery + snapshot) | ✅ | added live to a real Synology NVR, survives cold boot |
| **`camweb.so`** — enable the app's dormant local `:81` H.264 server | ✅ | binds `0.0.0.0:81`; `livestream.cgi` serves the stream |
| **Wi-Fi survives the LD_PRELOAD chain** | ✅ | `camweb.so` must lead (its `unsetenv` guard protects udhcpc) |
| **OTA/internet block** (reject non-LAN routes) | ✅ | keeps multicast for WS-Discovery, rejects the rest |
| **Microphone → RTSP** (G.711 μ-law, `PCMU/8000`, 2nd RTP track) | ✅ | native `IMP_AI` pure-read shim; coexists with the app — see [AUDIO.md](docs/AUDIO.md) |
| **`battery_osd.so`** — real battery % on the OSD | ✅ | live voltage read; label is letter-free (partial OSD font) |
| **`wifi_sd.so`** — SD-card Wi-Fi onboarding | ✅ switch / 🟡 first-join | AP-switch verified live; clean first-join needs a fresh unit |
| **`pir_sleep.so`** — low-battery PIR-wake sleep | 🟡 | enable/addresses re-proven; sleep branch needs battery <50% at bench |
| **Talk-back → speaker** (NVR → camera audio out) | ⚠️ **open / experimental** | on the [`talkback-experimental`](../../tree/talkback-experimental) branch, **not** in the default image: the software path reaches the DAC (loopback-proven) but the speaker stage never enables, **and** the AO calls corrupt the shared codec so the **mic goes robotic** — [AUDIO.md](docs/AUDIO.md#2-speaker--talk-back--not-audible--the-open-problem-️) |
| **`:81` audio CGI** as an audio source | ❌ | vendor handler builds the frame container but never calls `IMP_AI` |
| **Full Thingino firmware** (Track B) | ⚠️ blocked | flashes & boots, but AIC8800**U** Wi-Fi never enumerates — [FLASHING.md](docs/FLASHING.md#the-thingino-full-firmware-path) |

**Bottom line:** local video + ONVIF + one-way (camera→NVR) audio is done and solid. **Two-way
talk-back is the one unsolved feature** — the audio provably reaches the codec DAC but the speaker
amp stage stays off. See [AUDIO.md](docs/AUDIO.md) for the exact wall and the most promising lead.

---

## Target hardware

| Part | Detail |
|---|---|
| SoC | Ingenic **T23N** (MIPS32, kernel `3.10.14-Archon`, uClibc 0.9.33) |
| Sensor | GalaxyCore **GC2083** (fw also probes OmniVision OS02N10); H.264 main 2304×1296 / sub 640×360 |
| Wi-Fi | **AIC8800** (AIC8800U/MC over SDIO); brought up by U-Boot + `aic8800_netdrv.ko` |
| Codec | Ingenic on-chip **`jz-inner-codec`** (class-D speaker), built into the kernel |
| Power MCU | battery gauge, PIR wake, deep-sleep power gate (AIC keepalive) |
| Flash | 8 MiB SPI NOR; `/system` = **mtd4** |
| Power | USB-C + solar; optional battery. No ESP32 on the board. |

> The camera is marketed as a **BW4** (also seen as BW4-N / BW4-Plus / XH-BW4).
> Some of the internal notes this repo grew from mislabelled it "QC3"/"BW6" — that's the same
> hardware.

---

## How it works (30-second version)

The stock camera has a fully capable H.264 encoder but exposes video only through the vendor
cloud. Instead of replacing the firmware, this project **re-uses the camera's own software** and
adds a thin layer:

1. A `/system/bin/vp_project` **wrapper** (PATH-shadows the real binary) sets up an `LD_PRELOAD`
   chain and starts the daemon at boot — the on-disk vendor binary is never patched.
2. **`camweb.so`** flips on the app's compiled-in-but-disabled local web server, so
   `livestream.cgi` serves raw H.264 on `127.0.0.1:81`.
3. **`cam_onvifd`** — a static MIPS C daemon on the camera — reads that local stream, re-frames it
   to **RTSP `:554`** and serves **ONVIF `:80`** (SOAP + WS-Discovery + snapshot). It also mixes in
   a live **G.711 audio track** fed by `mic_capture.so` (a pure `IMP_AI` reader).
4. A handful more `LD_PRELOAD` shims add battery-OSD, SD Wi-Fi onboarding, and PIR sleep. (The
   experimental talk-back shim lives on the `talkback-experimental` branch and is **kept out of
   the default image** — it corrupts the mic; see [AUDIO.md](docs/AUDIO.md).)

Everything is packed into one XZ-squashfs `mtd4` overlay. Deep dive: [ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Quick start

You need: the camera already onboarded to Wi-Fi, a serial console (COM, 115200 8N1, DTR/RTS
de-asserted), WSL/Linux with `mksquashfs`/`xz` for building, and — for a working stream — your
**unit's device password**.

```sh
# 1. Read your unit's device password + vuid (per-unit random, from NVS).
#    Method: docs/ARCHITECTURE.md  ->  "Device password".

# 2. Build an image carrying YOUR creds (needs the toolchain-built binaries in bin/;
#    to rebuild those from source see src/onvif_rtsp/build.sh and the shim notes).
DEVPW=<your-unit-devpw> VUID=<your-unit-vuid> bash src/build_clean_image.sh mycam.bin

# 3. On the camera: back up mtd4 FIRST (this is your revert image), then flash.
cat /dev/mtd4 > /mnt/sda0/mtd4_stock_backup.bin        # <-- do not skip
md5sum mycam.bin                                        # match your build output
kill -9 $(pidof vp_project) 2>/dev/null ; sleep 2
umount /system 2>/dev/null
flashcp -v mycam.bin /dev/mtd4
reboot
```

After reboot the camera comes up with Wi-Fi, `:81`, RTSP `:554`, ONVIF `:80`, the OTA block, and
the OSD/audio features live. Point your NVR at `rtsp://<cam-ip>:554/live` (ONVIF user/pass default
`admin`/`admin`). Full procedure, the SD-free TFTP method, and recovery: [FLASHING.md](docs/FLASHING.md).

> The prebuilt **`firmware/mtd4_integrated.bin`** (md5 `949ddff9eef4a6cdfd215ec1169c74eb`) is
> committed with `devpw=CHANGE_ME` and **no vendor voice-prompt media** — it's the reference
> structure and boots, but produces **no video** until you rebuild with your creds (step 2). To
> keep the camera's spoken voice prompts, build from your own stock `/system` dump instead — see
> [STOCK_SYSTEM.md](docs/STOCK_SYSTEM.md).

---

## Repository layout

```
firmware/  mtd4_integrated.bin        clean flashable overlay (CHANGE_ME, no vendor media) + .md5
bin/       cam_onvifd, *.so          our prebuilt MIPS binaries (daemon + 5 LD_PRELOAD shims)
src/
  onvif_rtsp/src/                      the on-device ONVIF/RTSP/audio daemon (C)
  shims/                               camweb, wifi_sd, pir_sleep, battery_osd, mic_capture
  build_clean_image.sh                 build the public-safe overlay (from scratch, your creds via env)
  build_integrated.sh                  build from YOUR camera's stock /system (keeps voice prompts)
docs/
  ARCHITECTURE.md  FLASHING.md  ONVIF.md  FEATURES.md  AUDIO.md  DISCOVERY.md  TESTING.md
  STOCK_SYSTEM.md
NOTICE.md   LICENSE
```

## Docs

- **[ARCHITECTURE.md](docs/ARCHITECTURE.md)** — hardware, flash map, the deploy channel, the
  LD_PRELOAD chain, the media path, and how the per-unit device password is read.
- **[FLASHING.md](docs/FLASHING.md)** — exact flash + revert, the **NEVER-DFU** rule, the SD-free
  TFTP method, auto-start-on-boot, recovery.
- **[ONVIF.md](docs/ONVIF.md)** — the daemon's ONVIF/RTSP services, auth, and an NVR walkthrough.
- **[FEATURES.md](docs/FEATURES.md)** — each shim precisely (addresses, mechanism).
- **[AUDIO.md](docs/AUDIO.md)** — mic (done) and talk-back (open) in full.
- **[TESTING.md](docs/TESTING.md)** — end-to-end test matrix.
- **[STOCK_SYSTEM.md](docs/STOCK_SYSTEM.md)** — build an image from your own stock dump / keep
  voice prompts.

## Safety & secrets

- The admin **device password is a per-unit random value** in `mtd5` NVS — not a default, not
  invented here. Every committed artifact ships **`CHANGE_ME`** placeholders; you supply your own
  unit's value at build time. **Never commit an image built with a real password.**
- **Only `mtd4` is ever written.** `mtd0/mtd2/mtd3/mtd5` stay stock, so every deploy is
  revert-safe. **Do not DFU** unless the camera won't boot at all — a full-chip DFU wipes `mtd5`
  and your Wi-Fi creds. See [FLASHING.md](docs/FLASHING.md).
- This overlay contains **only our own code** plus neutral text stubs — no vendor firmware, media,
  or binaries are redistributed here. Details: [NOTICE.md](NOTICE.md).

## License

Our code is released under the MIT License ([LICENSE](LICENSE)). It interoperates with Ingenic's
proprietary IMP SDK and the vendor application via documented addresses/ABIs; no vendor code is
included. See [NOTICE.md](NOTICE.md).
