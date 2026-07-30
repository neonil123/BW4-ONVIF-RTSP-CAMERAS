# WireGuard on the camera (userspace)

Deploy the camera on any network anywhere and reach its RTSP/ONVIF over your own
WireGuard VPN. The camera dials **out** to your WG server, so it works behind any
NAT with no port-forwarding — from another peer on the tunnel you just hit the
camera's WG IP (`rtsp://10.x.x.x:554/live`).

## Why userspace (boringtun)

WireGuard became a Linux kernel feature in **5.6**. This camera runs kernel
**3.10**, which has no in-kernel WireGuard and no `wireguard.ko`, and the vendor
ships no kernel source. So we run **[boringtun](https://github.com/cloudflare/boringtun)**,
Cloudflare's userspace WireGuard, over the kernel's built-in TUN device
(`/dev/net/tun` is present). Measured resident set on-device: ~108 KB.

The tool `wg` (wireguard-tools) configures the interface via boringtun's
userspace UAPI socket — no netlink/kernel module involved.

## Porting boringtun to MIPS (the hard part)

boringtun is not built for this target. Five fixes were needed
(`src/wireguard/patch_boringtun.py` applies the source ones idempotently):

1. **ioctl encoding.** boringtun hardcodes the generic-Linux `TUNSETIFF`
   (`0x400454ca`). MIPS uses BSD-style ioctl direction bits, so it must be
   `0x800454ca` there. Wrong value → `EBADFD`, tunnel never comes up.
2. **IPv6 absent.** The kernel has no `AF_INET6`; boringtun opened a v6 UDP
   socket unconditionally (`EAFNOSUPPORT`). Made best-effort (`udp6 = None`).
3. **Two packet handlers** (`device/mod.rs` timer + iface handlers) assumed
   `udp6` was always present and panicked / early-returned when it was `None`.
   Made `udp6` optional in both.
4. **Static, non-PIE.** The device is uClibc with no musl loader, so the binary
   must be fully static + non-PIE (`crt-static`, `-static -no-pie`,
   `link-self-contained=no`, `panic=abort`; `-lunwind` satisfied by the
   toolchain's `libgcc_eh.a`). Tier-3 `mipsel-unknown-linux-musl` needs nightly
   `-Zbuild-std`.
5. **Runtime flags:** `--disable-connected-udp` (the connected-UDP path aborts on
   3.10), `--disable-multi-queue` (old TUN driver), `-t 1`. Don't use boringtun's
   own daemonize (unstable here) — run `-f` under a small restart supervisor.

`ring` (a dependency) does compile for MIPS via its portable fallback.

Build: `src/wireguard/build_boringtun.sh` (boringtun) and `build_wgtools.sh`
(`wg`). Prebuilt MIPS32 LE static binaries are in `bin/boringtun-cli` and `bin/wg`.

## Bring-up

`src/wireguard/wg_up.sh` consumes a standard wg-quick `.conf` (busybox-only parse
— no iproute2/awk/sed): it starts boringtun under a HUP-trapped restart
supervisor, configures `wg0` with `ifconfig`/`route`, sets the peer via `wg` with
`PersistentKeepalive`, and pins a host route to the server endpoint through the
real gateway.

Example config (fill in your own values — **never commit real keys**):

```ini
[Interface]
PrivateKey = <camera-private-key>
Address = 10.9.0.2/32
[Peer]
PublicKey = <server-public-key>
PresharedKey = <optional-psk>
Endpoint = vpn.example.com:51820
AllowedIPs = 10.9.0.0/24
PersistentKeepalive = 25
```

## Cloud-block interaction

The integrated image installs reject routes (`0.0.0.0/1` + `128.0.0.0/1`) that
keep the camera off the vendor cloud. `wg_up.sh` **keeps** them and punches a
single host-route hole to your WG endpoint; the WG subnet is reached via the
more-specific `wg0` route. Net posture: **LAN + your WG server only** — the
camera cannot phone home anywhere else.

## Storage note

boringtun is ~1 MB; the `/system` partition is 384 KB and the flash is fully
partitioned, so the binary is delivered on the SD card (it mmaps from there,
staying reclaimable and RAM-cheap). The binary is public software, not a secret.
Configuration (keys) can be baked into the image's own mtd4 free space instead of
the SD so credentials never sit on a removable card.
