# ONVIF / RTSP on the device — `okam_onvifd`

The camera now serves **standards RTSP and full ONVIF Profile-S from a daemon running on
the camera itself** — no PC in the loop. This is the primary media path; the PC proxy
(`tools/okam_rtsp_proxy.py`, see [ARCHITECTURE.md](ARCHITECTURE.md) §4) is now a
dev/fallback tool.

- **RTSP:** `rtsp://<cam-ip>:554/live` — H.264, 2304×1296, ~15 fps `[verified]`
- **ONVIF:** SOAP on `:80` (`/onvif/device_service`), WS-Discovery on UDP `3702` `[verified]`
- **Snapshot:** `GET http://<cam-ip>:80/onvif/snapshot` → JPEG `[verified]`
- **Verified live** against a real **Synology Surveillance Station** NVR (native ONVIF add)
  and after a **cold boot** (auto-starts from the flashed image). `[verified]`

`okam_onvifd` is a static MIPS32r2 LE C daemon (`builds/features/onvif_rtsp/`, source in
`src/`). It is a byte-for-byte C port of the already-proven Python stack
(`tools/vstarcam_frame.py` + `tools/rtsp_server.py` + `tools/okam_rtsp_proxy.py`). It
connects to the camera's own local H.264 source (`vp_project`'s `livestream.cgi` at
`127.0.0.1:81`, exposed by the `okabweb.so` shim — see [FEATURES.md](FEATURES.md)) and
re-serves it. `vp_project` contains **no** RTSP/ONVIF code of its own (probed with
`tools/app_onvif.py`), so this is a genuinely new on-device component, not an unlock.

| artifact | value |
|---|---|
| daemon binary | `builds/features/onvif_rtsp/okam_onvifd` |
| md5 (stripped) | **`8435ab9bcb6c1c235befcfd498b7cef9`** |
| size | 210,888 bytes (~206 KiB), static MIPS32r2 LE ELF (`Type: EXEC`, no `PT_INTERP`) |
| unstripped | `okam_onvifd.unstripped`, md5 `6d2ffd7bff76c836ab6d28d41abc5a55` |
| config | `/system/etc/okam_onvifd.conf` (template: `okam_onvifd.conf.example`) |
| runbook | `builds/features/onvif_rtsp/DEPLOY.md` |

> **Why static, not dynamic against the device uClibc 0.9.33.** No uClibc cross-toolchain
> exists in the dev environment, and musl's CRT (`__libc_start_main(sp)`) is
> calling-convention-incompatible with uClibc's multi-arg `__libc_start_main` — a
> musl-object/uClibc-runtime binary can't even reach `main()` without a hand-written o32
> CRT stub, unverifiable without the device or MIPS emulation. A well-tested 206 KiB static
> binary (same size class as `tools/aic_keepalive`) was the safer call. Swapping `build.sh`'s
> `CC` to a real uClibc toolchain and dropping `-static` recovers the dynamic path with no
> source change. Details in `builds/features/onvif_rtsp/README.md`. `[verified]`

---

## 1. The RTSP service (`:554`) `[verified]`

- Endpoint `rtsp://<cam-ip>:554/live`; SDP `H264/90000`, `sprop-parameter-sets` from the
  live SPS/PPS; RFC-6184 FU-A fragmentation for the large IDRs.
- Ops: `OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN / GET_PARAMETER` (keepalive advertised).
- Both `SETUP` transports work: `RTP/AVP/TCP` (interleaved, the default) and `RTP/AVP` UDP
  unicast. Prefer TCP — no RTCP is emitted on UDP (see [ARCHITECTURE.md](ARCHITECTURE.md) §4).
- One dedicated reader thread pulls `livestream.cgi` and fans access units to **per-client
  bounded queues** (drop-oldest), a C port of the proxy's hub architecture. Reconnects on
  the device's ~123 s `livestream.cgi` EOF cadence with a fast first-retry backoff (visible
  gap ~0.38 s live, ≤ one 4 s GOP).
- Advertised resolution is **parsed from the real SPS** (`h264_sps.c` → 2304×1296 live),
  not a hardcoded default — `video_width`/`video_height` in the conf are only a fallback
  until the first SPS is seen.

## 2. ONVIF SOAP (`:80`) `[verified]`

One HTTP endpoint (`/onvif/device_service`) handles Device + Media + Event SOAP. Actions
are matched by a substring scan of the request body (no XML parser), the same technique the
reference CGI implementations use; response shapes follow the bundled
`builds/prudynt_bundle/onvif` reference for client compatibility.

### 2a. Access policy — this is the part that made Synology work `[verified]`

The daemon matches how real Profile-S cameras behave (confirmed against Synology
Surveillance Station's add-camera wizard, which was the acceptance target):

- **All discovery/capability/activation ops are PRE-AUTH open** — they succeed with **no
  credentials at all**. An NVR must be able to enumerate and classify the camera before it
  has entered any credentials; Synology `401`'d and got stuck "activating" when these were
  gated. This is `action_requires_auth()` in `onvif_soap.c`.
- **Only `GetStreamUri` requires authentication** (the one actual media-access op). An
  invalid/missing credential there returns HTTP **401** with `WWW-Authenticate: Digest …` /
  `Basic …` challenges and an `env:Sender`/`ter:NotAuthorized` SOAP fault body — which is
  exactly what makes Synology retry with credentials on that one step.
- Anything recognized but genuinely unimplemented returns HTTP **400** +
  `env:Receiver`/`ter:ActionNotSupported` (never a bare 500), so a client can tolerate a
  missing non-critical op instead of aborting.

> **Contradiction flagged:** the comment in `okam_onvifd.conf.example` (and the header of
> `config.c`) says *"Every ONVIF operation except GetSystemDateAndTime/GetHostname/GetWsdlUrl
> requires authentication."* That is **stale/inverted** — the shipped code does the
> opposite (only `GetStreamUri` is gated; everything else is pre-auth open). Trust the code
> and this doc, not that comment. The default credentials it documents (`admin`/`admin`) are
> still correct.

### 2b. Auth methods `[verified]`

Any **one** of these authenticates `GetStreamUri`; credentials are accepted additively (a
token on a pre-auth op is fine too):

- **WS-Security `wsse:UsernameToken`** — PasswordDigest per WS-UsernameToken Profile 1.0
  (`Base64(SHA1(Nonce || Created || Password))`), with a PasswordText fallback. Self-contained
  SHA-1 + base64 (`wsse.c`), no new libc dependency. No `Created` freshness window is enforced
  (the camera RTC may be skewed; the digest match is what authenticates).
- **HTTP Digest (RFC 2617)** — `HA1=MD5(user:realm:pass)`, `HA2=MD5(method:uri)`,
  `response=MD5(HA1:nonce:nc:cnonce:qop:HA2)` (`httpauth.c` + a from-scratch RFC-1321
  `md5.c`). **This is what Synology Surveillance Station actually uses** (it authenticates
  ONVIF at the HTTP layer and never sends a WS-Security token).
- **HTTP Basic** — base64 `user:pass`.

Credentials are `onvif_user` / `onvif_pass` in the conf, **default `admin`/`admin`**. These
are a **completely separate credential** from the camera's internal `devpw`/`vuid` (which
the daemon injects itself into the `:81` handshake and never exposes over ONVIF).

### 2c. Operations implemented

**Pre-auth (discovery / capability):** `GetDeviceInformation`, `GetCapabilities`,
`GetServices`, `GetServiceCapabilities`, `GetScopes`, `GetProfiles` (+ singular `GetProfile`),
`GetVideoSources`, `GetVideo{Source,Encoder}Configuration(s)` (+ `[Options]`), `GetAudio*`
(empty/stub — no separate audio stream, an empty list is valid), `GetSystemDateAndTime`,
`GetHostname`, `GetWsdlUrl`. `[verified]`

**Event service (minimal)** — Synology sets up a pull-point subscription during activation
even with no motion detection wired up, and previously got a bare 500 for all of it:
`GetEventProperties` (advertises a motion topic), `CreatePullPointSubscription` (returns a
SubscriptionReference + CurrentTime/TerminationTime), `PullMessages` (always returns empty —
no real events ever fire, a normal "nothing happened" answer), `Renew`, `Unsubscribe`,
`SetSynchronizationPoint`. The Events service is advertised at its own distinct XAddr so its
`GetServiceCapabilities` (same SOAP action name as Media's) is disambiguated by request path.
`[verified]`

**Activation (accept-and-no-op)** — the daemon relays one fixed `vp_project` stream and
genuinely can't honor profile mutation, so these succeed without changing anything:
`GetNetworkInterfaces` (reports the **real** WLAN interface name / `HwAddress` / IPv4, queried
live via `getifaddrs()` + `ioctl(SIOCGIFHWADDR)`), `CreateProfile` (echoes the requested
Name/Token wrapped around the one real H264 config), `Set/Add/Remove{Video,Audio}*Configuration`,
`SetProfile`, `DeleteProfile`, `GetGuaranteedNumberOfVideoEncoderInstances` (`TotalNumber=1`),
`GetNTP` (`FromDHCP=false`), `GetRelayOutputs` (empty — this device has none). `[verified]`

**User management (accept-and-no-op)** — some NVRs run an "activation" step that enumerates or
sets the admin user and fault out if it 500s. `GetUsers` advertises the single admin
(`UserLevel=Administrator`); `CreateUsers`/`DeleteUsers`/`SetUser` return an empty 200 without
changing the fixed credential; `GetDNS`/`GetDiscoveryMode` answer with sane stubs. This is what
cleared the "camera reachable but says *activation* then fails" symptom. `[verified]`

**Media access (auth-gated):** `GetStreamUri` → `rtsp://<cam-ip>:554/live`. Deliberately
**token-agnostic** — it never reads `trt:ProfileToken`, so a newly-`CreateProfile`d token (or
any token) still returns the one real URL. `[verified]`

### 2d. Snapshot — `GetSnapshotUri` + `GET /onvif/snapshot` `[verified]`

`GetSnapshotUri` (also token-agnostic) points at a real **`GET /onvif/snapshot`** handler —
plain HTTP, not SOAP, short-circuited in `handle_conn()` before any body parsing. It proxies
`vp_project`'s own local snapshot CGI (`GET http://127.0.0.1:81/snapshot.cgi?…`, falling back
to `/onvifsnapshot.cgi`) using the same `devpw`/`vuid` it uses for video — so **Synology never
sees those credentials**. The device mislabels its snapshot response `Content-Type` as
`text/html` even though the body is a real JPEG, so `fetch_snapshot_path()` validates the
**JPEG magic `FF D8 FF`** instead of trusting the header, and the daemon always answers with
a corrected `Content-Type: image/jpeg`.

Quick check (no ONVIF client needed):
```
curl -v http://<cam-ip>:80/onvif/snapshot -o snap.jpg     # -> "JPEG image data"
```

## 3. WS-Discovery (UDP 3702) `[verified device-side]`

`onvif_wsd.c` joins `239.255.255.250` on an explicit interface (`vnet0` preferred, else the
first UP non-loopback IPv4). It tries an ifindex-based `ip_mreqn` join before falling back to
an address-based `ip_mreq` — a plain `INADDR_ANY` join fails with `ENODEV` on this device's
AIC8800 WiFi driver (found live on cam #1). The reply is unicast directly to the Probe's
source address+port, carrying `Types = NetworkVideoTransmitter` and
`XAddrs = http://<cam-ip>:80/onvif/device_service`.

> **The `dasHost` caveat.** On the Windows dev/test host, UDP 3702 is owned by the Function
> Discovery service (`dasHost`), so a multicast Probe from that same box does not loop back
> to the harness's second socket. This is a **test-host limitation, not a device bug** — the
> serial log proves the daemon receives every Probe and unicasts a correct ProbeMatch. A
> real NVR or Linux host on the LAN discovers it. See §5 for what this means for setup.

---

## 4. Setup walkthrough — Synology Surveillance Station (and generic NVRs)

Two independent ways to add the camera. Both stream the same H.264. Use whichever your NVR
prefers; **manual add is the norm** — auto-discovery multicast frequently does not cross the
WiFi/wired boundary of a typical home LAN (see §5).

### 4a. Native ONVIF (recommended) `[verified with Synology]`

Synology Surveillance Station → **Add Camera** → choose brand/**Marca = `ONVIF`**:

| field | value |
|---|---|
| IP address | `<cam-ip>` (pilot: `192.168.100.106`) |
| Port | **`80`** |
| Username / Password | the conf's `onvif_user` / `onvif_pass` (**default `admin`/`admin`**) |

It authenticates on the first try and auto-populates the H264 2304×1296 stream. (Getting
here took several iterations — every "stuck activating" was one more op Synology needed:
pre-auth discovery, the Event pull-point subscription, the activation ops, and the snapshot
proxy. See §2 for why.)

### 4b. User-Defined RTSP `[verified]`

If you'd rather skip ONVIF, add a **User-Defined / Custom RTSP** camera:

- RTSP URL path: **`/live`**, port **`554`** → `rtsp://<cam-ip>:554/live`
- Codec H.264, no NVR-side credentials needed for the RTSP URL itself.

### 4c. Other NVRs `[verified design; Synology is the proven one]`

Frigate, Blue Iris, Hikvision/Dahua recorders, ONVIF Device Manager: add as an ONVIF camera
with IP + port 80 + `onvif_user`/`onvif_pass`, or point them at the RTSP URL directly. Any
Profile-S NVR can add it — no PC middleware.

---

## 5. When auto-discovery fails (it usually does) `[verified]`

WS-Discovery is multicast (`239.255.255.250:3702`). On a real home LAN the camera is often on
WiFi while the NVR is wired, and consumer routers frequently don't forward link-local
multicast across that boundary — so an NVR's "ONVIF search" may not find the camera even
though the daemon answers every Probe correctly. **Manual add (§4) is the expected path.**
The device-side WS-Discovery is correct and will be found by NVRs/hosts on the same L2
segment; the two harness warnings about discovery (see [TESTING.md](TESTING.md)) are the
`dasHost` test-host artifact, not device faults.

---

## 6. Deploy, credentials, and debugging

The daemon is **baked into the integrated `/system` image and auto-starts on boot** (the
wrapper waits for `:81`, copies the binary into `/tmp`, `chmod`s it, and runs it with
`/system/etc/okam_onvifd.conf`). See [FLASHING.md](FLASHING.md) §4 for the persistent-install
and TFTP-flash mechanics, and `builds/features/onvif_rtsp/DEPLOY.md` for the run-it-now serial
path.

**Config precedence:** `--devpw`/`--vuid`/`--onvif-user`/`--onvif-pass` CLI > `OKAM_DEVPW`/
`OKAM_VUID` env > `/system/etc/okam_onvifd.conf`. Only `devpw`/`vuid` truly need setting.
They are **per-unit random** (the pilot's are `devpw=<your-unit-devpw>`, `vuid=<your-unit-vuid>`,
used only as a worked example — your unit differs; re-read `devpw` from
`/proc/$(pidof vp_project)/mem @0x81E988` if the camera is re-onboarded). `build_integrated.sh`
takes `DEVPW=`/`VUID=` env at build time and ships **`CHANGE_ME`** placeholders in the public
repo — never commit a real device password.

**Debugging against a real NVR:**
- Bump `log_level=3`. Every request logs
  `onvif: op=<action> auth=<none|digest|basic|wsse>(user) -> <200|401|400>`. E.g.
  `op=GetStreamUri auth=digest(admin) -> 401` means the NVR *is* sending Digest but the
  password doesn't match the conf; `auth=none() -> 401` means it sent nothing; a
  `op=<actual action> … -> 400` prints the real unimplemented action name (e.g.
  `tptz:GetConfigurations`) so a missing op is easy to spot.
- The first ~15 requests of each run are raw-captured (request line + headers + up to 400 B
  of body) to `/tmp/onvif_raw.log` (self-truncates on restart, never grows unbounded).

## 7. Acceptance

See [TESTING.md](TESTING.md) §6 and `tools/onvif_test/ACCEPTANCE.md`. The daemon is declared
ONVIF-working when the PC harness reports 0 failures against the device:
```
python tools/onvif_test/onvif_harness.py --target <cam-ip> --user admin --pwd admin
```
Live cold-boot run scored **20/22** — the only two fails are the environmental `dasHost`
UDP-3702 discovery artifact on the Windows test box (§3), not daemon bugs.
