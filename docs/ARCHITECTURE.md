# Architecture

The full technical picture: the hardware, why the deploy channel is what it is, how the
single-wrapper LD_PRELOAD chain works and why order matters, and the media path from the
camera's sensor to a client's RTSP player.

Throughout, facts are marked **[verified]** (reproduced live on the pilot camera or by a
passing offline test) vs **[planned]** (designed / under construction).

---

## 1. Hardware

**Camera:** the **BW4** (VeePai/Vstarcam-family; earlier working notes mislabelled it "QC3"/"BW6" — same hardware). Owner has 5; all work is on one **pilot** (cam #1).

| Block | Detail |
|---|---|
| SoC | Ingenic **T23N**, MIPS32 (o32 ABI, little-endian, mips32r2) |
| Kernel | Linux **3.10.14-Archon** (vermagic `3.10.14-Archon preempt mod_unload MIPS32_R1 32BIT`) |
| libc | **uClibc 0.9.33** (this matters — see the weak-symbol dead-end in §3) |
| Sensor | GalaxyCore **GC2083**, i2c addr **0x37**; H.264 main 2304×1296, sub 640×360 |
| ISP / encoder | Ingenic **IMP** core **1.3.0** (`isvp_t23` sdk-lv3, gcc540), statically linked into `vp_project` |
| WiFi | **AIC8800** (labelled AIC8800**U**; firmware ver `20.121.102`, "wifi 10.17.33") over **SDIO**; kernel driver `aic8800_netdrv.ko` (GPL fork of AICSemi `aicwf_sdio`/rwnx + a custom `aic_cdev` char-dev power shim); deps `jzmmc` + `mmc_core` |
| Power MCU | Puya MCU — battery gauge + PIR + deep-sleep. Talks to the AIC over a `cmd6` "channel"; **power-gates the SoC** if it doesn't get a `cmd6` check-in within ~80 s |
| Flash | 8 MiB SPI NOR (25QH64) |
| Power in | **USB-C** (photo `photos/board1_*.jpg`) + solar panel; battery is a separate optional connector. Solar model: sun → USB/panel, dark → battery. |
| Not present | **No ESP32** on the board (this is central — see §4) |

### The AIC power gate [verified]

The AIC8800 cuts SoC power ~80 s after `vp_project` stops sending it `cmd6` frames
(`06 00 01 00 00`). This is a **battery-saving** gate, not a security feature. It is why
you can't simply kill `vp_project` and run your own streamer — the board reboots. There is
also a hardware `/dev/watchdog` and a userspace `app_wdt`. On the pilot these three
reboot-on-`vp_project`-death sources were all identified and, for the abandoned "replace
vp_project" Track B, defeated with a `cmd6` keepalive + `sleep 3` watchdog handover
(bench-proven: SoC survived past 88 s, 355 keepalive frames, zero reboots). **The shipped
approach avoids all of this by keeping `vp_project` alive and healthy** and only
`LD_PRELOAD`-augmenting it.

### Flash / partition map (8 MiB NOR) [verified from live `/proc/mtd` + boot log]

| mtd | offset | size | contents | format | touched by this project? |
|---|---|---|---|---|---|
| `mtd0` | `0x000000` | — | **U-Boot** (also carries the AIC8800 WiFi stack + fw loader, and the **HW-LZMA** decoder) | — | never |
| `mtd2` | `0x0e0000` | ~4.32 MB usize | **kernel** uImage `Linux-3.10.14-Archon` (comp=5 = jzlzma, dict `0x10000`) | jzlzma | never |
| `mtd3` | `0x2e0000` | `0x480000` (4608 K) | **rootfs** RAM disk (`root=/dev/ram0 rdinit=/linuxrc`): `/etc/init.d/rcS`, `/bin`, `/lib`, **`/usr/bin/vp_project`** | 16-byte header + **jzlzma** newc cpio | **no** (reflash path is dead — §2) |
| `mtd4` | `0x760000` | `0x60000` (384 K) | **`/system`**: `www/` CGI docroot, voice-prompt assets | **XZ squashfs** | **YES — the only partition we write** |
| — (appfs) | `0x4c0000` | — | proprietary app blob (magic `0x7eeb4a3f`) | proprietary | never |
| `mtd5` | (param) | — | **NVS / param** — WiFi creds + **device admin password** (PCAM format) | vendor | never (and **DFU must not wipe it** — §FLASHING) |

> Historical note / pitfall: an early carve used offset `0x2c0000` (inside the kernel) for
> the rootfs and produced 185 files with **no** `vp_project`. The correct rootfs is
> **`mtd3 @ 0x2e0000`, size `0x480000`**. The memory notes and older docs occasionally show
> the wrong `0x2c0000`; treat `0x2e0000` as authoritative.

### `vp_project` — the vendor app [verified]

`/usr/bin/vp_project` (PID 100 on stock, md5 `5a8ea3edc499ffe644efaf2700ec037d` ==
our RE copy `builds/exfil/vp_project.bin`) is the whole camera application: WiFi/AIC
bring-up, ISP/sensor init, H.264 encode, session/P2P, battery/power, OSD. It is
**ET_EXEC / non-PIE**, loaded at a fixed base `0x400000` (no ASLR on kernel 3.10), so every
runtime virtual address in this documentation is **fixed** — that is what makes the shims'
absolute `lui/ori` address materialization work. Strings are absolute; **calls are
gp-relative through the GOT** (`lw $t9,%call16($gp); jalr $t9`, GOT@`0x7f3080`,
GP=`0x7fb070`), and the video hubs are reached via `.data` function-pointer tables — which
is why naive top-down xref from `main` finds nothing and the RE tools
(`tools/app_gotpage.py`, `app_constref.py`, `app_dumpall.py`) resolve the call graph by
reading GOT words and are **gap-tolerant** (capstone stops at embedded data words).

---

## 2. The deploy channel — why only `mtd4` `/system` XZ-squashfs

**The `mtd3` (rootfs) reflash path is permanently dead. [verified]** We fully cracked
Ingenic's proprietary `jzlzma` compression (`tools/jzlzma.py` — decode + a greedy/lazy/rep
encoder, round-trip-verified against our own decoder). But the **T23 decompresses the
`mtd3` ramdisk with an on-SoC *hardware* LZMA engine**, far stricter than any software
codec. Flashing *any* re-compressed rootfs — even a byte-for-byte **no-op** recompress of
stock — boot-loops with:

```
lzma hardware error CTRL register value : 0x00004000
rootfs error, lzma dec timeout
```

We ruled out match-distance, padding byte (`0x00` vs `0xFF`), and overshoot as causes; it
is the HW decoder rejecting our bitstream. Matching it bit-for-bit is impractical.
**Conclusion: never reflash `mtd3`.** (Consequences: any doc that describes shipping
`wifi_sd`/`pir_sleep` via an `mtd3` rcS `LD_PRELOAD` chain — including those features' own
READMEs — is describing a channel **that never boots**. The integrated image supersedes
them; see §3.)

**`mtd4` `/system` is the escape hatch. [verified]** `/system` is a **standard XZ
squashfs**, not jzlzma, so it is rebuilt with ordinary `mksquashfs -comp xz -b 131072` and
u-boot/kernel never touch the HW-LZMA path for it. Crucially, stock `rcS`:

```
PATH=/system/bin:/bin:/sbin:/usr/bin:/usr/sbin      # /system/bin is FIRST
LD_LIBRARY_PATH=/system/lib:/usr/lib
mount -t squashfs /dev/mtdblock4 /system            # mtd4 mounted before launch
… vp_project …                                      # launched by BARE NAME
```

Stock `/system` has **no `bin/` directory**. So we add **`/system/bin/vp_project`** — a
tiny shell wrapper that, by PATH order, **shadows** `/usr/bin/vp_project`. The wrapper sets
up the shims and `exec`s the real binary. Only `mtd4` (384 K) is rewritten; `mtd0` (boot),
`mtd2` (kernel), `mtd3` (rootfs), `mtd5` (NVS) and the appfs stay **byte-for-byte stock**,
making every deploy revert-safe by simply flashing stock `mtd4` back.

**PATH-shadowing verified live:** `command -v vp_project` resolved to the `/system/bin`
copy, and an on-device `dd` patch of `/usr/bin/vp_project` reproduced our offline-patched
md5 exactly.

---

## 3. The single-wrapper LD_PRELOAD chain

All four features are delivered by **one** `/system/bin/vp_project` wrapper with **one**
`LD_PRELOAD` assignment, so nothing can clobber anything (an earlier per-feature design had
`battery_osd`'s wrapper do a bare `LD_PRELOAD=/system/lib/battery_osd.so`, which would
*replace* the chain — that is dissolved by putting everything in one place).

```sh
#!/bin/sh
REAL=/usr/bin/vp_project
# (1) NOP the create_web onboarding gate in the RAM copy so camweb can bind :81
[ -f "$REAL" ] && printf '\000\000\000\000' | dd of="$REAL" bs=1 seek=514932 count=4 conv=notrunc 2>/dev/null
# (2) background: once vnet0 has an IP, install OTA-block reject routes (LAN /24 stays up)
( while ! ifconfig vnet0 2>/dev/null | grep -q 'inet addr'; do sleep 2; done
  route add -net 0.0.0.0   netmask 128.0.0.0 reject
  route add -net 128.0.0.0 netmask 128.0.0.0 reject ) &
# (3) one LD_PRELOAD listing all four shims — camweb FIRST (see below)
export LD_PRELOAD="/system/lib/camweb.so /system/lib/wifi_sd.so /system/lib/pir_sleep.so /system/lib/battery_osd.so"
# (4) hand off to the real, unmodified binary (still lives in stock mtd3)
exec "$REAL" "$@"
```

- `seek=514932` = `0x7db74` = vaddr **`0x47db74`** — the `create_web` onboarding gate. On
  an **onboarded** cam this gate self-bails (it only returns "enabled" when the stored
  password equals the factory `"888888"`, which onboarding replaced with a random value), so
  the NOP is **required** for `:81` to bind. It edits only the **RAM** copy of the binary —
  the on-disk `/usr/bin/vp_project` in `mtd3` is never modified. [verified]

### Why `camweb` must lead — the `unsetenv` protection [verified, hard dependency]

All four `.so` import `pthread_create`. Inside `vp_project` that resolves fine (it links
uClibc + libpthread). But `vp_project` spawns **busybox** children — `udhcpc` (DHCP),
`ifconfig`, `sh` — via `system()`/`/bin/sh`, and **busybox has no libpthread**. Because
uClibc binds **eagerly** at load, any busybox child that *inherits* `LD_PRELOAD` fails to
resolve `pthread_create` and dies:

```
/bin/busybox: can't resolve symbol 'pthread_create'
```

The killer consequence: `udhcpc` never runs → **no DHCP → no WiFi** (`vnet0` absent, empty
route table). This is exactly what broke `camweb` v1/v2, and (subtly) also broke the very
first WiFi *onboarding*.

**The fix that shipped (v4/v5):** `camweb`'s constructor calls **`unsetenv("LD_PRELOAD")`
first**, before `vp_project` spawns any child. `vp_project`'s children exec with a clean
environment and load busybox normally; `camweb` (and the other three shims) stay active
**inside** `vp_project` because they were already loaded when the process started. This only
works if the `unsetenv`-doer runs first, so **camweb must stay first in the chain**
(verified: only `camweb_v5.so` exports `unsetenv`; the other three do not).

> Dead-ends worth knowing: (a) a plain 45-second worker delay did **not** help — the failure
> is at **load**, before any shim code runs. (b) declaring the pthread/sleep imports
> `__attribute__((weak))` (camweb v3) so busybox would load the `.so` with NULL stubs
> **failed** on this uClibc 0.9.33 (`can't load library 'camweb_v3.so'` — weak-undef not
> honored). The `unsetenv` route is the only one that works here.

### Shim target map — disjoint, no overlap [verified by disassembly]

| shim | reads | writes / calls | worker |
|---|---|---|---|
| `camweb.so` | — | `create_web` `0x47db44`, web-fd `0x7e8db8`; RAM NOP `0x47db74` | 1 thread |
| `wifi_sd.so` | `/mnt/sda0/wifi.ini` | `0x453a2c(ssid,pwd,2)` → sink `0x48e134` → creds `0x8111b0`, connect `0x48ba1c` | 1 thread |
| `pir_sleep.so` | getter `0x48d0ec` | config `0x821858`=1, `0x821859`=50 (base `0x81e850` via getter `0x5d7fd8`) | 1 thread |
| `battery_osd.so` | getter `0x48d0ec` | OSD text `0x80d814`, dirty bit `0x80d788` bit7 (region base `0x80d774`, stride `0x6f0`) | 1 thread |

All BSS/VA targets are disjoint; four independent detached workers; each `.so` is a
freestanding `DYN` MIPS o32 mips32r2 PIC object with **no NEEDED libs**, an `.init_array`
constructor, and undefined syms limited to the expected libc functions (resolved from
`vp_project`'s uClibc at load). Full details per shim in [FEATURES.md](FEATURES.md).

---

## 4. The media path

### 4a. On the device: the dead-code web server [verified]

Because there is **no ESP32**, the app's local web/CGI server never starts:
`vp_web_create_socket` (`0x47db44`) is **present but unreachable** — its bootstrap is
compiled out (no `if(esp32)` branch, no reachable caller). It is a **no-arg, idempotent**
routine that binds a listen socket and stores the fd at `0x7e8db8` (`-1` until bound).
`camweb.so` spawns a worker that simply **calls it** in a retry loop until the fd goes
valid. Live result: it binds **`0.0.0.0:81`** (LAN-reachable — *not* `127.0.0.1` as the
original RE assumed and as the stale `camweb.c` comment still says).

Once `:81` is up, the camera's own **`livestream.cgi`** serves H.264:

```
GET /livestream.cgi?loginuse=admin&loginpas=<devpw>&user=admin&pwd=<devpw>
    &vuid=<VUID>&streamid=10&substream=2&audiostream=0&filename=
```

- `streamid=10 & substream=2` = **main** stream (2304×1296 H.264 High). `streamid=0` yields
  nothing.
- **On success the server streams the raw `0xA815AA55` VStarcam frame protocol immediately,
  with NO HTTP status/headers.** Only on *failure* (bad auth/params) does it return an
  HTTP-200 wrapper whose body is `result=-1;…`. The proxy exploits this: it peeks the first
  4 bytes — frame magic ⇒ pass through; anything else ⇒ raise a clear auth/param error.

### 4b. Device password [verified]

Auth requires `loginuse=admin` **and** `loginpas` == the **stored admin password**, which
`vp_project` copies from `mtd5` NVS into the `.bss` global **`0x81E988`** at startup
(`strcpy` by fn `0x42e00c`; PLT stubs resolved via objdump: `0x7d46d0`=strcmp,
`0x7d4590`=sprintf). This value is **per-unit random**, set during onboarding — it is **not**
the factory `"888888"` (an earlier "default-password gate" theory was wrong; fn `0x42a230`
is only the failure-response builder). No memory patch is needed — just authenticate with
the real value, read live:

```sh
dd if=/proc/$(pidof vp_project)/mem bs=1 skip=8513928 count=20   # 8513928 = 0x81E988
```

On the pilot this read `<your-unit-devpw>`. **Re-read it if the camera is ever re-onboarded.**

### 4c. The VStarcam frame protocol [verified — reconstructed from `vp_project`]

`tools/vstarcam_frame.py` implements the 32-byte frame header the app's session reader
(`0x42eec0`) expects and its builders (`0x42f688`, `0x42ffc0`, `0x43002c`, `0x4302d8`)
emit:

```
u32 @0x00  magic = 0xA815AA55   (55 aa 15 a8 on the wire)
u8  @0x04  type
u8  @0x05  codec/stream (4 = H.264 observed)
u32 @0x0c  ts_field  (device timestamp; unit unknown — the proxy ignores it)
u32 @0x10  size      (payload bytes following the 32-byte header)
u8  @0x16  = 0xff    (sentinel on some builders)
```

Video payloads are **Annex-B** H.264 (`00 00 01` start codes) as the IMP encoder emits.
`FrameReassembler` slices frames by `size@0x10` and resyncs to the next magic on
corruption.

### 4d. On the PC: the RTSP proxy [verified — now the dev/fallback path]

> **Superseded as the primary path.** The RTSP endpoint now lives **on the camera**
> (`cam_onvifd`, §4e / [ONVIF.md](ONVIF.md)). The PC proxy below is kept as a dev/debug and
> fallback tool — it is a byte-for-byte Python sibling of the daemon's C code, and the daemon
> reproduces its architecture and fixes. Everything in this section still holds when you run
> the proxy; it just isn't how a deployed camera streams anymore.

`tools/cam_rtsp_proxy.py` (+ `rtsp_server.py`, `vstarcam_frame.py`) turns `:81` into a
standard `rtsp://<pc>:8554/live` (SDP `H264/90000`, `profile-level-id=640032`, sprop from
the live SPS/PPS; RFC-6184 FU-A fragmentation for the large IDRs):

```
python tools/cam_rtsp_proxy.py --connect 192.168.100.106:81 \
    --user admin --pwd <devpw> --vuid <VUID> \
    --streamid 10 --substream 2 --port 8554 --name live --fps 15
```

**Architecture (after a QA→fix loop, `builds/qa_findings.md`):** ONE dedicated
`camera-reader` thread pulls `livestream.cgi` continuously and fans access units out to
**per-client bounded queues** (drop-oldest); each RTSP client drains only its own queue, so
camera reads are never gated behind RTP pacing/transmit. Bugs fixed (all verified):

1. **"one frame then freeze"** — a single shared single-use generator coupled the camera
   read to RTP send on one thread; now a decoupled reader + per-client queues.
2. **second client crashed the stream thread** (`generator already executing`) — no shared
   generator anymore; each client gets its own `_Subscriber` queue.
3. **~30 s disconnect** — the RTSP control socket's idle recv-timeout tore down healthy
   viewers; now `socket.timeout` just `continue`s and `GET_PARAMETER` keepalive is
   advertised.
4. **RTP pacing** — device timestamps are ignored; a monotonic 90 kHz clock is synthesized
   at a fixed fps and kept monotonic across reconnects.
5. **camera EOFs every ~123 s** (a firmware cadence) — reader reconnects with a fast 0.3 s
   first-retry backoff (was fixed 2 s) so the visible gap is ~0.5 s (≤ one 4 s GOP), only
   escalating (×2, cap 5 s) if successive connects yield no video.

Verified: 180 s soak = 2701 frames @ 15 fps, VLC held >20 min, 2 concurrent clients OK, no
socket/thread/subscriber leak, working set flat ~20–22 MB, `test_stream.py` **18/18**.
Residual (both minor/by-design): the ~123 s device cutoff is firmware (mitigated to ~0.5 s,
not eliminated); no RTCP emitted on UDP (use the default TCP transport).

### 4e. On the device: `cam_onvifd` — the primary media path [verified]

The RTSP endpoint now lives **on the camera**. `cam_onvifd`
(`builds/features/onvif_rtsp/cam_onvifd`, static MIPS32r2 LE, md5
**`8435ab9bcb6c1c235befcfd498b7cef9`**) is a C port of the exact Python stack in §4d
(`vstarcam_frame.py` + `rtsp_server.py` + `cam_rtsp_proxy.py`). It runs as a client of the
same local `livestream.cgi` — connecting to **`127.0.0.1:81`** on the device (the proxy
connects to `<cam-ip>:81` from the PC) — and re-serves it as:

- **RTSP** on `:554/live` (same H.264, same reader-hub + per-client-queue architecture and
  reconnect-on-EOF behaviour as §4d, ported to C threads).
- **ONVIF** Device/Media/**Event** SOAP on `:80`, plus **WS-Discovery** on UDP 3702 and a
  **snapshot** proxy at `GET /onvif/snapshot`.

`vp_project` itself contains **no** RTSP/ONVIF protocol strings (probed by
`tools/app_onvif.py`), so this is a genuinely new on-device component, not an unlock. It is
**purely additive** — it never stops or patches `vp_project`, so the AIC `cmd6` keepalive
(§1) stays intact and there is no watchdog juggling. It respects the same constraints as the
shims (uClibc-free because it's static; needs the `camweb` `:81` unlock; subject to the AIC
power gate on battery). It's baked into the integrated image and **auto-starts on boot** via
the wrapper (see [FLASHING.md](FLASHING.md) §4). Verified live against a real Synology NVR and
after a cold boot.

Full detail — services, access policy, auth, ops, WS-Discovery caveat, NVR setup — is in
**[ONVIF.md](ONVIF.md)**; discovery specifics in [DISCOVERY.md](DISCOVERY.md).
