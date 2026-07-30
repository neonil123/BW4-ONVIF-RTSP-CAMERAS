# WireGuard on the camera

Deploy the camera on any network anywhere and reach its RTSP/ONVIF over your own
WireGuard VPN. The camera dials **out** to your WG server, so it works behind any
NAT with no port-forwarding — from another peer on the tunnel you just hit the
camera's WG IP (`rtsp://10.x.x.x:554/live`).

There are two ways to run it. **The kernel module is the recommended path** — it's
~170 KB, fits in flash, runs in-kernel (fast, RAM-cheap), and needs no SD card.
The userspace **boringtun** path is kept as a fallback (see the end).

The camera runs Linux **3.10**, which predates in-kernel WireGuard (kernel 5.6),
so neither path is "just enable it" — both took real porting.

---

## Recommended: the kernel module (`wireguard.ko`)

### Why it's even possible

The vendor kernel (`3.10.14-Archon`) has **`CONFIG_MODVERSIONS` off** — its own
modules carry no `__versions` section, so the loader accepts any module whose
**vermagic string matches**, without per-symbol CRC checks. So a `wireguard.ko`
built against a *compatible* 3.10.14 tree loads, as long as the symbols it imports
are exported (only one wasn't — see below). Check your unit:

```sh
# on the camera: is MODVERSIONS off, and what is the exact vermagic?
readelf -p .modinfo /lib/modules/<some-vendor>.ko | grep -i vermagic
readelf -S /lib/modules/<some-vendor>.ko | grep __versions   # empty = MODVERSIONS off
```

For this SoC: `vermagic=3.10.14-Archon preempt mod_unload MIPS32_R1 32BIT`,
MODVERSIONS off, MIPS32 **R1** (o32) — note the kernel is R1 even though userspace
is r2.

### Build (`src/wireguard/kmod/`)

1. **Kernel tree** — thingino's Ingenic 3.10.14 kernel builds WireGuard on this
   exact SoC: `git clone --depth 1 https://github.com/themactep/thingino-linux`.
2. **Configure to match the vermagic** (`prep_kernel.sh`): a T23 defconfig, then
   `PREEMPT=y`, `SMP=n`, `MODULE_UNLOAD=y`, `MODVERSIONS=n`, `CPU_MIPS32_R1=y`,
   `CONFIG_LOCALVERSION="-Archon"` (set it **once** — in `.config` *or* the env,
   not both, or you get `-Archon-Archon`). Then `make modules_prepare`.
3. **gcc-11 vs a 2013 kernel**: create the missing `compiler-gccN.h` by copying
   the highest present (`cp include/linux/compiler-gcc5.h compiler-gcc{6..12}.h`).
4. **Module** (`build_module.sh`): build
   [wireguard-linux-compat](https://git.zx2c4.com/wireguard-linux-compat) (the
   out-of-tree module for kernels 3.10–5.5; it bundles its own `udp_tunnel`/crypto
   compat) with `-include wg_compat_fix.h`, which supplies the gaps this *modified*
   Ingenic kernel leaves in the mainline-targeted compat shims:
   - `IFF_XMIT_DST_RELEASE`, `fallthrough`, and pulling `netdevice.h` in early so
     `netif_keep_dst()` sees a complete `struct net_device`;
   - a self-contained `ip_tunnel_get_stats64` — **the only symbol of 139 the
     Archon kernel doesn't export**, so we provide it instead of importing it.
5. **Match the vermagic exactly** and `insmod`. Result: `wireguard 126304 Live`.

Prebuilt: `bin/wireguard.ko` (~170 KB, vermagic `3.10.14-Archon …`).

### Creating and configuring the interface without iproute2

The device has no `ip`/iproute2 and (to fit flash) no 220 KB `wg` tool. Instead,
**`src/wireguard/kmod/wgctl.c`** (~60 KB static) does both jobs over netlink:

- creates the netdev — `RTM_NEWLINK` with `IFLA_INFO_KIND="wireguard"` (rtnetlink);
- configures it — `WG_CMD_SET_DEVICE` (genetlink) from a standard wg-quick `.conf`.

> **Gotcha that cost hours:** the WireGuard genl **uapi attribute numbers** must be
> exact. `WGPEER_A_ALLOWEDIPS` is **9**, not 8 (8 is `TX_BYTES`) — get it wrong and
> the kernel *silently drops* allowed-ips, then every packet returns `ENOKEY`
> (errno **161** on MIPS). Likewise `WGDEVICE_A_FLAGS`=5 and `LISTEN_PORT`=6 (a
> wrong FLAGS shows up as a phantom `listening port: 1`). See the enums in
> `wgctl.c`. Nested attrs use `NLA_F_NESTED` (0x8000); list elements are typed by
> index. Endpoints are IPv4-only (resolve hostnames when you write the config).

### Everything in flash — no SD

`load_cfg.sh` reads the baked config out of **mtd4's last block** (our `/system`
partition; the vendor never touches it) into `/tmp` at boot (stripping CRLF from
Windows configs). The boot wrapper then, if WireGuard is enabled, runs
`wg_up_kmod.sh`: `insmod wireguard.ko`, `wgctl` to create+configure `wg0`, then
busybox `ifconfig`/`route`. The image installs `wireguard.ko` + `wgctl` in
`/system` (~308 KB squashfs, comfortably under the 384 KB partition). So the SD
holds nothing WireGuard-related; credentials live in read-only flash.

### Cloud-block interaction

The integrated image installs reject routes (`0.0.0.0/1` + `128.0.0.0/1`) that keep
the camera off the vendor cloud. `wg_up_kmod.sh` **keeps** them and punches one
host-route hole to your WG endpoint; the WG subnet is reached via the more-specific
`wg0` route. Net posture: **LAN + your WG server only.**

---

## Fallback: userspace boringtun

If you can't build/load the module, [boringtun](https://github.com/cloudflare/boringtun)
(Cloudflare's userspace WG) runs over the kernel's built-in TUN device. It's ~1 MB
(so it lives on the SD, not flash) with ~108 KB RSS. Porting it to this MIPS target
needed five fixes (`src/wireguard/patch_boringtun.py` applies the source ones):

1. **ioctl encoding** — `TUNSETIFF` is `0x800454ca` on MIPS (BSD-style dir bits),
   not the generic `0x400454ca`; wrong value → `EBADFD`.
2/3. **IPv6 absent** — the kernel has no `AF_INET6`; boringtun opened a v6 socket
   unconditionally and two handlers panicked on the missing `udp6`. Made optional.
4. **Static, non-PIE** — uClibc has no musl loader; build fully static + non-PIE
   (tier-3 `mipsel-unknown-linux-musl` needs nightly `-Zbuild-std`; `-lunwind`
   satisfied by the toolchain's `libgcc_eh.a`).
5. **Runtime flags** — `--disable-connected-udp` (the connected-UDP path aborts on
   3.10), `--disable-multi-queue`; run `-f` under a restart supervisor, not its own
   daemonize.

Build: `src/wireguard/build_boringtun.sh` + `build_wgtools.sh`. Bring-up:
`src/wireguard/wg_up.sh`. Prebuilt: `bin/boringtun-cli`, `bin/wg`.

---

## Example config

Fill in your own values — **never commit real keys**. IPv4 endpoints only.

```ini
[Interface]
PrivateKey = <camera-private-key>
Address = 10.9.0.2/32
[Peer]
PublicKey = <server-public-key>
PresharedKey = <optional-psk>
Endpoint = 203.0.113.4:51820
AllowedIPs = 10.9.0.0/24
PersistentKeepalive = 25
```
