# NOTICE — scope, secrets, and what is (not) redistributed

## Intended use
This project documents and tooling covers modifications to **cameras the operator owns**, as an
authorized right-to-repair / interoperability exercise: making the device speak the open ONVIF and
RTSP standards on the local network instead of depending on a vendor cloud. It is provided for
that purpose. Do not use it against hardware you do not own or are not authorized to modify.

## No secrets are published here
- The camera's **admin device password** and **device id (vuid)** are **per-unit random values**
  stored in the device's `mtd5` NVS. They are **not** defaults and are **not** contained anywhere
  in this repository.
- Every committed configuration and firmware artifact ships **`CHANGE_ME`** placeholders. You read
  your own unit's values off your own device and supply them at build time (see
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) → *Device password*).
- **Never commit** an image or config built with a real device password, a full-chip flash dump
  (it contains your NVS/Wi-Fi credentials), or any capture that includes credentials.

## No vendor firmware or media is redistributed
- The prebuilt overlay in `firmware/` is assembled **from scratch** and contains **only**:
  our own compiled binaries (`okam_onvifd`, the `*.so` shims), our own shell wrapper, our own
  config with placeholder creds, and a few **neutral text stubs** (`language.txt`, an empty
  `upgrade.txt`, a community `version.ini`).
- It deliberately excludes the vendor's `/system` content — the voice-prompt audio (`*.opus`,
  `*.g711a`) and logo — which are the vendor's copyrighted assets. As a result the camera loses
  its spoken prompts; everything else is unaffected. If you want them back, rebuild against **your
  own** camera's stock `/system` dump with `src/build_integrated.sh` (see
  [docs/STOCK_SYSTEM.md](docs/STOCK_SYSTEM.md)).
- No Ingenic IMP SDK code, no vendor application (`vp_project`), and no kernel/bootloader images
  are included. Our code interoperates with those via documented runtime addresses and ABIs only.

## Third-party references
- **Ingenic IMP** audio/video SDK — proprietary (Ingenic). Not included.
- **[gtxaspec/ingenic-audiodaemon](https://github.com/gtxaspec/ingenic-audiodaemon)** — referenced
  in [docs/AUDIO.md](docs/AUDIO.md) as the working-sequence reference for AO output.
- **[thingino-firmware](https://github.com/themactep/thingino-firmware)** — referenced for the
  full-firmware (Track B) path.

## Warranty
None. Flashing embedded devices can brick them. Only `mtd4` is written by the documented
procedure, and a self-taken `mtd4` backup is your revert path — but you assume all risk.
