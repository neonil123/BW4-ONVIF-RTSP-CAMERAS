# Camera Discovery

How an NVR/DVR (or you) finds the camera and gets a playable stream. The camera is now a
**native ONVIF device** (`cam_onvifd`, see [ONVIF.md](ONVIF.md)) — but on a real home LAN
you'll usually still **add it manually**, because WS-Discovery multicast rarely crosses the
WiFi/wired boundary. This page explains both.

---

## Current state — the camera is an ONVIF device  [verified]

The on-device daemon serves ONVIF on `:80` and RTSP on `:554`, so the DVR talks to the
**camera itself** — there is no PC in the loop anymore. Two ways to add it:

1. **Native ONVIF add** (verified with Synology Surveillance Station): Add Camera → brand
   `ONVIF`, IP `<cam-ip>`, port **80**, credentials `onvif_user`/`onvif_pass` (default
   `admin`/`admin`).
2. **User-Defined / custom RTSP**: `rtsp://<cam-ip>:554/live`.

Full setup walkthrough (both methods, Synology + generic NVRs) is in [ONVIF.md](ONVIF.md) §4.
`<cam-ip>` is whatever address your DHCP server hands the camera; find it in the router's
DHCP lease table (look for the camera's MAC / hostname) or via serial `ifconfig vnet0`. A
DHCP reservation is worth setting so the NVR entry doesn't break on lease renewal.

The device's ONVIF credential (default `admin`/`admin`) is **separate** from the camera's
internal device password (`<your-unit-devpw>` on the pilot, per-unit random) — the daemon
injects the latter into the `:81` handshake itself; you never enter it into the NVR.

---

## WS-Discovery — works device-side, but auto-search usually fails on a home LAN

`cam_onvifd` implements WS-Discovery correctly (UDP 3702, multicast `239.255.255.250`,
`ProbeMatch` with `Types = NetworkVideoTransmitter` and `XAddrs = http://<cam-ip>:80/onvif/device_service`,
unicast back to the Probe's source). Serial logs prove it receives every Probe and answers.
`[verified device-side]`

**But auto-discovery ("ONVIF search" in the NVR) usually does not find it**, for two reasons:

- **Multicast across the WiFi/wired boundary.** The camera is on WiFi; the NVR is often
  wired. Consumer routers frequently don't forward link-local multicast between those
  segments, so the Probe never reaches the camera (or the reply never returns). This is a
  network-topology fact, not a daemon bug.
- **The `dasHost` test-host caveat.** On the Windows dev/test host, UDP 3702 is owned by the
  Function Discovery service (`dasHost`), so a Probe sent from that same machine can't be
  received back by the test harness. The two "discovery" warnings in the acceptance harness
  (see [TESTING.md](TESTING.md) §6) are this artifact — the raw-socket prober still validates
  the device's unicast reply, and a real NVR or Linux host **on the same L2 segment** does
  discover it.

**Practical guidance: add the camera manually** (IP + port 80, or the RTSP URL). Treat
WS-Discovery auto-search as a bonus that works when the NVR shares the camera's segment, not
as the primary path.

---

## Why the stock camera couldn't be discovered at all

`vp_project` contains **no** RTSP or ONVIF protocol strings — probed directly with a string
scan over a dump of the stock `/usr/bin/vp_project` (checks `RTSP/1.0`, `DESCRIBE`,
`rtsp://`, `a=rtpmap`, `GetProfiles`, `onvif.org`, `NetworkVideoTransmitter`, `ws-discovery`,
`wsdd`, …). The stock app speaks only the private VStarcam `0xA815AA55` frame protocol over
its `:81` CGI and, for the cloud, a P2P protocol on `:19548`/`:20xxx`. ONVIF is therefore a
**genuinely new on-device component** (`cam_onvifd`) added by this project, not a dormant
feature that was unlocked.

---

## Architecture nuance — the endpoint moved on-device

The DVR always talks to whatever **hosts the RTSP/ONVIF endpoint**:

- **Before:** endpoint = the **PC proxy** (`rtsp://<pc>:8554/live`); the camera spoke only its
  private `:81` protocol; discovery was you, manually, typing a URL.
- **Now:** endpoint = the **camera** (`cam_onvifd`, `:554` + `:80`); the camera is an ONVIF
  device the NVR adds and pulls directly. `[verified]`

Same H.264 either way. The on-device move is what makes "add it as an ONVIF camera" possible;
manual add is still the norm only because of the multicast-boundary reality above, not because
the camera lacks discovery.
