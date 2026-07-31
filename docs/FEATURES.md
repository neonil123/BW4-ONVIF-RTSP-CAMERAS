# Features

> **Repo-path & count note.** This page was written against the original working tree, so some
> prose still references paths like `builds/…` and `tools/…` and says "four shims". In **this**
> repo the shim **sources** live under [`src/shims/`](../src/shims) and the **prebuilt binaries**
> under [`bin/`](../bin). The shipped overlay carries **five** shims: the four documented below
> **plus** `mic_capture.so` (native `IMP_AI` mic reader, see **[AUDIO.md](AUDIO.md)**). The
> talk-back shim `speaker_feed.so` is **not** in the default image — it lives on the
> `talkback-experimental` branch because it corrupts the mic (see [AUDIO.md](AUDIO.md)).
>
> Any path below still written as `builds/…` or `tools/…` (other than
> [`tools/windows-flasher/`](../tools/windows-flasher), which *is* published) refers to the
> author's **private** working tree and is **not in this repo** — such lines are marked
> *(not published)*. Nothing you are told to run lives at one of those paths.

The deployable is a set of **`LD_PRELOAD` shims** that augment the stock `vp_project` **without
modifying its binary**, plus **one on-device daemon** (`cam_onvifd`) that re-serves the
camera's H.264 as ONVIF/RTSP. This page documents the four core shims + the daemon. The daemon is
covered in full in **[ONVIF.md](ONVIF.md)**; the summary section is below.

Each shim is a freestanding `DYN` MIPS o32 mips32r2 PIC object with **no NEEDED libraries**,
an `.init_array` constructor, and undefined symbols limited to the libc functions it uses
(resolved at load from `vp_project`'s own uClibc). All target addresses are **fixed** because
`vp_project` is ET_EXEC / non-PIE at base `0x400000` (see
[ARCHITECTURE.md](ARCHITECTURE.md) §1). Addresses were RE'd from a dump of the stock
`vp_project` (md5 `5a8ea3edc499ffe644efaf2700ec037d`, == the on-device binary; the dump
itself is vendor code and is **not published** here).

**The md5s below were recomputed from the files in this repo** (`md5sum bin/*
firmware/mtd4_integrated.bin`) and cross-checked against the same files unpacked out of the
shipped squashfs — they match byte for byte. Where an older README or an earlier revision of
these docs lists a different md5, that older value is stale.

| shim | file | md5 | size | status |
|---|---|---|---|---|
| camweb (v5) | [`bin/camweb.so`](../bin/camweb.so) | `53c59f013e7d1098edec541e330a16fb` | 2,080 B | **verified live** |
| wifi_sd | [`bin/wifi_sd.so`](../bin/wifi_sd.so) | `f3c6c2ab296d5c5cdfbb91fd0d4da53e` | 4,916 B | offline-ok; apply seen live |
| pir_sleep | [`bin/pir_sleep.so`](../bin/pir_sleep.so) | `0cf8afb9053a70c59edaa7471331f8e0` | 2,600 B | offline-ok (thr=50) |
| battery_osd | [`bin/battery_osd.so`](../bin/battery_osd.so) | `119900999bf56f65b1c652a79a0e2a8b` | 3,032 B | verified live (display) |
| mic_capture | [`bin/mic_capture.so`](../bin/mic_capture.so) | `bc49c932a6a84f49a51492c487846d1f` | 5,376 B | verified live (see [AUDIO.md](AUDIO.md)) |

The integrated deployable that carries **all five shims plus `cam_onvifd`**:
[`firmware/mtd4_integrated.bin`](../firmware/mtd4_integrated.bin), 393,216 B, md5
**`949ddff9eef4a6cdfd215ec1169c74eb`** — recomputed from the current file and identical to
the value recorded in
[`firmware/mtd4_integrated.bin.md5`](../firmware/mtd4_integrated.bin.md5), which is the
authoritative reference. Older values that float around in earlier notes — `6cd7c13a…`,
`0b3273fd…`, `6b1203e6…` — are all **stale** and do **not** describe the image in this repo.

The daemon binary itself: [`bin/cam_onvifd`](../bin/cam_onvifd), 226,728 B, md5
**`067d4e3c6d17a29682686ebc3d7aae50`**.

---

## camweb.so — `:81` web-server / RTSP unlock  [verified live]

**What:** starts `vp_project`'s built-in-but-disabled local web/CGI server so
`livestream.cgi` serves H.264 on the LAN. This is the linchpin of the whole RTSP path.

**Key addresses:**
- `vp_web_create_socket` = **`0x0047db44`** — no-arg, idempotent; binds the listen socket.
- web listen fd (int) = **`0x007e8db8`** (`-1` until bound).
- `create_web` onboarding gate NOP = file offset **514932** = **`0x7db74`** = vaddr
  **`0x47db74`**. On an onboarded cam the gate (`0x5d7f94`) self-bails, so this NOP is
  **required** for `:81` to bind. The wrapper `dd`-patches only the **RAM** copy.

**Mechanism:** the constructor spawns a worker; after a short delay it calls
`create_web()` in a loop until `*fd >= 0`. Source:
[`src/shims/camweb.c`](../src/shims/camweb.c); the shipped build
([`bin/camweb.so`](../bin/camweb.so), the "v5" build) adds the critical
`unsetenv("LD_PRELOAD")` in its constructor + logging.

**Live facts that corrected the original RE:**
- It binds **`0.0.0.0:81`** (LAN-reachable), **not** `127.0.0.1:81` (the `camweb.c` header
  comment still says 127.0.0.1 — **stale**).
- Version history worth knowing: v1/v2 broke WiFi (LD_PRELOAD cascaded to busybox
  `udhcpc`); v3 weak-symbol approach failed to load on uClibc 0.9.33; **v4 added
  `unsetenv`** (WiFi validated live); **v5** = v4 + the create_web poke + logging (shipped).
  See [ARCHITECTURE.md](ARCHITECTURE.md) §3 for the full `unsetenv` reasoning — **camweb
  must stay first in the LD_PRELOAD chain**.

**Delivery of just camweb (RTSP-only):** the author kept a camweb-only `mtd4` image
(`mtd4_camweb_v8.bin`, md5 `fb1266aa06d4faa7d1efa46c844d304f`) as the **primary revert**
target (back to a known-good RTSP-only state), staged on the SD during integrated flashing.
That image is *(not published)* in this repo — only the full integrated image ships. To
build an equivalent camweb-only overlay yourself, run
[`src/build_integrated.sh`](../src/build_integrated.sh) with only `camweb.so` in the
`LD_PRELOAD` list, or use the stock-restore path in [FLASHING.md](FLASHING.md) to revert.

---

## wifi_sd.so — SD-card WiFi onboarding  [offline-verified; apply observed live]

**What:** drop a text file on the SD card and the camera joins your WiFi on boot — no app,
no cloud. **No video is ever recorded to the SD** (the shim only reads a file and calls the
vendor's WiFi-connect; it makes zero calls into the record module around `0x6b39xx`).

**`wifi.ini`** at SD root (`/mnt/sda0/wifi.ini`, or `wifi.txt`):

```
SSID=YourNetworkName
PASSWORD=YourWiFiPassword
```

- Keys case-insensitive; `=` or `:` separator. Aliases: SSID = `ssid`/`wifi_ssid`;
  password = `password`/`pass`/`psk`/`key`/`wifikey`/`wifi_wpa_psk`.
- Tolerant of CRLF, whitespace, blank lines, `#`/`;` comments; SSID may contain spaces/`=`.
- Validation (else the file is ignored — safe no-op): SSID 1..32 chars; password empty
  (open) or **8..63** chars (WPA2 minimum). Parser is host-unit-tested (25 cases,
  `test_parser.c` → `ALL TESTS PASSED`) and shared verbatim with the shim via
  [`src/shims/parse_ini.h`](../src/shims/parse_ini.h), which
  [`src/shims/wifi_sd.c`](../src/shims/wifi_sd.c) `#include`s. The header ships; the
  host-side unit test itself is *(not published)*.

**Key addresses:**
- WiFi-connect entry = **`0x00453a2c`** `int fn(char *ssid, char *pwd, int mode)`; the shim
  calls it with **mode `2`** (what the vendor factory thread uses). Arg convention confirmed
  across 4 call sites (`0x410a78`, `0x4341f4`, `0x48a190`, `0x452dc4`).
- It feeds `0x48e134(ssid,pwd)` — the common sink of every onboarding path (factory/QR/
  smartlink) — which stages creds in the wifi struct at **`0x8111b0`** and tail-calls the
  connect at **`0x48ba1c`**.
- **State gate:** returns a negative error with **no side effects** unless the wifi state
  global at **`0x7e8c20 == 2`** (STA-ready). The shim retries (bounded, 24 × 3 s) so the
  creds apply once the radio is ready.

**On success** the shim renames `wifi.ini` → `wifi.ini.applied` (won't re-apply; signals
success). **Security:** the password is on the card in plaintext — delete
`wifi.ini.applied` or wipe the SD after the camera is online.

> Deploy note: this feature's own README describes an **`mtd3` rcS repack** deploy — that
> channel is **dead** (HW-LZMA, see [ARCHITECTURE.md](ARCHITECTURE.md) §2). It actually
> ships via the integrated `mtd4` image. Live, `wifi_sd` was observed switching the pilot to
> a new AP; the full **un-onboarded** first-join flow needs a factory-fresh unit to gate.

---

## pir_sleep.so — low-battery PIR-wake sleep  [offline-verified; low-branch needs bench]

**What:** when the battery drops **below 50 %**, the camera enters the vendor low-power/AOV
state (deep-sleep, wakes on **PIR** motion, streams, sleeps again), resuming normal
streaming once the battery recovers (vendor **+10 %** hysteresis → ~60 %). It enables a
real, already-compiled vendor feature (`Smart_Electricity_Sleep`) by writing **two config
bytes** — it never calls the sleep path itself and touches no exit path.

**Key addresses:**

| param | config offset | fixed VA | meaning |
|---|---|---|---|
| `Smart_Electricity_Sleep_Switch` | +0x3008 | **`0x00821858`** | 0=off, 1=on |
| `Smart_Electricity_Threshold` | +0x3009 | **`0x00821859`** | percent (vendor-valid 30..100) |

- Config struct base = **`0x0081e850`** (from getter `0x5d7fd8`: `lui $v0,0x82; addiu -0x17b0`).
- The shim asserts `switch=1 / threshold=50` after a ~12 s settle and **re-asserts every 4 s**
  so a later `app_param` reload can't leave it disabled.
- Vendor decision routine **`0x45a9c4`** (polled from `vp_lpc_low_bat_check` `0x4598xx`):
  ```
  batt = 0x48d0ec()                      ; battery % (MCU getter, same as OSD/app)
  if (session->0xcf0 == in-call) return  ; never sleeps while a client streams
  if (config[0x3008] != 1) return        ; switch must be ON
  thr = config[0x3009]
  if (batt <  thr)     -> 0x48e92c(1)    ; ENTER SLEEP (MCU command 0x30)
  if (batt >  thr+10)  -> 0x48e92c(0)    ; WAKE (10% hysteresis)
  ```
  `0x48e92c` sends **MCU command 0x30** via `aic_channel_msg_send` (`0x48b874`) — the
  vendor's own tested deep-sleep + PIR-wake power path.

**Threshold** = `#define THRESHOLD` in [`src/shims/pir_sleep.c`](../src/shims/pir_sleep.c);
the shipped [`bin/pir_sleep.so`](../bin/pir_sleep.so) (md5 `0cf8afb9…`) is **50** (an earlier
`pir_sleep.prev.so`, md5 `56bdd5d5…`, was 30 — *(not published)*).
Vendor-valid range 30..100; the +10 % wake hysteresis is vendor code (changing it needs a
`.text` patch — not done).

> Two internal inconsistencies to be aware of (flagged, not papered over): (a) the original
> per-feature README prose and its objdump snippet still said `li 30`/`threshold=30` in
> places even though the shipped build is 50 — trust the md5 (`0cf8afb9` = 50). (b)
> **Battery-source caveat:** the OSD/app `%` historically came from MCU byte `0x7fbf58`,
> while the sleep decision uses getter `0x48d0ec` (direct byte `0x811ee0` if 1..254, else a
> voltage curve of word `0x811edc`). On the pilot, `0x7fbf58` is **stuck at 0**, so the
> getter is the only real source and both OSD and sleep now agree on it. If a unit's two
> fields diverge, "threshold=50" may not equal "OSD 50 %" — read `0x7fbf58` vs `0x811ee0`
> and adjust `THRESHOLD`.

By design, a viewer **holding a stream** below the threshold keeps the camera awake
(`session->0xcf0` guard) — intended ("wake and stream on motion").

---

## battery_osd.so — real battery % on the video OSD  [verified live (display)]

**What:** overlays the real battery level on `vp_project`'s on-screen timestamp, e.g.
`2026-07-28 14:43:00 90%`. The shim makes **no call into `vp_project`** — it only writes the
same two OSD descriptor fields the app itself writes, at fixed BSS VAs, after a start-up
delay + a "region populated" wait. Worst case is a cosmetic glitch, not a crash.

**Value source (the important live finding):** the MCU `percentage` byte **`0x7fbf58` is
dead on this unit** (stuck at 0 on a healthy battery); so is the sleep "direct %" byte
`0x811ee0`. The only real data is the voltage word `0x811edc` (e.g. 416 → 4.16 V → ~90 %).
So the shim calls the **vendor battery getter `0x48d0ec`** (o32, no args, returns % in
`$v0`; returns `0x811ee0` if 1..254 else `curve(0x811edc)` via `0x48b068`) as an absolute
function pointer — the **same value the vendor sleep logic and the app use**, so OSD and
sleep agree. `$gp` is saved/restored around the call; no new NEEDED lib.

**OSD write targets:**
- Channel-0 OSD region array: base **`0x0080d774`**, stride **`0x6f0`**, indices 0..2.
- idx0 = the **primary timestamp overlay**. Its strftime text buffer =
  `0x80d774 + 0*0x6f0 + 0xa0` = **`0x0080d814`** (`char[64]`); redraw dirty flags =
  `+0x14` = **`0x0080d788`** (u16, set **bit7** = `0x0080`), exactly as
  `osd_region_set()` (`0x0047ed48`) does (`strncpy` dst+0xa0 max 63; `lhu/ins…,7,1/sh`).
- The region render runs `strftime()` on descriptor+0xa0 every refresh (that is how the
  clock ticks). The shim rewrites idx0's format to a battery-bearing strftime string and
  re-asserts the dirty bit every few seconds so `NN` tracks the battery.

**Label is letter-free — the partial-glyph-font finding [verified live]:** live frames
render all-digit dates and `%` perfectly, but `BAT` came out as `A` ('A' shows; 'B'/'T'
vanish) because the OSD font has a **partial glyph set** (missing letter glyphs) that no
buffer content can fix. So the shipped label is `"%Y-%m-%d %H:%M:%S NN%%"` (strftime renders
`%%`→`%`), built only from proven-rendering glyphs (digits, `-`, `:`, space, `%`). Verified
OSD read a real ~98 %. Re-enable a text label with `-DOSD_LABEL='" BAT "'` once a unit's
real font charset is known.

**Build knobs ([`src/shims/battery_osd.c`](../src/shims/battery_osd.c)):** `-DOSD_IDX=0|1|2`
(0 = timestamp merge, default), `-DOSD_START_DELAY=<s>`, `-DOSD_PERIOD=<s>`. An earlier
broken build, `battery_osd.prev.so` (md5 `f9892687…`, rendered `… A 0%`), was kept for
reference in the working tree and is *(not published)*.

> The strftime **output** buffer is 256 bytes on the stack (`strftime(out,256,…)`
> `0x47e5e8`), so the ~28-char merged line never truncates — an earlier "small output
> buffer" residual risk was mistaken.

---

## cam_onvifd — on-device ONVIF/RTSP daemon  [verified live]

**What:** a static MIPS C daemon that runs **on the camera** and re-serves the local `:81`
H.264 as **standards RTSP `:554/live`** + **full ONVIF `:80`** (Device/Media/Event SOAP +
WS-Discovery + snapshot). It is the **primary media path**; the PC proxy is now a fallback.
Verified live against a real **Synology Surveillance Station** NVR and after a **cold boot**
(auto-starts from the flashed image).

- Binary: [`bin/cam_onvifd`](../bin/cam_onvifd), md5 **`067d4e3c6d17a29682686ebc3d7aae50`**,
  226,728 bytes, static MIPS32r2 LE (`Type: EXEC`, no `PT_INTERP`).
- Source: [`src/onvif_rtsp/src/`](../src/onvif_rtsp/src) (C ports of the PC-side Python proxy
  stack, plus `onvif_soap.c`, `onvif_wsd.c`, `wsse.c`, `httpauth.c`, `md5.c`,
  `h264_sps.c`). Build: [`src/onvif_rtsp/build.sh`](../src/onvif_rtsp/build.sh)
  (WSL musl MIPSEL cross).
- Config: `/system/etc/cam_onvifd.conf` (template:
  [`src/onvif_rtsp/cam_onvifd.conf.example`](../src/onvif_rtsp/cam_onvifd.conf.example)) —
  per-unit `devpw`/`vuid` (for the `:81` handshake) + `onvif_user`/`onvif_pass` (what the NVR
  types, default `admin`/`admin`).
  [`src/build_integrated.sh`](../src/build_integrated.sh) takes `DEVPW=`/`VUID=` env; the
  shipped image carries `CHANGE_ME` placeholders.
- **Purely additive** — never stops/patches `vp_project`, so the AIC keepalive stays intact.

Access policy in one line: **all discovery/capability/activation/event ops are pre-auth open;
only `GetStreamUri` requires auth** (WS-Security UsernameToken OR HTTP Digest OR Basic — any
accepted). This is what real Profile-S cameras do and what Synology needs. Full detail
(services, ops, auth, snapshot, WS-Discovery caveat, NVR setup) in **[ONVIF.md](ONVIF.md)**.

---

## Integrated image — five shims + the daemon

[`firmware/mtd4_integrated.bin`](../firmware/mtd4_integrated.bin) (393,216 bytes, md5
**`949ddff9eef4a6cdfd215ec1169c74eb`**) is a single `/system` XZ-squashfs with one wrapper,
all five `.so` in one `LD_PRELOAD`, and `cam_onvifd` (+ its conf) that the wrapper auto-starts
once `:81` is up. See [ARCHITECTURE.md](ARCHITECTURE.md) §3 for the wrapper and chain-order
dependency, and [FLASHING.md](FLASHING.md) §4 for the flash/revert/auto-start procedure.

Components embedded in the image, **verified byte-identical** to the copies in
[`bin/`](../bin) by unpacking the shipped squashfs and re-hashing every member:

| member in image | md5 | size |
|---|---|---|
| `/system/lib/camweb.so` | `53c59f013e7d1098edec541e330a16fb` | 2,080 B |
| `/system/lib/wifi_sd.so` | `f3c6c2ab296d5c5cdfbb91fd0d4da53e` | 4,916 B |
| `/system/lib/pir_sleep.so` | `0cf8afb9053a70c59edaa7471331f8e0` | 2,600 B |
| `/system/lib/battery_osd.so` | `119900999bf56f65b1c652a79a0e2a8b` | 3,032 B |
| `/system/lib/mic_capture.so` | `bc49c932a6a84f49a51492c487846d1f` | 5,376 B |
| `/system/bin/cam_onvifd` | `067d4e3c6d17a29682686ebc3d7aae50` | 226,728 B |

The image also carries `/system/bin/vp_project` (the 2,816-byte `LD_PRELOAD` **wrapper
script**, not the vendor binary — it shadows the real one via `PATH`), `/system/etc/cam_onvifd.conf`
(with `CHANGE_ME` placeholders), and small `version/` and `www/` stubs.
