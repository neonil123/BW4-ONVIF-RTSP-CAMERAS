# NOTICE — scope, licences, secrets, and what is (not) redistributed

## Intended use
This project documents and tooling covers modifications to **cameras the operator owns**, as an
authorized right-to-repair / interoperability exercise: making the device speak the open ONVIF and
RTSP standards on the local network instead of depending on a vendor cloud. It is provided for
that purpose. Do not use it against hardware you do not own or are not authorized to modify.

## Scope of the MIT licence
[LICENSE](LICENSE) is a plain MIT licence and it covers **this project's own work only** — the
`cam_onvifd` daemon (`src/onvif_rtsp/`), the five `LD_PRELOAD` shims (`src/shims/`), `wgctl`
(`src/wireguard/kmod/wgctl.c`, `wg_create.c`, `wg_compat_fix.h`), every build/deploy script under
`src/` and `tools/`, the configuration examples, and the documentation.

It does **not**:

- **grant rights to vendor software.** No Ingenic IMP SDK code, no vendor application
  (`vp_project`), and no kernel or bootloader image is included here. The MIT-licensed code
  interoperates with those via documented runtime addresses and ABIs only.
- **cover the third-party binaries in `bin/`.** `bin/wireguard.ko`, `bin/wg` and
  `bin/boringtun-cli` are other people's work, redistributed unmodified-in-licence under
  **their own terms** (GPLv2 / GPLv2 / BSD-3-Clause). See
  *[Third-party software redistributed here](#third-party-software-redistributed-here-bin)* below.
  Nothing in this repository relicenses them, and the MIT grant does not extend to them.

The MIT-licensed programs do not link against the GPL'd works: `cam_onvifd` never touches them at
all, and `wgctl` talks to the kernel module over the ordinary Linux **netlink syscall interface**
(see the licensing note under `bin/wgctl` below). Shipping them side by side in one repository is
mere aggregation, not a combined work.

## No secrets are published here
- The camera's **admin device password** and **device id (vuid)** are **per-unit random values**
  stored in the device's `mtd5` NVS. They are **not** defaults and are **not** contained anywhere
  in this repository.
- Every committed configuration and firmware artifact ships **`CHANGE_ME`** placeholders. You read
  your own unit's values off your own device and supply them at build time (see
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) → *Device password*).
- **Never commit** an image or config built with a real device password, a full-chip flash dump
  (it contains your NVS/Wi-Fi credentials), or any capture that includes credentials.
- WireGuard `PrivateKey` / `PresharedKey` values are secrets too. The shipped configs and docs use
  placeholders only; do not commit a real tunnel config, and do not bake one into an image you
  then publish.

## No vendor firmware or media is redistributed
- The prebuilt overlay in `firmware/` is assembled **from scratch** and contains **only**:
  our own compiled binaries (`cam_onvifd`, the `*.so` shims), our own shell wrapper, our own
  config with placeholder creds, and a few **neutral text stubs** (`language.txt`, an empty
  `upgrade.txt`, a community `version.ini`).
- It deliberately excludes the vendor's `/system` content — the voice-prompt audio (`*.opus`,
  `*.g711a`) and logo — which are the vendor's copyrighted assets. As a result the camera loses
  its spoken prompts; everything else is unaffected. If you want them back, rebuild against **your
  own** camera's stock `/system` dump with `src/build_integrated.sh` (see
  [docs/STOCK_SYSTEM.md](docs/STOCK_SYSTEM.md)).
- No Ingenic IMP SDK code, no vendor application (`vp_project`), and no kernel/bootloader images
  are included. Our code interoperates with those via documented runtime addresses and ABIs only.
- The prebuilt `firmware/mtd4_integrated.bin` also carries **no third-party binaries at all** — its
  squashfs holds `bin/cam_onvifd`, our `bin/vp_project` wrapper, the five `lib/*.so` shims and the
  text stubs, and nothing else. The WireGuard components below are shipped **only** as loose files
  in `bin/`, for the optional VPN path in [docs/WIREGUARD.md](docs/WIREGUARD.md); you opt into them
  by building your own image.

## Third-party software redistributed here (`bin/`)

Three of the files in `bin/` are **not** this project's work and are **not** MIT-licensed. They are
redistributed here under the licences below. Everything else in `bin/` (`cam_onvifd`, `wgctl` and
the five `*.so` shims) is this project's own work under [LICENSE](LICENSE).

| File | Upstream project | Version | Licence |
|---|---|---|---|
| `bin/wireguard.ko` | [wireguard-linux-compat](https://git.zx2c4.com/wireguard-linux-compat) (mirror: [github.com/WireGuard/wireguard-linux-compat](https://github.com/WireGuard/wireguard-linux-compat)) | `1.0.20220627` | **GPL-2.0-only** |
| `bin/wg` | [wireguard-tools](https://git.zx2c4.com/wireguard-tools) (mirror: [github.com/WireGuard/wireguard-tools](https://github.com/WireGuard/wireguard-tools)) | `1.0.20260223` | **GPL-2.0-only** (sources also offer MIT — see below) |
| `bin/boringtun-cli` | [boringtun](https://github.com/cloudflare/boringtun) | `0.7.1` | **BSD-3-Clause** |

### `bin/wireguard.ko` — WireGuard kernel module (GPLv2)
Copyright (C) Jason A. Donenfeld \<Jason@zx2c4.com\> and contributors.

Read straight out of the shipped file (`readelf -p .modinfo bin/wireguard.ko`):

```
version=1.0.20220627
author=Jason A. Donenfeld <Jason@zx2c4.com>
description=WireGuard secure network tunnel
license=GPL v2
srcversion=B2A68D2111961A327BD1C82
vermagic=3.10.14-Archon preempt mod_unload MIPS32_R1 32BIT
```

`1.0.20220627` is upstream's newest `wireguard-linux-compat` release tag. It is a Linux kernel
module — a derived work of the kernel — cross-compiled against
[themactep/thingino-linux](https://github.com/themactep/thingino-linux) (Ingenic's Linux 3.10.14,
GPLv2). Neither the kernel tree nor the WireGuard source is vendored into this repository; both are
fetched by `src/wireguard/kmod/clone_kernel.sh`.

**This project's modifications** to make it build against a 3.10.14 tree with GCC 11 are
GPLv2-compatible and are published in full, in source form, in this repository:
`src/wireguard/kmod/wg_compat_fix.h` (force-included compat defines) plus the header-shim and
`KCFLAGS` handling in `src/wireguard/kmod/build_module.sh` and `prep_kernel.sh`.

### `bin/wg` — WireGuard userspace configuration tool (GPLv2)
Copyright (C) 2015-2026 Jason A. Donenfeld \<Jason@zx2c4.com\>. All Rights Reserved.

The binary self-reports `wireguard-tools v1.0.20260223 - https://git.zx2c4.com/wireguard-tools/`,
which matches upstream tag `v1.0.20260223`. Upstream's `README.md` states *"This project is
released under the GPLv2"* and ships the GPLv2 text as `COPYING`; the individual C sources
additionally carry `SPDX-License-Identifier: GPL-2.0 OR MIT`, so a recipient may elect either. This
repository makes no election on your behalf — **treat the shipped binary as GPLv2**, which is the
stricter and safer reading.

Built by `src/wireguard/build_wgtools.sh` with **no source modifications** (only `CFLAGS`/`LDFLAGS`
for a static mipsel build).

### `bin/boringtun-cli` — userspace WireGuard implementation (BSD-3-Clause)
Copyright (c) 2019 Cloudflare, Inc. All rights reserved.

The binary embeds `boringtun 0.7.1` and `Vlad Krasnov <vlad@cloudflare.com>`; upstream
`boringtun/Cargo.toml` for 0.7.1 declares `license = "BSD-3-Clause"` and the repository ships
`LICENSE.md` with the Cloudflare BSD-3-Clause text, reproduced in full at the end of this file as
BSD-3-Clause clause 2 requires for binary redistribution.

**This project's modifications** are published as source in this repository and are applied by the
build script at build time, not vendored:
- `src/wireguard/build_boringtun.sh` — arch-aware `TUNSETIFF` (MIPS uses BSD-style ioctl direction
  bits: `0x8004_54ca`, not `0x4004_54ca`);
- `src/wireguard/patch_boringtun.py` — makes the IPv6 listen socket best-effort, because this
  kernel has no IPv6 and the unconditional `AF_INET6` socket fails `EAFNOSUPPORT` at startup.

### `bin/wgctl` — this project's own work (MIT), with a note on the interface
`bin/wgctl` is built from `src/wireguard/kmod/wgctl.c`, which is original code written for this
project and is covered by [LICENSE](LICENSE) (MIT). It contains no WireGuard code.

It does re-declare the WireGuard **generic-netlink UAPI constants** (`WG_GENL_NAME`,
`WG_CMD_SET_DEVICE`, the `WGDEVICE_A_*` / `WGPEER_A_*` / `WGALLOWEDIP_A_*` attribute numbers) so it
can configure the module without linking `libmnl` or shipping the 220 KB `wg` tool. Those values
come from WireGuard's `uapi/linux/wireguard.h`, whose upstream header is
`SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR MIT`, Copyright (C) 2015-2019
Jason A. Donenfeld — i.e. explicitly available under MIT, and in any case carrying the
Linux-syscall-note that exempts userspace programs which merely use the interface. The constants
are used here under the **MIT** option of that dual licence.

### Statically linked runtime components
`bin/cam_onvifd`, `bin/wgctl`, `bin/wg` and `bin/boringtun-cli` are fully static MIPS binaries built
with a `mipsel-linux-musl` cross-toolchain (see each `build*.sh`). They therefore embed:
- **[musl libc](https://musl.libc.org/)** — MIT licence, © Rich Felker and contributors;
- the **GCC runtime library** (`libgcc`) — GPLv3 *with the GCC Runtime Library Exception*, which
  permits distribution of the resulting binaries under any licence;
- for `boringtun-cli` only, the **Rust standard library** and its crate dependencies — MIT OR
  Apache-2.0.

None of these impose licence terms on this project's own source.

## Corresponding source for the GPL'd binaries (GPLv2 §3)

`bin/wireguard.ko` and `bin/wg` are GPLv2 object code, so their **complete corresponding source**
must be available to anyone who receives them.

- **What this repository provides today.** `src/wireguard/build_wgtools.sh`,
  `src/wireguard/kmod/clone_kernel.sh` and `src/wireguard/kmod/build_module.sh` fetch the upstream
  sources and reproduce the builds, and every local modification is published here in source form
  (`wg_compat_fix.h`; no modifications at all for `wg`). Together with the exact upstream URLs and
  version strings above, that is enough to rebuild both binaries.
- **Known gap — the upstream clones are not pinned.** All three scripts use
  `git clone --depth 1` against the default branch, so they record no tag or commit hash; a clone
  made later may not reproduce these binaries byte-for-byte. (The stale `VER=1.0.20210914` fallback
  in `build_wgtools.sh` is only a tarball fallback path and does **not** match the shipped `wg`,
  which is `1.0.20260223`.) Pinning to `v1.0.20260223` and `v1.0.20220627` respectively would close
  this. Fetching source over the network is also not the same thing as source *accompanying* the
  object code under GPLv2 §3(a).
- **Written offer (GPLv2 §3(b)).** For at least three years from the date you received these
  binaries, the maintainer of this repository will, on request, provide a complete machine-readable
  copy of the corresponding source for `bin/wireguard.ko` and `bin/wg` — including the exact
  upstream trees used and all modifications — for no more than the cost of physically performing
  the distribution. Open an issue on this repository to request it.
- The full GPLv2 text is **not** currently shipped as a separate file here; it is at
  <https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt> and as `COPYING` in both upstream trees.

If you redistribute this repository, or an image built from it that includes those binaries, these
obligations travel with you.

## Third-party software referenced but *not* redistributed
- **Ingenic IMP** audio/video SDK — proprietary (Ingenic). Not included; interoperated with via
  documented runtime addresses and ABIs only.
- **[gtxaspec/ingenic-audiodaemon](https://github.com/gtxaspec/ingenic-audiodaemon)** — referenced
  in [docs/AUDIO.md](docs/AUDIO.md) as the working-sequence reference for AO output.
- **[thingino-firmware](https://github.com/themactep/thingino-firmware)** — referenced for the
  full-firmware (Track B) path.
- **[thingino-linux](https://github.com/themactep/thingino-linux)** — the Ingenic Linux 3.10.14
  kernel tree `bin/wireguard.ko` is compiled against (GPLv2, Linus Torvalds and contributors).
  Fetched by `clone_kernel.sh`; no kernel source or image is stored in this repository.

## Warranty
None. Flashing embedded devices can brick them. Only `mtd4` is written by the documented
procedure, and a self-taken `mtd4` backup is your revert path — but you assume all risk.
This applies to the third-party binaries above as well: they are redistributed **as is**, and their
own licences disclaim all warranty (see the BSD-3-Clause disclaimer below and GPLv2 §§11-12).

---

## Appendix — BSD 3-Clause licence for boringtun (`bin/boringtun-cli`)

Verbatim from <https://github.com/cloudflare/boringtun/blob/master/LICENSE.md>:

```
Copyright (c) 2019 Cloudflare, Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted
provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice, this list of
       conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice, this list of
       conditions and the following disclaimer in the documentation and/or other materials
       provided with the distribution.
    3. Neither the name of the copyright holder nor the names of its contributors may be used to
       endorse or promote products derived from this software without specific prior written
       permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

WireGuard and the "WireGuard" logo are registered trademarks of Jason A. Donenfeld. This project is
not affiliated with or endorsed by Jason A. Donenfeld, Cloudflare, Inc., Ingenic, or the camera's
vendor.
