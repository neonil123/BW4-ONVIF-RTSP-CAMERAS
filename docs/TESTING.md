# Test Matrix

End-to-end tests for every feature and the whole system: the exact test, the method, the
expected result, and the pass criterion. Each row is tagged with where it runs and its
current result.

**Legend — Result:** ✅ passed live/offline · ⏳ awaiting bench gate · 🧪 offline-only so far ·
🚧 needs an in-progress component. **Method:** *serial* = COM3 root shell · *proxy* = PC
`okam_rtsp_proxy.py` + `ffprobe`/`ffmpeg` · *offline* = host-side unit test/disasm.

> Read the **on-device runnability constraints** (below) before running any serial test —
> this busybox is missing most of the usual tools and several "obvious" checks give **false
> results**.

---

## 0. On-device runnability constraints (READ FIRST)

This busybox lacks `od` `hexdump` `xxd` `cmp` `sort` `awk` `sed` `tail` `head` `cut` `xargs`
`killall` `nohup` `setsid` `wget` `nc` `base64` `gunzip`. It **does** have `tftp` (a client) —
so network image/binary transfer uses TFTP (see [FLASHING.md](FLASHING.md)); an older note
saying "no tftp" was wrong. Consequences for tests:

- **Never use `cmp`.** `cmp` is absent, so a `cmp a b || echo DIFF` always fires `DIFF`
  (false positive). **Fingerprint bytes with `dd` + `md5sum`** instead:
  ```sh
  dd if=/proc/$(pidof vp_project)/mem bs=1 skip=$((0xADDR)) count=1 of=/tmp/b 2>/dev/null
  md5sum /tmp/b          # compare against a PC-side md5(byte 0x00..0xff) lookup table
  ```
- **Read a config/OSD/battery byte** the same way (`skip=$((0xADDR))` into `/proc/<pid>/mem`).
- The console is flooded by `vp_logcat`; `kill $(pidof vp_logcat)` to quiet it. Judge
  "vp_project died" by the debug **flood going silent**, not by scraping output.
- Anything launched over serial dies when the port closes (login SIGHUP) — **run + verify in
  one port-open session**. Detect the shell with `id` → `uid=` (the banner is
  "Zeratul login:", not a prompt). Marker pattern: `echo DN''_x` (embedded quote so the
  match hits the *output*, not the command echo).
- The SD must be inserted **before boot** to mount at `/mnt/sda0`.
- Serial harness scripts: `tools/recon_serial.ps1`, `check_flashtools.ps1`,
  `verify_integrated2.ps1`, `check_shims.ps1`, `reboot_verify_v8.ps1` — DTR/RTS de-asserted,
  marker-bracketed reads, `vp_*` DEBUG spam filtered.

---

## 1. Offline / host-side suite (no camera)  ✅

| # | Test | Method | Expected | Pass criterion | Result |
|---|---|---|---|---|---|
| 1.1 | Frame framing: build → byte-fragmented feed → reassemble | `python tools/test_stream.py` | 3 frames reassembled, payloads + timestamps intact | exact match | ✅ (part of 18/18) |
| 1.2 | Reassembler resync after garbage gap | test_stream | both frames recovered across injected junk | 2 frames, payloads intact | ✅ |
| 1.3 | RTP FU-A packetize/depacketize round-trip (single + fragmented NAL) | test_stream | one marker at AU end, uniform ts/ssrc, NAL bytes preserved | exact | ✅ |
| 1.4 | End-to-end RTSP server ↔ Python client (TCP interleaved) | test_stream | DESCRIBE returns `H264/90000` SDP w/ sprop; IDR AU carries SPS+PPS+IDR | asserts pass | ✅ |
| 1.5 | Full proxy: synthetic VStarcam dump → RTSP → client | test_stream | SDP has sprop from dump; IDR body length preserved | asserts pass | ✅ |
| 1.6 | `wifi.ini` parser (25 good/malformed cases) | `builds/features/wifi_sd/build_wifi_sd.sh` (host) | `ALL TESTS PASSED` | all 25 | ✅ |
| 1.7 | Each shim `.so` is well-formed | `objdump`/`readelf` per `verify.sh`/`verify.py` | DYN MIPS o32 mips32r2 PIC, no NEEDED libs, `.init_array` ctor, UND syms = expected libc, target `lui` immediates present | all shims | ✅ |
| 1.8 | Shim address re-proof vs `vp_project.bin` | `tools/app_gotpage.py -d <addr>`, feature `verify.*` | each target VA materialized by absolute `lui/ori`, no GOT/`$gp` coupling | reproduced | ✅ |
| 1.9 | Image diff: integrated `/system` vs stock | integrated `build_integrated.sh` self-verify | only `bin/vp_project` (wrapper) + 4 `lib/*.so` added; nothing changed/removed; mtd0/2/3/appfs byte-identical | diff = additions only | ✅ |
| 1.10 | Artifact md5s match docs | `md5sum` the built files | integrated=`6b1203e6…`, okam_onvifd=`8435ab9b…`, okabweb_v5=`822795eb…`, wifi_sd=`f3c6c2ab…`, pir_sleep=`0cf8afb9…`, battery_osd=`119900999…` | exact | ✅ |

**`test_stream.py` overall: 18 passed, 0 failed.** Run it before every proxy change (RTSP
regression gate).

---

## 2. Deploy / flash gates  (bench)

| # | Test | Method | Expected | Pass criterion | Result |
|---|---|---|---|---|---|
| 2.1 | SD present + image md5 pre-flight | serial `md5sum /mnt/sda0/mtd4_integrated.bin` | `154ea4fbef9a515c56934f6d39a66f66` | exact; **abort flash on mismatch** | ✅ (gate in `flash_integrated.ps1`) |
| 2.2 | Revert image md5 pre-flight | serial `md5sum /mnt/sda0/mtd4_okabweb_v8.bin` | `fb1266aa06d4faa7d1efa46c844d304f` | exact | ✅ |
| 2.3 | Live `mtd4` backed up before flash | serial `cat /dev/mtd4 > /mnt/sda0/mtd4_backup_live.bin; md5sum` | non-empty, md5 recorded | file present | ✅ |
| 2.4 | flashcp succeeds | serial `flashcp -v … /dev/mtd4` | no `error/failed/busy` in output | clean | ✅ |
| 2.5 | flashcp readback matches | (v8 path) readback md5 == written | equal | exact | ✅ (v8) |
| 2.6 | Revert restores stock behaviour | flashcp `mtd4_system_STOCK.bin` → reboot | boots stock, WiFi intact (NVS untouched) | boots, `vnet0` up | ✅ |

---

## 3. Per-feature functional tests  (bench)

### 3.1 okabweb — `:81` web server / RTSP unlock  ✅

| # | Test | Method | Expected | Pass |
|---|---|---|---|---|
| a | `:81` binds | serial `netstat -ltn` | `tcp 0.0.0.0:81 LISTEN` | ✅ |
| b | LD_PRELOAD present | serial `cat /proc/$(pidof vp_project)/environ \| tr '\0' '\n' \| grep LD_PRELOAD` | lists `okabweb.so` first | ✅ |
| c | create_web NOP applied (RAM) | serial `dd if=/proc/$(pidof vp_project)/mem bs=1 skip=514932 count=4 of=/tmp/b; md5sum /tmp/b` | 4 × `00` (md5 of `00000000`) | ✅ |
| d | livestream serves H.264 (auth OK) | PC: raw `GET /livestream.cgi?…streamid=10&substream=2` | first 4 bytes = `55 aa 15 a8` (frame magic, no HTTP header) | ✅ |
| e | wrong pw / streamid rejected | PC: bad `loginpas` or `streamid=0` | HTTP body `result=-1;…` (no magic) | ✅ |
| f | RTSP end-to-end | proxy → `ffprobe -rtsp_transport tcp rtsp://<host>:8554/live` | `h264 (High) 2304x1296 @ ~15fps` | ✅ (`builds/dvr_feed_frame.jpg`) |

### 3.2 wifi_sd — SD-card onboarding  🧪 / ⏳

| # | Test | Method | Expected | Pass | Result |
|---|---|---|---|---|---|
| a | parser correctness | offline host test | `ALL TESTS PASSED` | 25/25 | ✅ |
| b | join on boot (un-onboarded unit) | put valid `wifi.ini` on SD **before** power-on; boot | joins AP in ~30–60 s; router shows a DHCP lease; `ifconfig vnet0` has `inet addr` | camera on LAN | ⏳ needs a factory-fresh unit |
| c | success marker | serial `ls /mnt/sda0` | `wifi.ini` → `wifi.ini.applied` | renamed | ⏳ |
| d | switch existing AP | valid `wifi.ini` on onboarded unit | applies via `0x453a2c(…,2)`; associates to new SSID | new SSID joined | ✅ (seen switching the pilot to a new AP in the integrated image) |
| e | garbage/missing file is a no-op | boot with no/garbage `wifi.ini` | no crash, no reboot, WiFi unchanged | clean no-op | ⏳ |
| f | **no video recorded to SD** | serial `ls /mnt/sda0` after run | no recorded `.mp4`/media files created by us | none | ⏳ |
| g | state-gate timing | (implicit) applies only when `0x7e8c20==2` | bounded retry 24×3 s covers STA-ready timing | applied once | ⏳ |

### 3.3 pir_sleep — low-battery PIR-wake sleep (threshold 50%)  🧪 / ⏳

| # | Test | Method | Expected | Pass | Result |
|---|---|---|---|---|---|
| a | enable took (switch byte) | serial `dd …/mem skip=$((0x821858)) count=1` → md5 | byte = `01` | ✅ | ⏳ verify live |
| b | threshold byte | serial `dd …/mem skip=$((0x821859)) count=1` → md5 | byte = `50` (0x32) | == 50 | ⏳ |
| c | CGI reports enabled | serial `vp_cgi_low_power` | `smart_sleep_switch:1, smart_sleep_thrd:50` | matches | ⏳ |
| d | re-assert survives config reload | wait >4 s after an `app_param` reload; re-read bytes | still `01`/`50` | stable | ⏳ |
| e | charged battery (≥~55%): no sleep | boot, stream | normal streaming; **no** `received sleep mode event`, no MCU cmd 0x30 | stays awake | ⏳ |
| f | low branch: enter sleep | drain <50% (or temp build `THRESHOLD 95`) | `received sleep mode event`, MCU cmd 0x30, console quiets, power drops | deep-sleeps | ⏳ battery-dependent |
| g | PIR wake | motion at the lens while asleep | SoC wakes; a client can stream again; returns to sleep after idle | wake→stream→sleep | ⏳ (also verify PIR armed: `vp_pir_set_wakeup`; `IBT_Profiles.ini` ships `PIR=off` app-alarm) |
| h | hysteresis recovery | charge above thr+10 (~60%) | stops sleeping, resumes continuous stream | resumes | ⏳ |
| i | session guard | hold a stream while <50% | does **not** sleep mid-stream (`session->0xcf0`) | stays awake | ⏳ |
| j | battery-source agreement | serial read `0x7fbf58` vs `0x811ee0` (both via `/proc/pid/mem`) | if equal, thr=50 == "OSD 50%"; if `0x7fbf58`==0 the getter `0x48d0ec` governs | document the delta | ⏳ (on pilot `0x7fbf58`≡0) |

### 3.4 battery_osd — real battery % on OSD  ✅ (display) / ⏳ (tracking)

| # | Test | Method | Expected | Pass | Result |
|---|---|---|---|---|---|
| a | OSD shows a battery % | proxy → view stream / capture frame | timestamp line ends `… NN%` (letter-free) | NN present | ✅ (`builds/osd_final_frame.jpg`, read ~98%) |
| b | value is real | compare NN to app/MCU `percentage:%d` or getter `0x48d0ec` | NN ≈ vendor value | agrees | ✅ (~98% vs 4.16V) |
| c | clock keeps ticking | watch the overlay | `%H:%M:%S` advances (strftime intact) | ticks | ✅ |
| d | NN tracks charge/drain | over time | NN moves with battery | tracks | ⏳ long-run |
| e | no letter-glyph misdraw | inspect frame | digits/`-`/`:`/space/`%` render; **no** truncation/garbage | clean | ✅ (letter-free label chosen because font lacks `B`/`T`) |
| f | self-heal after bring-up | reboot, wait past start-delay | overlay appears and stays correct (not stuck `0%`) | stable | ✅ (v3 unconditional re-write) |

---

## 4. System regression gates  (bench — the make-or-break checks)

| # | Gate | Method | Expected | Pass criterion | Sev |
|---|---|---|---|---|---|
| R1 | **WiFi survives the 4-shim LD_PRELOAD chain** | after boot: serial `ifconfig vnet0` | `inet addr` present within ~90 s | has IP ⇒ `unsetenv` guard held; **no IP ⇒ revert** | **High** |
| R2 | busybox children load clean | serial: confirm `udhcpc` ran / route table populated | default route present, DHCP lease | not empty | High |
| R3 | okabweb still first / present | serial `…/environ \| grep LD_PRELOAD` | `okabweb.so` leads the list | first token | High |
| R4 | `:81` binds after reboot | serial `netstat -ltn` | `0.0.0.0:81 LISTEN` | present | Med |
| R5 | **Reboot persistence** | `reboot`; re-run R1 + 3.1 | all four shims active, WiFi + `:81` up | all green after reboot | High |
| R6 | OTA/internet blocked | serial `route -n` | two `/1` reject routes present; LAN `/24` still reachable | reject routes present | Med |
| R7 | no unclean-exit reboot loop | watch serial ~2 min | camera stays powered past ~90 s (AIC keepalive intact via healthy vp_project) | no reboot loop | High |
| R8 | mtd integrity | serial `md5sum` of a captured `/dev/mtd0`,`mtd2`,`mtd3` vs stock | unchanged | byte-identical | Med |

---

## 5. RTSP proxy stability tests  ✅ (characterized in `builds/qa_findings.md`)

| # | Test | Method | Expected | Pass | Result |
|---|---|---|---|---|---|
| a | **continuous, not 1-frame** | `ffmpeg -rtsp_transport tcp -i rtsp://…/live -t 180 -f null -` | ~2700 frames @ 15 fps, duration ~180 s | no early freeze | ✅ (2701 frames) |
| b | no ~30 s disconnect | same 180 s run | `client connect`→`disconnect` gap = full run, not 30 s | ≥ run length | ✅ (idle-timeout fix) |
| c | multi-client | two `ffmpeg -t 15` started simultaneously (TCP+UDP) | both produce continuous 226-frame files; no `generator already executing` | 0 tracebacks | ✅ |
| d | slow-client isolation | one client stops reading 25 s | it disconnects `dropped=…` while reader holds 15.1 fps and other client unaffected | no reader starvation | ✅ |
| e | reconnect on ~123 s cutoff | watch `proxy_err.log` ≥2.5 min | `EOF → reconnecting in ~0.3s → opened`; visible gap ~0.5 s | gap ≤ one GOP | ✅ (0.45–0.50 s) |
| f | RTP monotonic | ffprobe across a 180 s soak | `Non-monotonic DTS` = 0 (one benign startup) | 0 | ✅ |
| g | clean GOP / params | ffprobe | 4 s GOP, IDR carries SPS+PPS; `profile-level-id=640032` | correct | ✅ |
| h | no resource leak | 30-min soak + UDP churn | working set flat (~20–22 MB), threads=5, handles plateau, subscribers → 0 | flat | ✅ |
| i | VLC longevity | open in VLC across a run | holds > 20 min | no freeze | ✅ |
| j | UDP source port matches advertised | RTSP probe | RTP src port == advertised `server_port` | match | ✅ |

Known residuals (by design): the ~123 s device cutoff is firmware (mitigated to ~0.5 s, not
removed); no RTCP is emitted on UDP → **use the default TCP transport**.

---

## 6. ONVIF / RTSP daemon  ✅ (harness + offline suites + live cold-boot)

The on-device daemon (`okam_onvifd`) is real and verified. Three test layers.

### 6a. PC acceptance harness — `tools/onvif_test/`  ✅

`onvif_harness.py` proves the daemon is a real, discoverable ONVIF Profile-S camera any NVR
accepts. Four stages: WS-Discovery, ONVIF device/media SOAP, RTSP pull (frame-**count** over
N s — a single decodable frame is an explicit FAIL), and an NVR-compat checklist. Runs today
without the camera against a bundled mock (`mock_onvif_server.py`), and against the live
device with `--target`. Pass criteria: `tools/onvif_test/ACCEPTANCE.md`.

```
PY=C:\Users\neoni\AppData\Local\Programs\Python\Python312\python.exe
%PY% tools\onvif_test\onvif_harness.py --mock                                   # self-contained
%PY% tools\onvif_test\onvif_harness.py --mock --proxy-rtsp rtsp://127.0.0.1:8554/live
%PY% tools\onvif_test\onvif_harness.py --target 192.168.100.106 --user admin --pwd admin   # live device
```

| # | Test | Method | Expected | Result |
|---|---|---|---|---|
| a | WS-Discovery Probe → ProbeMatch | raw-socket + WSDiscovery lib prober | ProbeMatch, `NetworkVideoTransmitter`, XAddrs → device_service | ✅ mock / ✅ live device-side (see caveat) |
| b | ONVIF device/media ops | raw SOAP: `GetDeviceInformation`, `GetCapabilities`, `GetProfiles`, `GetStreamUri` | valid SOAP 1.2, H264 2304×1296 profile, `GetStreamUri` → `rtsp://…/live` | ✅ |
| c | RTSP pull, continuous | `ffprobe` + `ffmpeg` frame count over 5 s | `h264 2304×1296 ~15fps`, ≥ `N*15*0.5` frames | ✅ (76 frames/5 s ≈ 15.2 fps) |
| d | NVR-compat checklist | profile token, VEC=H264, resolution, `rtsp://` URI, RTP transports, port **554** | all present | ✅ |
| e | NVR auto-add (real NVR) | Synology Surveillance Station → Add ONVIF, `:80`, `admin`/`admin` | camera authenticates, populates H264 2304×1296 | ✅ (verified live) |

**Mock run (this box, 2026-07-28): 25 passed, 0 failed, 2 warnings.** Live cold-boot run:
**20/22.** The two warnings/fails are **environmental, not daemon bugs**: (1) the WSDiscovery
*library* finds 0 services because the Windows host's `dasHost` owns UDP 3702 (multicast
doesn't loop back on the same box); the raw-socket prober still validates the unicast reply.
(2) the mock serves RTSP on a high port (554 needs admin on the PC) — the device correctly
serves `:554`. See [DISCOVERY.md](DISCOVERY.md) for the multicast/`dasHost` caveat.

### 6b. Daemon offline suite — `builds/features/onvif_rtsp/test/`  ✅

`bash builds/features/onvif_rtsp/test/run_offline_tests.sh` runs six suites, all passing, no
camera required:

| # | Suite | Proves |
|---|---|---|
| 1 | frame-parser equivalence (`test_frame.c`) | C port byte-identical to `vstarcam_frame.py` on `builds/stream_capture.raw` (46 frames / 52 NALs, one byte at a time) |
| 2 | RTSP protocol equivalence (`test_rtsp_e2e.py`) | drives the compiled daemon with the Python reference's own RTSP client; SPS(21B)+PPS(44B)+IDR(44856B) over TCP-interleaved *and* UDP, incl. reconnect-on-EOF |
| 3 | ONVIF sanity (`test_onvif.py`) | every discovery/Event/activation op succeeds with **zero creds**; `GetStreamUri` 401→authenticates via WSSE / a full HTTP Digest round trip / Basic; wrong passwords rejected; `/onvif/snapshot` JPEG-magic proxy; token-agnostic `GetStreamUri`; unimplemented op → 400 (not 500); WS-Discovery ProbeMatch |
| 4 | resolution equivalence (`check_profile_resolution.sh`) | `GetProfiles`/`GetVideoEncoderConfiguration` report the **SPS-derived** 2304×1296, not the config default |
| 5 | WS-Security vectors (`test_wsse.c`) | SHA-1 FIPS 180-1 KAT + WS-UsernameToken digest cross-checked in Python |
| 6 | HTTP Digest/Basic vectors (`test_httpauth.c`) | MD5 RFC-1321 KAT + the RFC-2617 §3.5 worked example + full challenge→validate round trips |

### 6c. Live device verification (cold boot)  ✅

After a real cold boot of the integrated image: daemon auto-starts (wrapper waits for `:81`,
stages to `/tmp`, runs it); `ffprobe rtsp://<cam>:554/live` = h264 2304×1296 15 fps; ONVIF
SOAP `:80` harness 20/22; SPS-derived resolution 2304×1296; camera-EOF self-heal ~0.38 s;
snapshot proxy returns a real JPEG; Synology adds it natively. See
[ONVIF.md](ONVIF.md) and `builds/features/onvif_rtsp/DEPLOY.md`.

---

## 7. Gaps the matrix cannot yet cover  (pending in-progress work)

- **Audio track** — video-only today. The `:81` audio CGI is an architectural dead-end (the
  handler builds the `0xA815AA55` container but never calls `IMP_AI`, so it emits no samples —
  confirmed by disasm + `builds/features/audio/audio_probe.py` finding zero audio frames).
  Native `IMP_AI` capture (16 kHz/16-bit/mono, frame type=6) is the path; a **mic-contention
  test** (can a 2nd process grab the mic while `vp_project` runs, or `EBUSY`) is pending at the
  bench. No audio tests exist yet. See [AUDIO.md](AUDIO.md).
- **WS-Discovery auto-search across the WiFi/wired boundary** — the daemon answers correctly
  device-side, but multicast rarely crosses a home-LAN segment boundary and the Windows test
  host's `dasHost` owns UDP 3702; both make auto-search unreliable off the camera's segment.
  Manual ONVIF add is the verified path (§6, [DISCOVERY.md](DISCOVERY.md)).
- **Extra OSD metrics** (beyond battery %) — infrastructure (OSD region writer) exists;
  no metrics/tests built.
- **wifi_sd un-onboarded first-join (3.2 b/c/e/f/g)** — needs a **factory-fresh** unit; the
  pilot is already onboarded, so only the "switch AP" path (3.2 d) is proven live.
- **pir_sleep low branch / PIR wake (3.3 f/g/h)** — requires driving the real battery below
  50% and physical PIR motion at the bench; enable/addresses are proven, the power behaviour
  is not yet exercised.
- **battery_osd long-run tracking (3.4 d)** — display verified at a single ~98% point; the
  NN-tracks-drain behaviour over a full discharge is not yet captured.
- **Multi-unit** — everything is on **cam #1**; the other 4 cameras are untouched and
  unverified. Re-read each unit's device password (per-unit random) before deploying.
- **HW-LZMA constraint** blocks any future need to modify the kernel/rootfs — a hard ceiling,
  not a test gap, but it bounds what can ever be delivered outside `mtd4`.
