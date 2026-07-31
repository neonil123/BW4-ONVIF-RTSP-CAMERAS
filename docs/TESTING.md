# Test Matrix — verification record

End-to-end verification of every feature and of the whole system: the exact test, the method
used, the expected result, and the pass criterion. Each row is tagged with where it ran and
what it produced.

> ### What this document is — and what it is not
>
> This is a **record of verification performed on the author's pilot unit (cam #1)**, not a
> runnable test suite you can check out and execute.
>
> Many **Method** entries name tooling that lives in the author's **private working tree** —
> the `tools/` harnesses, the Python proxy/RTSP reference implementation, the offline build
> suites under `builds/`, and captured artifacts (`.raw` dumps, frame `.jpg`s, `qa_findings.md`).
> **None of that is published in this repo.** Every such reference is marked **[not shipped]**
> below, and wherever the same check can be reproduced with a stock tool — `ffprobe`, `ffmpeg`,
> `curl`, `md5sum`, or a plain serial shell — that command is given instead.
>
> **If a path is marked [not shipped], the file is not in this repository — do not try to run
> it.** When in doubt, `git ls-files` is the authority on what actually ships.
>
> The **results are real**: frame counts, fps figures, soak durations, byte values and pass/fail
> marks are what those instruments measured on the pilot camera. They are kept verbatim as
> evidence, even where the instrument that produced them is not published.

**Legend — Result:** ✅ passed live/offline · ⏳ awaiting bench gate · 🧪 offline-only so far ·
🚧 needs an in-progress component. **Method:** *serial* = COM3 root shell · *proxy* = the
author's PC-side RTSP proxy + `ffprobe`/`ffmpeg` **[not shipped]** · *offline* = host-side unit
test/disasm **[not shipped]**. Substitute your camera's address for `<cam-ip>` throughout.

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
- The serial tests below were driven by the author's PowerShell harnesses (`recon_serial.ps1`,
  `check_flashtools.ps1`, `verify_integrated2.ps1`, `check_shims.ps1`, `reboot_verify_v8.ps1`) —
  DTR/RTS de-asserted, marker-bracketed reads, `vp_*` DEBUG spam filtered. **[not shipped]** —
  they are not in this repo. Any serial terminal reproduces them provided DTR/RTS are
  de-asserted (otherwise the camera resets on port open); the shipped
  `tools/windows-flasher/flash-console.ps1` shows the working port settings.
- When pushing bytes over serial as octal `printf '\NNN'`, keep the payload **≤ 64 bytes/line**;
  longer lines overflow the tty RX FIFO under video load and wedge a `PS2` continuation. Full
  serial-transfer constraints: [FLASHING.md](FLASHING.md).

---

## 1. Offline / host-side suite (no camera)  ✅  — *tooling [not shipped]*

> Rows 1.1–1.8 ran against the author's host-side Python reference implementation and build
> tree (`tools/test_stream.py`, `tools/app_gotpage.py`, `builds/**`). **None of that is
> published here.** The rows are retained as a record of what was proven before the C daemon
> existed; the shipped descendant of that reference implementation is `src/onvif_rtsp/`.
> Reader-runnable substitutes are given in the Method column where one exists.

| # | Test | Method | Expected | Pass criterion | Result |
|---|---|---|---|---|---|
| 1.1 | Frame framing: build → byte-fragmented feed → reassemble | `test_stream.py` **[not shipped]** | 3 frames reassembled, payloads + timestamps intact | exact match | ✅ (part of 18/18) |
| 1.2 | Reassembler resync after garbage gap | `test_stream.py` **[not shipped]** | both frames recovered across injected junk | 2 frames, payloads intact | ✅ |
| 1.3 | RTP FU-A packetize/depacketize round-trip (single + fragmented NAL) | `test_stream.py` **[not shipped]** | one marker at AU end, uniform ts/ssrc, NAL bytes preserved | exact | ✅ |
| 1.4 | End-to-end RTSP server ↔ Python client (TCP interleaved) | `test_stream.py` **[not shipped]**. Shipped equivalent: `ffprobe -rtsp_transport tcp rtsp://<cam-ip>:554/live` against a live camera | DESCRIBE returns `H264/90000` SDP w/ sprop; IDR AU carries SPS+PPS+IDR | asserts pass | ✅ |
| 1.5 | Full proxy: synthetic VStarcam dump → RTSP → client | `test_stream.py` **[not shipped]** (the PC proxy it drove is also **[not shipped]** — see §5) | SDP has sprop from dump; IDR body length preserved | asserts pass | ✅ |
| 1.6 | `wifi.ini` parser (25 good/malformed cases) | host self-test in the author's `build_wifi_sd.sh` **[not shipped]**; the parser itself ships as `src/shims/wifi_sd.c` | `ALL TESTS PASSED` | all 25 | ✅ |
| 1.7 | Each shim `.so` is well-formed | **runnable here:** `readelf -hdsW bin/camweb.so` (and the other `bin/*.so`) | DYN MIPS o32 mips32r2 PIC, no NEEDED libs, `.init_array` ctor, UND syms = expected libc, target `lui` immediates present | all shims | ✅ |
| 1.8 | Shim address re-proof vs `vp_project.bin` | `tools/app_gotpage.py -d <addr>` **[not shipped]**; the vendor binary it disassembles is **not redistributable**, so this row cannot be reproduced from this repo. Addresses are documented in [FEATURES.md](FEATURES.md) | each target VA materialized by absolute `lui/ori`, no GOT/`$gp` coupling | reproduced | ✅ |
| 1.9 | Image diff: integrated `/system` vs stock | **runnable here:** `src/build_integrated.sh` self-verify (needs your own stock `/system` dump — see [STOCK_SYSTEM.md](STOCK_SYSTEM.md)) | only `bin/vp_project` (wrapper) + `lib/*.so` added; nothing changed/removed; mtd0/2/3/appfs byte-identical | diff = additions only | ✅ |
| 1.10 | Shipped artifact md5s match the docs | **runnable here:** `md5sum bin/* firmware/mtd4_integrated.bin` | every value matches the artifact register in [FEATURES.md](FEATURES.md); the image additionally matches the committed `firmware/mtd4_integrated.bin.md5` | exact | ✅ |

> **1.10 keeps no hashes of its own on purpose.** The authoritative md5 register lives in
> [FEATURES.md](FEATURES.md) and in `firmware/mtd4_integrated.bin.md5` — one place, so it
> cannot go stale here. (An earlier revision of this table hard-coded a build that predates
> the shipped image; those values were wrong and have been removed.)

**Measured result: 18 passed, 0 failed** across 1.1–1.5 on the author's tree. The suite itself
is **[not shipped]**; what it guarded now lives in `src/onvif_rtsp/`, so after changing that
daemon re-verify against a live camera (`ffprobe -rtsp_transport tcp rtsp://<cam-ip>:554/live`
plus a frame-count run, §6c).

---

## 2. Deploy / flash gates  (bench)

| # | Test | Method | Expected | Pass criterion | Result |
|---|---|---|---|---|---|
| 2.1 | SD present + image md5 pre-flight | serial `md5sum /mnt/sda0/mtd4_integrated.bin` | matches `firmware/mtd4_integrated.bin.md5` | exact; **abort flash on mismatch** | ✅ (author's `flash_integrated.ps1` **[not shipped]**; the shipped `tools/windows-flasher/flash-console.ps1 -Md5 <hash>` performs the same abort-on-mismatch gate) |
| 2.2 | Revert image md5 pre-flight | serial `md5sum /mnt/sda0/mtd4_camweb_v8.bin` | `fb1266aa06d4faa7d1efa46c844d304f` | exact | ✅ |
| 2.3 | Live `mtd4` backed up before flash | serial `cat /dev/mtd4 > /mnt/sda0/mtd4_backup_live.bin; md5sum` | non-empty, md5 recorded | file present | ✅ |
| 2.4 | flashcp succeeds | serial `flashcp -v … /dev/mtd4` | no `error/failed/busy` in output | clean | ✅ |
| 2.5 | flashcp readback matches | (v8 path) readback md5 == written | equal | exact | ✅ (v8) |
| 2.6 | Revert restores stock behaviour | flashcp `mtd4_system_STOCK.bin` → reboot | boots stock, WiFi intact (NVS untouched) | boots, `vnet0` up | ✅ |

---

## 3. Per-feature functional tests  (bench)

### 3.1 camweb — `:81` web server / RTSP unlock  ✅

| # | Test | Method | Expected | Pass |
|---|---|---|---|---|
| a | `:81` binds | serial `netstat -ltn` | `tcp 0.0.0.0:81 LISTEN` | ✅ |
| b | LD_PRELOAD present | serial `cat /proc/$(pidof vp_project)/environ \| tr '\0' '\n' \| grep LD_PRELOAD` | lists `camweb.so` first | ✅ |
| c | create_web NOP applied (RAM) | serial `dd if=/proc/$(pidof vp_project)/mem bs=1 skip=514932 count=4 of=/tmp/b; md5sum /tmp/b` | 4 × `00` (md5 of `00000000`) | ✅ |
| d | livestream serves H.264 (auth OK) | PC: `curl -s "http://<cam-ip>:81/livestream.cgi?…streamid=10&substream=2" \| head -c 4 \| md5sum` | first 4 bytes = `55 aa 15 a8` (frame magic, no HTTP header) | ✅ |
| e | wrong pw / streamid rejected | PC: same `curl` with a bad `loginpas` or `streamid=0` | HTTP body `result=-1;…` (no magic) | ✅ |
| f | RTSP end-to-end | **runnable here:** `ffprobe -rtsp_transport tcp rtsp://<cam-ip>:554/live` (originally measured through the author's PC proxy on `:8554` **[not shipped]**) | `h264 (High) 2304x1296 @ ~15fps` | ✅ (evidence frame `dvr_feed_frame.jpg` **[not shipped]**) |

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
| a | OSD shows a battery % | **runnable here:** `ffmpeg -rtsp_transport tcp -i rtsp://<cam-ip>:554/live -frames:v 1 osd.jpg` and read the overlay | timestamp line ends `… NN%` (letter-free) | NN present | ✅ (evidence frame `osd_final_frame.jpg` **[not shipped]**, read ~98%) |
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
| R3 | camweb still first / present | serial `…/environ \| grep LD_PRELOAD` | `camweb.so` leads the list | first token | High |
| R4 | `:81` binds after reboot | serial `netstat -ltn` | `0.0.0.0:81 LISTEN` | present | Med |
| R5 | **Reboot persistence** | `reboot`; re-run R1 + 3.1 | all four shims active, WiFi + `:81` up | all green after reboot | High |
| R6 | OTA/internet blocked | serial `route -n` | two `/1` reject routes present; LAN `/24` still reachable | reject routes present | Med |
| R7 | no unclean-exit reboot loop | watch serial ~2 min | camera stays powered past ~90 s (AIC keepalive intact via healthy vp_project) | no reboot loop | High |
| R8 | mtd integrity | serial `md5sum` of a captured `/dev/mtd0`,`mtd2`,`mtd3` vs stock | unchanged | byte-identical | Med |

---

## 5. RTSP proxy stability tests  ✅  — *superseded architecture, tooling [not shipped]*

> These characterize the author's **PC-side proxy** (`cam_rtsp_proxy.py` / `rtsp_server.py` /
> `vstarcam_frame.py`), the pre-daemon design in which a PC re-served the camera's `:81` stream
> as RTSP on `:8554`. **That proxy is not published here** and is **no longer the architecture** —
> the shipped camera serves RTSP itself on `:554` via `cam_onvifd` (§6), with no PC in the media
> path. The findings write-up (`qa_findings.md`) is likewise **[not shipped]**.
>
> The section is kept because the failure modes it documents (idle timeouts, the ~123 s device
> cutoff, slow-client starvation, GOP/SPS behaviour) are **camera-side** and carried over into
> the on-device daemon's design. Substitute `rtsp://<cam-ip>:554/live` for `rtsp://…:8554/live`
> to re-run any of these against the shipped daemon with plain `ffmpeg`/`ffprobe`.

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

The on-device daemon (`cam_onvifd`) is real and verified. Three test layers.

### 6a. PC acceptance harness  ✅  — *harness [not shipped]*

> The harness (`onvif_harness.py`, its mock `mock_onvif_server.py`, and the pass-criteria sheet
> `ACCEPTANCE.md`) lives in the author's private tree and is **not published here** — there is
> no `tools/onvif_test/` in this repo. The invocations below are recorded for provenance, **not
> as commands you can run**; reader-runnable substitutes follow the table.

`onvif_harness.py` proved the daemon is a real, discoverable ONVIF Profile-S camera any NVR
accepts. Four stages: WS-Discovery, ONVIF device/media SOAP, RTSP pull (frame-**count** over
N s — a single decodable frame is an explicit FAIL), and an NVR-compat checklist. It ran both
without a camera against a bundled mock, and against the live device with `--target`.

```sh
# [not shipped] — recorded for provenance only; these files are not in this repo
python3 onvif_harness.py --mock                                   # self-contained
python3 onvif_harness.py --mock --proxy-rtsp rtsp://127.0.0.1:8554/live
python3 onvif_harness.py --target <cam-ip> --user admin --pwd admin   # live device
```

**Reproduce the same four stages with stock tools** against a flashed camera:

```sh
# b) ONVIF device/media SOAP — any op, e.g. GetDeviceInformation (digest auth, admin/admin)
curl -s --digest -u admin:admin -X POST http://<cam-ip>:80/onvif/device_service \
  -H 'Content-Type: application/soap+xml' \
  --data '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body>
          <GetDeviceInformation xmlns="http://www.onvif.org/ver10/device/wsdl"/>
          </s:Body></s:Envelope>'

# c) RTSP pull, continuous — a frame COUNT, not a single frame (1 frame = FAIL)
ffprobe -rtsp_transport tcp rtsp://<cam-ip>:554/live         # expect h264 2304x1296 ~15fps
ffmpeg -rtsp_transport tcp -i rtsp://<cam-ip>:554/live -t 5 -f null -   # expect ≳ 5*15*0.5 frames

# e) NVR auto-add — add the camera in your NVR as a manual ONVIF device, :80, admin/admin
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

### 6b. Daemon offline suite  ✅  — *suite [not shipped]*

> The suite (`run_offline_tests.sh` and its six sub-suites) and the capture it replays
> (`stream_capture.raw`) live in the author's build tree and are **not published here** — there
> is no `builds/` directory in this repo. The daemon **sources** they exercise *are* shipped,
> under `src/onvif_rtsp/src/`, so the table maps each suite to the file it covers. All six
> passed; none can be re-run from this repo as-is.

| # | Suite (**[not shipped]**) | Proves | Shipped source covered |
|---|---|---|---|
| 1 | frame-parser equivalence (`test_frame.c`) | C port byte-identical to the Python `vstarcam_frame.py` reference **[not shipped]** on a 46-frame / 52-NAL capture **[not shipped]**, fed one byte at a time | `src/onvif_rtsp/src/vstarcam_frame.c` |
| 2 | RTSP protocol equivalence (`test_rtsp_e2e.py`) | drives the compiled daemon with the Python reference's own RTSP client; SPS(21B)+PPS(44B)+IDR(44856B) over TCP-interleaved *and* UDP, incl. reconnect-on-EOF | `rtsp_server.c`, `rtp_h264.c` |
| 3 | ONVIF sanity (`test_onvif.py`) | every discovery/Event/activation op succeeds with **zero creds**; `GetStreamUri` 401→authenticates via WSSE / a full HTTP Digest round trip / Basic; wrong passwords rejected; `/onvif/snapshot` JPEG-magic proxy; token-agnostic `GetStreamUri`; unimplemented op → 400 (not 500); WS-Discovery ProbeMatch | `onvif_soap.c`, `onvif_wsd.c` |
| 4 | resolution equivalence (`check_profile_resolution.sh`) | `GetProfiles`/`GetVideoEncoderConfiguration` report the **SPS-derived** 2304×1296, not the config default | `h264_sps.c`, `onvif_soap.c` |
| 5 | WS-Security vectors (`test_wsse.c`) | SHA-1 FIPS 180-1 KAT + WS-UsernameToken digest cross-checked in Python | `wsse.c` |
| 6 | HTTP Digest/Basic vectors (`test_httpauth.c`) | MD5 RFC-1321 KAT + the RFC-2617 §3.5 worked example + full challenge→validate round trips | `httpauth.c`, `md5.c` |

### 6c. Live device verification (cold boot)  ✅

After a real cold boot of the integrated image: daemon auto-starts (wrapper waits for `:81`,
stages to `/tmp`, runs it); `ffprobe rtsp://<cam-ip>:554/live` = h264 2304×1296 15 fps; ONVIF
SOAP `:80` harness 20/22; SPS-derived resolution 2304×1296; camera-EOF self-heal ~0.38 s;
snapshot proxy returns a real JPEG; Synology adds it natively. This is the one layer of §6 a
reader can reproduce in full — every check above is `ffprobe`/`ffmpeg`/`curl` against a flashed
camera (commands in §6a). Deploy procedure: [FLASHING.md](FLASHING.md); service details:
[ONVIF.md](ONVIF.md). (The author's `DEPLOY.md` under `builds/` is **[not shipped]**; its
content is folded into those two docs.)

---

## 7. Gaps the matrix cannot yet cover  (pending in-progress work)

- **Audio — mic is DONE and shipped; only talk-back is still open.** *(This entry previously
  claimed the stream was "video-only"; that is no longer true and the claim has been removed.)*
  - **Microphone → NVR: shipped and verified ✅.** `mic_capture.so` ships in `bin/` and is
    embedded in `firmware/mtd4_integrated.bin`; it pure-reads `IMP_AI` dev 1 / ch 0 (16 kHz,
    16-bit mono, 25 fps) and feeds `cam_onvifd`, which 2:1 downsamples to 8 kHz, μ-law-encodes
    and serves a **second RTP track**. Verified live: the RTSP `DESCRIBE` SDP advertises
    `m=audio 0 RTP/AVP 0` with `a=rtpmap:0 PCMU/8000` alongside the H.264 track, and the daemon
    holds a steady 25.0 audio frames/s with correct pitch. **Runnable check:**
    `ffprobe -rtsp_transport tcp rtsp://<cam-ip>:554/live` — expect **two** streams, `h264` and
    `pcm_mulaw, 8000 Hz, mono`. Full account: [AUDIO.md](AUDIO.md).
  - **Mic contention — answered, not pending.** A second reader does **not** get `EBUSY`: the
    shim piggybacks the vendor's already-initialised AI pipeline as a pure reader, so it
    coexists with `vp_project`. Two RAM-only byte patches make the shim the sole `dev1`
    consumer, restoring the full 25 fps (see [AUDIO.md](AUDIO.md#full-rate-patch--making-the-shim-the-sole-dev1-consumer)).
  - **The `:81` audio CGI remains a confirmed dead end ❌** — the handler builds the
    `0xA815AA55` container but never calls `IMP_AI`, so it emits no samples (disasm plus a
    capture probe that found zero audio frames; the probe script itself is **[not shipped]**).
    This is why capture is done natively rather than pulled from the CGI. Not a gap — a
    closed question.
  - **Still a genuine gap: talk-back (NVR → camera speaker) ⚠️.** The software path is built
    and loopback-proven all the way to the codec DAC, but the speaker output stage never
    enables, and the AO calls corrupt the shared codec so the mic goes robotic until a full
    power-off. It is therefore kept on the `talkback-experimental` branch and **out of the
    default image**, so **no talk-back tests are in this matrix**. Adding them depends on
    solving the codec speaker-enable — see
    [AUDIO.md](AUDIO.md#2-speaker--talk-back--not-audible--the-open-problem-️).
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
