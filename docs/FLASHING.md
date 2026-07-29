# Flashing & Recovery

How to deploy the shim image and how to get back to stock, precisely and safely. The
production deploy writes **only `mtd4`** with `flashcp`. DFU is the last-resort recovery
channel and has one hard rule.

> ### Read this first (public image)
> - The image in **`firmware/mtd4_integrated.bin`** (md5 `949ddff9eef4a6cdfd215ec1169c74eb`)
>   is built with **`devpw=CHANGE_ME`**. Flashed as-is it will boot and expose ONVIF, but the
>   `:81` handshake fails so **there is no video** until you supply your unit's credentials.
> - **Make a working image for your camera:** read your unit's device password
>   ([ARCHITECTURE.md](ARCHITECTURE.md) → *Device password*), then rebuild:
>   `DEVPW=xxxx VUID=VQxxxx bash src/build_clean_image.sh my_cam.bin` and flash `my_cam.bin`.
>   The md5 below will differ from the committed one once you bake in real creds — that's
>   expected; **never commit that image**.
> - **This repo ships no vendor stock/revert images.** Your revert image is the **backup you
>   take of your own `mtd4` before flashing** (commands below). Take it first.

---

## The golden rules

1. **Only `/dev/mtd4` is ever written.** `mtd0` (boot), `mtd2` (kernel), `mtd3` (rootfs),
   `mtd5` (NVS) and the appfs stay stock. This is what makes every deploy revert-safe.
2. **NEVER DFU unless the camera won't boot at all.** A full-chip DFU write **rewrites
   `mtd5` NVS and erases the WiFi credentials** — you'd have to re-onboard (and re-read the
   device password). Revert with `flashcp` of a stock/known-good `mtd4` instead.
3. **Never reflash `mtd3`.** The T23 uses a **hardware LZMA** decoder that rejects any
   re-compressed rootfs (even a no-op) → `lzma dec timeout` boot loop. See
   [ARCHITECTURE.md](ARCHITECTURE.md) §2. This is why everything ships via `mtd4`.
4. **camweb must stay first in the LD_PRELOAD chain** — it's what keeps WiFi alive. The
   integrated image already does this; don't hand-edit the chain.

---

## Prerequisites

- Camera **already onboarded** to WiFi (creds in `mtd5` NVS). If not, onboard first (via the
  vendor app on **pure stock** — the shim interferes with initial pairing — or use the
  `wifi_sd` SD-card method once that's deployed).
- A **working SD slot** with a FAT card. (Historical caution: on the pilot the SD slot
  physically failed after ~10 insert/flash cycles — treat the slot gently; a dead slot blocks
  the only non-DFU flash channel.)
- Serial console on **COM3, 115200 8N1, DTR/RTS de-asserted**. Root login
  `vstarcam2017` / `20170912`.
- **Your own `mtd4` backup staged on the SD** (this is your revert image — take it *before*
  you flash anything):
  ```sh
  cat /dev/mtd4 > /mnt/sda0/mtd4_stock_backup.bin
  md5sum /mnt/sda0/mtd4_stock_backup.bin      # record this
  ```
- **Optional but recommended: a full-chip DFU backup** of the whole 8 MiB before you start
  (`dfu-util -a flash -U mycam_full_backup.bin`, see the DFU section). Keep it off-repo — a
  full dump contains your NVS/WiFi creds and vendor firmware; **never publish it.**

---

## Deploy — flash the integrated image to `mtd4`

Pre-flight md5 gates (abort if any mismatches — do **not** DFU to "fix" the SD):

```sh
mount | grep sda0                                   # SD must be at /mnt/sda0
md5sum /mnt/sda0/mtd4_integrated.bin                # MUST equal 949ddff9eef4a6cdfd215ec1169c74eb
md5sum /mnt/sda0/mtd4_camweb_v8.bin                # MUST equal fb1266aa06d4faa7d1efa46c844d304f
cat /dev/mtd4 > /mnt/sda0/mtd4_backup_live.bin && md5sum /mnt/sda0/mtd4_backup_live.bin  # extra revert
```

> The md5 to verify is **`949ddff9eef4a6cdfd215ec1169c74eb`** — the current
> `mtd4_integrated.bin`, which carries the four shims **plus `cam_onvifd`** and auto-starts
> the daemon on boot. Two older values are **stale**: `0b3273fd…` was the 4-shim build before
> the daemon was baked in, and the integrated `README.md` / `BENCH_FLASH_INTEGRATED.md`
> runbook cite `6cd7c13a…` (older still, before the final battery_osd/pir_sleep rebuilds).

`/system` (mtd4) is mounted read-only while running, so kill `vp_project`, unmount, flash,
reboot in one shot:

```sh
kill $(pidof vp_logcat) 2>/dev/null            # quiet the console flood
kill -9 $(pidof vp_project) 2>/dev/null ; sleep 2
umount /system 2>/dev/null
flashcp -v /mnt/sda0/mtd4_integrated.bin /dev/mtd4
reboot
```

**Safer alternative — U-Boot `sf` (unmounted; interrupt boot to the `=>` prompt):**

```
sf probe
sf erase 0x760000 0x60000
fatload mmc 0:1 0x80600000 mtd4_integrated.bin
sf write 0x80600000 0x760000 0x60000
reset
```

**Automated (PC-driven over COM3):** `tools/flash_integrated.ps1` does the login,
md5 pre-flight gates, live-`mtd4` backup, kill/umount/flashcp/reboot, and aborts before the
destructive step on any mismatch.

The device may perform **one harmless auto-reboot** mid first-boot (AIC WiFi not ready on
the first try); the second boot is clean. This is stock AIC behaviour, not the shim.

---

## TFTP flash — no SD swap needed  [verified: device has busybox `tftp`]

The device's busybox **does have a `tftp` client** (it lacks `wget`/`nc`/`base64`/`gunzip`/
`cmp`/`od` — this corrects an older "no tftp" note in the serial section below). So once the
camera is on WiFi you can pull an image over the **network** and `flashcp` it — no card
reader, no SD juggling:

```
# PC (any LAN host the camera can route to): serve the file on a high UDP port.
python tools/tftp_server.py                 # serves builds/exfil/ on UDP 6900 (see below)
```
```sh
# device (serial root shell), <PC_IP> = the serving host:
cd /tmp
tftp -g -r mtd4_integrated.bin -l /tmp/mtd4_integrated.bin <PC_IP> 6900
md5sum /tmp/mtd4_integrated.bin             # MUST equal 949ddff9eef4a6cdfd215ec1169c74eb
kill -9 $(pidof vp_project) 2>/dev/null ; sleep 2
umount /system 2>/dev/null
flashcp -v /tmp/mtd4_integrated.bin /dev/mtd4
reboot
```

- The same channel delivers the daemon binary and its conf for a **run-it-now** test without
  reflashing — `tftp -g` them into `/tmp`, `chmod +x`, run. See
  `builds/features/onvif_rtsp/DEPLOY.md`.
- Port: `tools/tftp_server.py` serves `builds/exfil/` (DEPLOY.md uses **6900**; some helper
  scripts use 6969 — match the port you launched). Scripts:
  `tools/{deploy,launch,redeploy}_onvifd.ps1`.
- Stop the PC RTSP proxy first if it's running — the camera's `:81` is single-client, and the
  daemon will contend with the proxy for it.

This is the SD-free path; the SD `flashcp` above and the U-Boot `sf` path remain valid.

---

## Auto-start on boot — how the daemon and shims come up  [verified]

The integrated image's `/system/bin/vp_project` wrapper (which PATH-shadows the real binary —
[ARCHITECTURE.md](ARCHITECTURE.md) §2/§3) does five things at boot, in one place:

1. **NOP the `create_web` onboarding gate** in the RAM copy (`dd … seek=514932 count=4`) so
   `camweb` can bind `:81`. On-disk `/usr/bin/vp_project` is never touched.
2. **OTA block** (backgrounded, after `vnet0` gets an IP): add a `224.0.0.0/4` multicast
   route (so WS-Discovery egress works) **then** two `/1` reject routes (LAN `/24` stays
   reachable, the cloud does not).
3. **Start `cam_onvifd`** (backgrounded): poll `netstat` until `0.0.0.0:81` is listening,
   then — because `/system` is a read-only squashfs and the packaged binary may lack `+x` —
   **copy `/system/bin/cam_onvifd` → `/tmp/cam_onvifd`, `chmod 0755`**, and run it with
   `--conf /system/etc/cam_onvifd.conf` (logs to `/tmp/cam_onvifd.log`).
4. **One `LD_PRELOAD`** listing all four shims, `camweb.so` first (its `unsetenv` guard
   keeps WiFi alive — [ARCHITECTURE.md](ARCHITECTURE.md) §3).
5. `exec /usr/bin/vp_project`.

So on a cold boot the camera comes up with WiFi, `:81`, the four shim features, the OTA block,
and RTSP `:554` + ONVIF `:80` all live — **verified after a real cold boot** (RTSP h264
2304×1296 15 fps, ONVIF up, snapshot up, self-heal on the ~123 s EOF measured at ~0.38 s). The
`/tmp` staging means the daemon is **not** persistent by itself; persistence comes from it
being baked into the flashed `/system` image and re-staged by the wrapper each boot. The
per-unit `devpw`/`vuid` are baked into `/system/etc/cam_onvifd.conf` at build time via
`build_integrated.sh`'s `DEVPW=`/`VUID=` env (public repo ships `CHANGE_ME`). Full wrapper
source: `builds/features/integrated/build_integrated.sh`.

---

## Revert (always available, fully reversible)

**Back to stock — flash the `mtd4` backup you took before flashing:**

```sh
kill -9 $(pidof vp_project) 2>/dev/null ; sleep 2 ; umount /system 2>/dev/null
flashcp -v /mnt/sda0/mtd4_stock_backup.bin /dev/mtd4
reboot
```

Because only `mtd4` was ever touched, restoring your own backup returns the camera to exactly
its pre-flash behaviour and keeps the WiFi creds intact. (This is why the golden rule is to
**capture `mtd4_stock_backup.bin` first** — it is your guaranteed revert.)

---

## Last resort — full DFU restore (wipes NVS)

Only if the camera won't boot at all. **This erases WiFi creds — you will re-onboard and
re-read the device password.** Procedure (from the hard-won `FLASHING.md` at the repo root):

> **First time / bricked unit?** Getting into BootROM requires opening the camera and shorting
> two flash pins during power-on — see [HARDWARE_DFU.md](HARDWARE_DFU.md) for the disassembly +
> pin-short walkthrough. This section covers the PC-side transfer once `a108:c309` enumerates.

**Tools:** `tools/thingino-dfu/…/thingino-dfu.exe` (bootstrap only, `-b`/`-l`),
`tools/dfu-util/…/dfu-util.exe` (all transfers — `thingino-dfu -w/-r` do **not** work on this
camera; U-Boot's DFU gadget re-enumerates with the same `a108:c309` VID:PID as the BootROM
and `thingino-dfu` mis-detects it as still-in-BootROM → `LIBUSB_ERROR_PIPE`).

Two hard-won facts:
- The U-Boot DFU loader **auto-boots after ~60 s** (bootdelay 0, no console window), so the
  whole 8 MiB transfer must finish inside that window.
- Windows is slow to bind WinUSB to the freshly-enumerated DFU interface, so **grab the
  gadget the instant it's born** — fire `dfu-util -a flash -U/-D` directly in a tight retry
  loop (~120 ms), not a `dfu-util -l` poll loop. A connect that lands early completes 8 MiB
  in ~24 s.

```
# STEP 1 — bootstrap (waits for BootROM)
thingino-dfu.exe -b --wait --cpu t23zn        # --cpu t23zn skips broken auto-detect
# STEP 2 — tight retry loop calling:
#   restore:  dfu-util -a flash -D mycam_full_backup.bin   # YOUR own full-chip backup
#   backup :  dfu-util -a flash -U readback.bin
```

**Trigger BootROM (physical):** short SPI-NOR **pins 5 (DI) + 6 (CLK)**, click the battery in
while shorted, hold ~2 s, release. `VID_A108&PID_C309` enumerates; `-b` grabs it. The
`LIBUSB_ERROR_PIPE` mid-write recurs intermittently — just re-run.

**Verify a write:** re-trigger, bootstrap again, `dfu-util -a flash -U readback.bin`, compare
SHA256 to the flashed image; or boot and confirm on serial.

---

## Serial-transfer notes (getting an image onto the SD without a card reader)

busybox on this device is minimal, but it **does have `tftp`** (correcting an earlier note) —
so the network TFTP-flash above is the preferred SD-free path. It still lacks
`wget`/`nc`/`base64`/`gunzip`, and `od`/`hexdump`/`xxd`/`cmp`/`awk`/`sed`/`tail`/`head`/`cut`/
`xargs`/`killall`/`nohup`. When neither the SD nor WiFi is available, images are transferred
over serial as **octal** via `printf '\NNN'` (busybox `printf` supports it) into `/tmp` or the
SD, then `flashcp`'d. Hard limits learned at the bench:

- Keep octal payload **≤ 64 bytes/line** — lines over ~256 B overflow the tty RX FIFO under
  video load, dropping bytes and wedging a `PS2` continuation (recover with `'` + Enter).
- The console is **flooded** by `vp_logcat`; `kill $(pidof vp_logcat)` to quiet it. Anything
  launched over serial dies when the port closes (login-shell SIGHUP) — do run+verify in one
  port-open session.
- **Verify bytes by md5, not `cmp`** (`cmp` is absent, so a `cmp || …` fallback always fires a
  false result). Read a byte with `dd … count=1 of=/tmp/b` then `md5sum /tmp/b` against a
  PC-side md5-of-byte table. Tools: `tools/pwmem3.ps1`, `tools/xfer_flash_v8.ps1`,
  `tools/reboot_verify_v8.ps1`, `tools/verify_integrated2.ps1`.

---

## The Thingino full-firmware path (Track B) — flashes, but WiFi is blocked

For completeness: a full **Thingino** firmware
(`tools/thingino-qc3_t23n_gc2083_aic8800u.bin`, sha256 `99ae51bda3…`) flashes and
**boots** cleanly via the DFU flow above (GC2083, prudynt, uhttpd, ONVIF; login
root/thingino). **But AIC8800U WiFi never enumerates** — `wlan0` never appears, the
`aic8800_fdrv`/`aic_load_fw` drivers register but never probe (the USB chip doesn't enumerate
on dwc2; likely an AIC8800 **variant** driver/firmware mismatch — the board is labelled
AIC8800**U** but the build ships D80 firmware). This is an open upstream issue
(thingino-firmware #1241). So Thingino gives no network → no RTSP, and the shim-on-stock
approach in the rest of these docs is the one that actually works. **Do not flash the other 4
cameras with Thingino** until AIC8800U WiFi is solved.
