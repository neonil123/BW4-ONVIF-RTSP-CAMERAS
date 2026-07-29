# Audio — mic works & ships; talk-back speaker is the open problem

Two independent halves:

| Direction | What | Status |
|---|---|---|
| **Camera → NVR** (microphone) | Live G.711 μ-law audio in the RTSP stream, second RTP track | **WORKING, shipped** ✅ |
| **NVR → camera** (speaker / talk-back) | Two-way audio out the camera's speaker | **NOT audible — open** ⚠️ |

The rest of this page is an honest record of how each half was built and exactly where the
talk-back wall is.

---

## 1. Microphone → RTSP — done ✅

> ### ✅ "Robotic/chunky" mic — root-caused with an acoustic tone-loop and FIXED
> An earlier note here blamed a "3.4 kHz brick-wall / native 8 kHz channel." **That was wrong.** A
> controlled acoustic loop (play calibrated tones + sweeps at the mic, capture the RTSP audio, measure)
> found **two** real, camera-side defects — both now fixed:
> 1. **Pitch was exactly one octave low** (f_out = f_in/2 on every tone + the sweep, duration
>    preserved). The mic content is genuinely **16 kHz** (`insmod audio.ko … samplerate=16000`), but the
>    daemon served the samples as 8 kHz → octave-down "Transformers" voice. **Fix:** the daemon now
>    **2:1 averaging-downsamples 16k→8k** before μ-law (`audio_source.c`), restoring correct pitch.
> 2. **"Chunky"/gappy = a frame SPLIT, not a bandwidth limit.** Instrumenting the shim to log
>    `IMPAudioFrame.seq` showed we received **every other frame** (seq delta 2). `dev1/ch0` emits the
>    full **25 fps** of 16 kHz, but `vp_project`'s own audio thread *and* our shim both call the
>    *consuming* `IMP_AI_GetFrame`, so each got half (12.5 fps ⇒ ~50% of the timeline missing). **Fix:**
>    two byte-patches to the RAM copy of `vp_project` neutralize the vendor's competing consumer so the
>    shim is the **sole consumer** → all 25 fps → contiguous 16 kHz (see *Full-rate patch* below).
>
> **Result (verified live):** shim seq delta **1**, daemon audio **25.0/s** steady, RTSP delivers
> real-time continuous audio (8.08 s captured in an 8 s window, was 4.3 s), **correct pitch**, stable.
> Residual: `dev1` still runs the vendor's NS/AGC/AEC DSP and gates to silence in a quiet room — that's
> cosmetic, not the chunking. For studio fidelity one could later disable `IMP_AI` NS/AEC or carry the
> raw 16 kHz as L16/Opus instead of narrow-band G.711.

### The dead end we started at (`:81` audio CGI)
The first idea was to pull audio the same way video is pulled — from the app's local
`livestream.cgi`/`audiostream.cgi` on `127.0.0.1:81`. **That is architecturally impossible
here:** the app's audio-CGI handler *builds* the `0xA815AA55` frame container but **never calls
`IMP_AI`**, so it emits zero audio samples. Captures with the audio parameters set contain only
Annex-B H.264 — no audio frames at all. It's a compiled-in but hollow endpoint. Don't spend
time on the CGI route.

### What actually works — native `IMP_AI` capture via an LD_PRELOAD shim
Audio is captured by **our own** code on the device, not pulled from the app:

- **`mic_capture.so`** (in the `LD_PRELOAD` chain) does a **pure read** of the Ingenic audio
  input using the vendor's already-initialised pipeline — **AI device 1, channel 0** — via the
  vendor `PollingFrame`/`GetFrame`/`ReleaseFrame` wrappers. It never re-inits or reconfigures
  the device, so it does **not** contend with `vp_project`. It ships S16LE PCM as UDP datagrams
  to `127.0.0.1:5599`.
- **Native mic format is 16 kHz / 16-bit / mono** (`samplerate=16000`; 640 samples = 40 ms per
  1280-byte frame, 25 fps). An earlier note guessed 8 kHz — wrong; that's what produced the
  octave-down pitch until the daemon downsample was added.
- **`okam_onvifd`** receives `:5599`, **2:1 averaging-downsamples 16k→8k** (anti-alias low-pass +
  decimate, carrying one sample across datagram boundaries), μ-law-encodes, and serves it as a
  **second RTP track** — `PCMU/8000`, payload type 0 — advertised in the RTSP SDP as
  `m=audio ... a=rtpmap:0 PCMU/8000 / a=control:track1`. Timestamp advances at 8 kHz. The live
  stream carries synchronized A/V at ~50 packets/s.

### Full-rate patch — making the shim the sole `dev1` consumer
`IMP_AI_GetFrame` *dequeues*; with two callers (the vendor audio thread + our shim) each gets every
other frame. The wrapper patches the **RAM copy** of `/usr/bin/vp_project` (on-disk binary untouched,
same mechanism as the `:81` onboarding NOP) at two sites found by disassembly (statically-linked
libimp; file offset = vaddr − 0x400000):
- **`0x4c3908` (file 801032)** — the vendor fetch's `jal IMP_AI_GetFrame` → `li v0,-2`. The fetch
  still **Polls** `dev1` (keeps pacing) but **skips GetFrame and returns −2**, a value its own audio
  thread (loop @`0x454b08`) treats as "transient, keep looping." Any other negative return would make
  that thread exit and tear down the AI channel we depend on — so **−2 specifically** keeps it alive
  and idle. Net: our shim becomes the sole consumer → all 25 fps.
- **`0x454c50` (file 347216)** — NOP the now-per-iteration error-log call so it can't spam tmpfs 24/7.

Cost: the vendor audio thread now frame-paces (~25/s) doing nothing; CPU stays within headroom and the
mic delivers a steady 25.0/s with no drops.

### The contention question — answered
The whole project depends on `vp_project` staying alive (it holds the power-gate keepalive). The
open worry was whether a second reader of the mic returns `EBUSY`. **It doesn't** — because
`mic_capture.so` piggybacks the vendor's running AI pipeline as a pure reader on **dev 1**
(rather than opening/initialising the device itself), capture and the app coexist. This is the
key insight that made mic audio shippable.

### MIPS gotcha that bit us
On this uClibc/MIPS target `SOCK_DGRAM == 1` (not 2 as on x86). `socket(AF_INET, 2, 0)` silently
made a **TCP** socket (hung `connect`, `EPIPE`); `socket(AF_INET, 1, 0)` is the correct UDP
socket. Same story for `SOL_SOCKET=0xffff`, `SO_RCVTIMEO=0x1006`. Anything doing raw syscalls on
this platform must use the MIPS numbers.

---

## 2. Speaker / talk-back → NOT audible — the open problem ⚠️

The **entire software pipeline is built and provably correct up to the codec**, but **no sound
comes out of the speaker.** This is the one unsolved item in the project.

> ⚠️ **Talk-back is on the [`talkback-experimental`](../../../tree/talkback-experimental) branch
> and is deliberately kept OUT of the default image.** Beyond not producing speaker sound, the
> `speaker_feed.so` AO calls (`IMP_AO_SetPubAttr`/`Enable`/`EnableChn` on the **shared**
> `jz-inner-codec`) were observed to **corrupt the microphone** — the 8 kHz mic turns into a
> robotic/metallic ("vocoder") signal. The sample rate stays correct 8 kHz and the daemon is
> unchanged, so the damage is in the codec's analog/mixer state, not our software. Critically it
> **survives a warm reboot and a soft power cycle** — on a battery camera, unplugging USB-C does
> not drop the codec rail. **Recovery: a true full power-off (remove USB-C *and* the battery ~30 s)**
> and run the mic-only image (no `speaker_feed`). Until the speaker enable is solved, do not put
> `speaker_feed.so` in a chain you care about the mic on.

### The pipeline (all working)
```
NVR talk audio (G.711)
  → okam_onvifd  speaker_sink.c   : UDP :5601, μ-law-decode, 8k→16k 2× linear upsample
  → UDP 127.0.0.1:5600
  → speaker_feed.so               : vendor AO wrappers → IMP_AO_SendFrame → codec DAC
```
`speaker_feed.so` drives the Ingenic audio **output**: AO **device 0, channel 0, 16 kHz / 16-bit
mono, 640-byte (320-sample) frames**. It calls the vendor's own AO helpers
(`ao_init`→`Enable`→`EnableChn`, `Soft_UNMute`, `SetVol`/`SetGain` at the vendor's talk-mode
levels) and then `ao_send` (which accumulates into the 640-byte global buffer and calls
`IMP_AO_SendFrame`). Every IMP call returns success (0).

### Proof the audio reaches the DAC
Playing audio into `:5601` makes that audio **appear in the microphone stream** — an electrical
DAC-out → ADC-in loopback inside the codec. So our samples travel all the way through
`IMP_AO_SendFrame` → the AO channel → the mixer → the DAC. The digital path is correct.

### Where it dies
The codec's **speaker output stage (the on-chip class-D amp / SPK route) never turns on.**
The amp is hardwired-on in the board sense (`spk_gpio = -1`, no amp-enable GPIO, no codec-route
ioctl anywhere in `vp_project`), and the codec is the **built-in kernel `jz-inner-codec`** — it
is compiled into the kernel, not a loadable `.ko` we can inspect or swap. So the missing
"enable speaker output" step lives in kernel code we can't see from userspace on stock firmware.

### What we ruled out
- Not a volume/mute issue — `SetVol`/`SetGain`/`Soft_UNMute` all applied; loopback shows the
  mixer gain is non-zero.
- Not a missing amp GPIO — there is none on this board (`spk_gpio=-1`).
- Not the software mute-fade threads (`_ao_play_mute/unmute_thread` are anti-pop fades, not amp
  control). `_ao_play_thread` is the only thing that drains AO frames to `/dev/dsp` (via ioctl
  `0x40085063`, not `write()`), and it is running.

### The most promising untried lead
The open-source **[gtxaspec/ingenic-audiodaemon](https://github.com/gtxaspec/ingenic-audiodaemon)**
plays audio out on sibling Ingenic SoCs (T20/T31/T40) using **only** the standard sequence —
`IMP_AO_SetPubAttr → GetPubAttr → Enable → EnableChn → SetVol → SetGain → SendFrame` — with **no
special amp/codec/GPIO enable**. The one thing it does that our shim does **not**: a genuinely
**fresh `SetPubAttr` init as the sole owner**. Our shim *piggybacks* `vp_project`'s
already-enabled AO, so our `Enable`/`EnableChn` are idempotent no-ops and **`SetPubAttr` never
re-runs** — which is plausibly what would re-program the codec's speaker route. The next
experiment is to force a full fresh AO init (incl. `SetPubAttr`) in `speaker_feed.so` instead of
inheriting the vendor's state. **Untried at time of writing.**

If that fails, the remaining path is kernel-side: RE the `jz-inner-codec` speaker-enable in the
kernel image (or run a thingino kernel where audio-out is known-good — but see the WiFi blocker
in [FLASHING.md](FLASHING.md#the-thingino-full-firmware-path)).

---

## Summary

| Item | Status |
|---|---|
| `:81` audio CGI as an audio source | **dead end** — handler builds container, never calls `IMP_AI` |
| Native `IMP_AI` mic capture (dev 1, 8 kHz, pure-read shim) | **done** ✅ |
| Mic contention with running `vp_project` | **no `EBUSY`** — pure-read coexists ✅ |
| Mic audio in RTSP/ONVIF (G.711 μ-law, `PCMU/8000`, track1) | **shipped** ✅ |
| Mic quality — correct pitch (2:1 downsample) + continuous full-rate (sole-consumer patch) | **fixed** ✅ |
| Talk-back software pipeline (`:5601`→sink→`:5600`→AO→DAC) | **built & correct** ✅ |
| Talk-back audible at the speaker | **NO** — codec SPK output stage not enabled ⚠️ **open** |
| Next lead | fresh `IMP_AO_SetPubAttr` init (don't piggyback vp_project) |
