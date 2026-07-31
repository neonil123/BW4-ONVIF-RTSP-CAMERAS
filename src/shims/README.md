# `src/shims/` — the `LD_PRELOAD` shims

Five freestanding MIPS shared objects that augment the stock `vp_project` **without patching
its binary**. They are loaded into `vp_project` by the `/system/bin/vp_project` wrapper's
single `LD_PRELOAD` assignment; each one's `__attribute__((constructor))` spawns one detached
worker thread and returns. Prebuilt copies live in [`../../bin`](../../bin).

Deployment, the wrapper script, and the `mtd4` overlay are covered in
[`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) and
[`docs/FLASHING.md`](../../docs/FLASHING.md); per-feature detail is in
[`docs/FEATURES.md`](../../docs/FEATURES.md) and [`docs/AUDIO.md`](../../docs/AUDIO.md).

## What each shim does

| source | one-line summary |
|---|---|
| `camweb.c` | Calls `vp_project`'s compiled-out-but-present `vp_web_create_socket` (`0x47db44`) in a retry loop until the listen fd at `0x7e8db8` goes valid, bringing up the dormant vendor web/CGI server on **`0.0.0.0:81`** so `livestream.cgi` serves H.264 to the LAN. Also strips `LD_PRELOAD` from the environment — see below. |
| `wifi_sd.c` | Reads `/mnt/sda0/wifi.ini` (or `.txt`) off the SD card, parses `SSID=` / `PASSWORD=` with `parse_ini.h`, and calls the vendor WiFi-onboarding entry `0x453a2c(ssid, pwd, 2)`; renames the file to `*.applied` on success. Cloud-free onboarding. |
| `pir_sleep.c` | Asserts the vendor's own `Smart_Electricity_Sleep` switch (`0x821858` = 1) and threshold (`0x821859` = 50) every few seconds, so below ~50% battery the camera drops into the vendor AOV/PIR-wake low-power mode and resumes at ~60% (fixed vendor +10 hysteresis). |
| `battery_osd.c` | Calls the vendor battery getter `0x48d0ec` and rewrites OSD region 0's strftime format at `0x80d814` to `"%Y-%m-%d %H:%M:%S NN%%"`, setting the redraw dirty bit at `0x80d788`, so the live battery percentage renders on the video overlay. |
| `mic_capture.c` | Pure-read `IMP_AI` consumer (dev 1, chn 0): `PollingFrame`/`GetFrame`/`ReleaseFrame` only, no state change. Ships each 1280-byte S16LE 16 kHz frame as one UDP datagram to `MIC_DEST` (default `127.0.0.1:5599`), where `cam_onvifd` turns it into the RTP audio track. |

`parse_ini.h` is a header-only, libc-free parser shared verbatim between `wifi_sd.c` and the
host-side unit test, so it links cleanly into a `-nostdlib` object.

## Load order: `camweb.so` MUST be first

```sh
export LD_PRELOAD="/system/lib/camweb.so /system/lib/wifi_sd.so /system/lib/pir_sleep.so /system/lib/battery_osd.so"
#                  ^^^^^^^^^^ must lead
```

This is a **hard dependency, not a style preference.** Every shim imports `pthread_create`.
Inside `vp_project` that resolves fine — it links uClibc 0.9.33 and libpthread. But
`vp_project` spawns **busybox** children (`udhcpc`, `ifconfig`, `sh`) via `system()`, and
**busybox has no libpthread**. uClibc binds eagerly at load, so any busybox child that
*inherits* `LD_PRELOAD` cannot resolve `pthread_create` and dies outright:

```
/bin/busybox: can't resolve symbol 'pthread_create'
```

The consequence is not cosmetic: `udhcpc` never runs → no DHCP → **no WiFi** (no `vnet0`,
empty route table). That is what broke the early `camweb` revisions.

The fix is in `camweb_init()`, which calls **`unsetenv("LD_PRELOAD")` before it spawns
anything** — so children exec with a clean environment and load busybox normally, while all
five shims stay live inside `vp_project` because they were already mapped at process start.
`camweb.so` is the only shim that exports `unsetenv`, and it only protects the children that
are forked *after* its constructor runs, so it has to be the first entry in the chain.

Two documented dead ends: a plain worker-thread delay does **not** help (the failure is at
*load*, before any shim code runs), and declaring the imports `__attribute__((weak))` fails
on this uClibc (`can't load library ...` — weak-undef is not honored).

## Building

```sh
bash src/shims/build.sh                 # all five, output next to the sources
bash src/shims/build.sh camweb wifi_sd  # a subset
OUTDIR=/tmp/out bash src/shims/build.sh # elsewhere
CC=/path/to/mipsel-linux-musl-gcc bash src/shims/build.sh
```

`build.sh` cross-compiles for MIPS32r2 little-endian and then runs `verify.sh` on its own
output; it hard-fails if the cross-compiler is absent rather than producing nothing. Default
toolchain is the same one [`src/onvif_rtsp/build.sh`](../onvif_rtsp/build.sh) uses,
`~/x-tools/mipsel-linux-musl-cross/bin/mipsel-linux-musl-gcc`; override with `CC=`.

The core flags, derived by inspecting the shipped `bin/*.so`:

```
-shared -fPIC -Os -march=mips32r2 -mabi=32 -EL
-nostdlib -ffreestanding -fno-stack-protector -fno-builtin
```

These objects are **freestanding**: `readelf -d` shows *zero* `NEEDED` entries. Every libc
function they use is left **undefined** and resolved at load time from the host process
`vp_project`, which already has uClibc + libpthread mapped. No toolchain libc is linked in,
which is why a *musl* cross-compiler can legitimately build objects that run against
*uClibc* — and why the toolchain's libc flavour does not matter. `build.sh` carries the
per-file link differences (`-soname`, `-z now`, `-s`) that the original per-feature scripts
used; none of them affect loading.

## Verifying

```sh
bash src/shims/verify.sh              # checks the shipped bin/*.so
bash src/shims/verify.sh path/to.so   # checks specific files
```

`verify.sh` asserts the four properties that determine whether a shim will load and work
(this is test 1.7 in [`docs/TESTING.md`](../../docs/TESTING.md)):

1. **ELF32, little-endian, type `DYN`, machine MIPS, flags `o32` + `mips32r2` + `pic`.**
2. **No `NEEDED` entries** — a `NEEDED` would drag another library into the process.
3. **Non-empty `.init_array`** — no constructor means the shim silently does nothing.
4. **Undefined dynamic symbols are a subset of the uClibc allowlist** (`pthread_create`,
   `pthread_detach`, `sleep`, `unsetenv`, `open`, `read`, `write`, `close`, `rename`,
   `socket`, `sendto`, `getenv`, `atoi`, `__errno_location`). Anything outside it will not
   resolve, and for a shim in the boot `LD_PRELOAD` chain that means the camera does not
   come up.

A byte-identical rebuild is **not** required — only the shape above is. (In practice the
default gcc 11.2.1 toolchain does reproduce all five shipped `bin/*.so` bit-for-bit; the
md5s are recorded in `build.sh` as provenance, not as a gate.)

## Changing a shim

Every hardcoded address in these sources is a fixed runtime VA that is only valid because
`vp_project` is `ET_EXEC` / non-PIE at base `0x400000` with no ASLR on kernel 3.10. They were
RE'd from one specific `vp_project` build (md5 `5a8ea3edc499ffe644efaf2700ec037d`).
**Re-verify the addresses against the `vp_project` in your own partition before reflashing** —
a stale address means writing into the wrong global. Each shim is fully reversible: drop it
from `LD_PRELOAD` or delete the `.so`; no bytes of `vp_project` on disk are ever modified.
