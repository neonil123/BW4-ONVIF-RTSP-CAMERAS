# WireGuard on the camera

Deploy the camera on any network anywhere and reach its RTSP/ONVIF over your own
WireGuard VPN. The camera dials **out** to your WG server, so it works behind any
NAT with no port-forwarding — from another peer on the tunnel you just hit the
camera's WG IP (`rtsp://10.9.0.2:554/live`).

> ### ⚠ Status: proven on the bench, **not** part of the default image
>
> Everything below was built and verified end-to-end on the author's cameras
> (RTSP streaming over the tunnel), but **this repo does not ship a WireGuard-
> enabled firmware image**. Neither `src/build_clean_image.sh` nor
> `src/build_integrated.sh` references WireGuard, and `firmware/mtd4_integrated.bin`
> contains no WireGuard component — its `/system/lib` holds only the five
> LD_PRELOAD shims and `/system/bin` only `cam_onvifd` plus the `vp_project`
> wrapper. There is also **no tool in this repo that writes the config blob into
> flash**, even though `load_cfg.sh` reads it from there.
>
> WireGuard is therefore a **do-it-yourself integration**: the pieces are all
> here, but you assemble the image. See
> [Integrating it yourself](#integrating-it-yourself-what-you-must-build) for the
> exact steps.

There are two ways to run it. **The kernel module is the recommended path** — it's
~167 KB, fits in flash, runs in-kernel (fast, RAM-cheap), and needs no SD card.
The userspace **boringtun** path is kept as a fallback (see the end).

The camera runs Linux **3.10**, which predates in-kernel WireGuard (kernel 5.6),
so neither path is "just enable it" — both took real porting.

---

## What is shipped vs. what you must do

| Component | In this repo? | Where |
|---|---|---|
| `wireguard.ko` (prebuilt, vermagic `3.10.14-Archon …`) | **yes** | `bin/wireguard.ko` (167 KB) |
| `wgctl` (prebuilt static netlink configurator) | **yes** | `bin/wgctl` (60 KB) |
| `boringtun-cli`, `wg` (prebuilt, fallback path) | **yes** | `bin/boringtun-cli` (1.4 MB), `bin/wg` (215 KB) |
| Kernel-module build recipe | **yes** | `src/wireguard/kmod/{clone_kernel,prep_kernel,build_module,build_wgctl}.sh`, `wg_compat_fix.h`, `wgctl.c`, `wg_create.c` |
| boringtun build recipe | **yes** | `src/wireguard/build_boringtun.sh`, `build_wgtools.sh`, `patch_boringtun.py` |
| Runtime scripts (config loader + bring-up) | **yes** | `src/wireguard/load_cfg.sh`, `wg_up_kmod.sh`, `wg_up.sh` |
| A firmware image containing any of the above | **no** | you build it — see below |
| A tool that bakes the config blob into mtd4 | **no** | you write it — snippet below |
| Boot-wrapper hook that calls `load_cfg.sh` / `wg_up*.sh` | **no** | you add it — snippet below |

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
is r2. The shipped `bin/wireguard.ko` carries exactly that vermagic; verify it the
same way before trusting it on your unit:

```sh
readelf -p .modinfo bin/wireguard.ko | grep -i vermagic
# vermagic=3.10.14-Archon preempt mod_unload MIPS32_R1 32BIT
```

### Build (`src/wireguard/kmod/`)

1. **Kernel tree** — thingino's Ingenic 3.10.14 kernel builds WireGuard on this
   exact SoC: `git clone --depth 1 https://github.com/themactep/thingino-linux`
   (`clone_kernel.sh`).
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

Prebuilt: `bin/wireguard.ko` (~167 KB, vermagic `3.10.14-Archon …`).

> ### Reproducibility warning: nothing upstream is pinned
>
> `clone_kernel.sh` does `git clone --depth 1` of **`themactep/thingino-linux`**
> and **`wireguard-linux-compat`** with **no pinned commit or tag**;
> `build_boringtun.sh` and `build_wgtools.sh` likewise shallow-clone
> `cloudflare/boringtun` and `wireguard-tools` at whatever HEAD is current (the
> latter only falls back to a `1.0.20210914` tarball if the clone fails). The
> boringtun path additionally needs an **unpinned Rust nightly** with
> `-Z build-std`, because `mipsel-unknown-linux-musl` is a **tier-3** target with
> no prebuilt `std` — and `build-std` is notoriously sensitive to the exact
> nightly date.
>
> Consequence: a rebuild months from now may not reproduce the shipped binaries,
> or may not build at all. If you get a working build, **record and pin** the
> kernel/compat/boringtun commit hashes and the nightly toolchain date
> (`rustup toolchain list`, `cargo +nightly -V`) before you need them again.

### Creating and configuring the interface without iproute2

The device has no `ip`/iproute2 and (to fit flash) no 215 KB `wg` tool. Instead,
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

### Config in flash — no SD (by design)

`load_cfg.sh` reads the baked config out of **mtd4** — our `/system` partition,
which the vendor never touches — into `/tmp` at boot (stripping CRLF from Windows
configs), so no WireGuard material ever sits on the SD and credentials live in
read-only flash. Concretely it does:

```sh
dd if=/dev/mtd4 bs=4096 skip=88 count=2   # 8 KB at offset 0x58000
```

i.e. **offset 0x58000** (`4096 * 88 = 360448`), well past the squashfs, inside the
last erase block of the 0x60000-byte partition. (The comment at the top of
`load_cfg.sh` says `0x40000`; the `dd` is authoritative — it reads `0x58000`.) It
writes `/tmp/wifi.ini`, `/tmp/wireguard.conf` and `/tmp/wg_default` (mode 0600).

`wg_up_kmod.sh` then does the bring-up: `insmod wireguard.ko`, `wgctl` to
create+configure `wg0`, then busybox `ifconfig`/`route`. It looks for the module
and tool at `/system/lib/wireguard.ko` and `/system/bin/wgctl` (falling back to
`/mnt/sda0/…`, overridable via `KO_OVR` / `CTL_OVR`).

**None of this runs in the shipped image** — nothing calls `load_cfg.sh`, and the
image contains neither the module nor `wgctl`. See the next section.

### Cloud-block interaction

The integrated image installs reject routes (`0.0.0.0/1` + `128.0.0.0/1`) that keep
the camera off the vendor cloud. `wg_up_kmod.sh` (and `wg_up.sh`) **keep** them and
punch one host-route hole to your WG endpoint via the real default gateway; the WG
subnet is reached via the more-specific `wg0` route. Net posture:
**LAN + your WG server only.**

---

## Integrating it yourself (what you must build)

Four things to do. Work on **your own copy** of the build script — the ones in
`src/` deliberately have no WireGuard in them.

### 1. Put the binaries and scripts into `/system`

In your copy of `src/build_clean_image.sh` (or `build_integrated.sh`), before the
`mksquashfs` step, add:

```sh
cp "$BIN/wireguard.ko"                 "$W/sys/lib/wireguard.ko"
cp "$BIN/wgctl"                        "$W/sys/bin/wgctl"
cp "$ROOT/src/wireguard/load_cfg.sh"   "$W/sys/bin/load_cfg.sh"
cp "$ROOT/src/wireguard/wg_up_kmod.sh" "$W/sys/bin/wg_up_kmod.sh"
chmod 0755 "$W/sys/bin/wgctl" "$W/sys/bin/"*.sh
```

**Size budget.** The shipped clean image is only 91,758 bytes of squashfs inside
the 393,216-byte (0x60000) partition, so there is ample room; on the author's
bench a `/system` carrying the module + `wgctl` came to roughly 308 KB. But note
the real ceiling is **not** the 393,216 the build scripts check — the config blob
lives at **0x58000 (360,448)**, so your squashfs must end *before* that. Add your
own check; the stock `[ $NEW -le $MTD4_SZ ]` test will not catch an overlap.

### 2. Hook it into the boot wrapper

The `/system/bin/vp_project` wrapper generated by the build script is the only
thing that runs on boot. Add a background stanza to it (after the route block),
mirroring how it already stages `cam_onvifd`: `/system` is a read-only squashfs
and packaged files may lack `+x`, so copy to tmpfs and `chmod` there.

```sh
# WireGuard: load baked config, bring up wg0 if enabled
( cp /system/bin/load_cfg.sh /system/bin/wg_up_kmod.sh /tmp/ 2>/dev/null
  chmod 0755 /tmp/load_cfg.sh /tmp/wg_up_kmod.sh 2>/dev/null
  /tmp/load_cfg.sh
  [ "$(cat /tmp/wg_default 2>/dev/null)" = "on" ] || exit 0
  n=0; while [ $n -lt 60 ]; do            # wait for the uplink to have an IP
    ifconfig vnet0 2>/dev/null | grep -q "inet addr" && break
    n=$((n+1)); sleep 3
  done
  KO_OVR=/system/lib/wireguard.ko CTL_OVR=/system/bin/wgctl \
    /tmp/wg_up_kmod.sh /tmp/wireguard.conf > /tmp/wg0.log 2>&1 ) &
```

Bring it up **after** the wrapper's cloud-block reject routes are installed, so
the endpoint host route is punched through them rather than being overwritten.

### 3. Bake the config blob at 0x58000

No tool in this repo does this. The blob is plain text in the format documented at
the top of `load_cfg.sh`, and the built `.bin` is zero-padded to the full 393,216
bytes, so you write it in place after the build (the whole partition is flashed):

```python
# bake_cfg.py IMAGE.bin wireguard.conf  ->  writes the blob at 0x58000, in place
import sys
img, conf = sys.argv[1], sys.argv[2]
wg = open(conf, 'rb').read().replace(b'\r\n', b'\n')
blob = b'###BW4CFG1###\n###WGDEFAULT=on###\n###WG###\n' + wg + b'\n###END###\n'
assert len(blob) <= 8192, 'blob must fit the 8 KB load_cfg.sh reads'
d = bytearray(open(img, 'rb').read())
assert len(d) == 0x60000
d[0x58000:0x58000 + len(blob)] = blob
open(img, 'wb').write(d)
```

Add a `###WIFI###` section (with `SSID=` / `PASSWORD=` lines) before `###WG###` if
you also want the baked Wi-Fi credentials `load_cfg.sh` supports. Set
`###WGDEFAULT=off###` to ship the tunnel disabled.

**This image now contains your private key.** Treat the built `.bin` as a secret:
never commit it, never post it in a bug report. Keep the plaintext `.conf` out of
the repo too (`.gitignore` it).

### 4. Verify on the camera

```sh
cat /tmp/wg_default            # on
head -3 /tmp/wireguard.conf    # your [Interface]
lsmod | grep wireguard         # wireguard 126304 0 - Live
ifconfig wg0                   # inet addr:10.9.0.2 MTU:1420
route -n                       # host route to the endpoint via the LAN gateway
```

Then, from another peer on the tunnel: `rtsp://10.9.0.2:554/live`.

---

## Fallback: userspace boringtun

If you can't build/load the module, [boringtun](https://github.com/cloudflare/boringtun)
(Cloudflare's userspace WG) runs over the kernel's built-in TUN device. It's ~1.4 MB
(so it lives on the SD, not flash) with ~108 KB RSS. Porting it to this MIPS target
needed five fixes (`build_boringtun.sh` applies the ioctl patch inline; the IPv6 one
is `src/wireguard/patch_boringtun.py`):

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

Build: `src/wireguard/build_boringtun.sh` + `build_wgtools.sh` (both unpinned — see
the reproducibility warning above). Bring-up: `src/wireguard/wg_up.sh`, which reads
the same `/tmp/wireguard.conf` and keeps the same route posture. Prebuilt:
`bin/boringtun-cli`, `bin/wg`. This path is likewise **not** wired into any shipped
image; put the two binaries on the SD and call `wg_up.sh` from your boot wrapper.

---

## Example config

Fill in your own values — **never commit real keys**. IPv4 endpoints only. The
addresses below are documentation-only placeholders
([RFC 5737](https://www.rfc-editor.org/rfc/rfc5737) `203.0.113.0/24`, RFC 1918
`10.9.0.0/24`); substitute your own tunnel subnet and server endpoint.

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

---

## Licensing

The prebuilt WireGuard components in `bin/` (`wireguard.ko`, `wg`, `boringtun-cli`)
are third-party code under their own licenses — see the third-party section of
[NOTICE.md](../NOTICE.md).
