# Building from your own stock `/system` (and keeping the voice prompts)

The committed `firmware/mtd4_integrated.bin` is built **from scratch** with only our own files —
so it carries **no vendor voice-prompt media** (`*.opus`/`*.g711a`/logo) and no per-unit secret.
The camera works fully but loses its spoken prompts ("wifi link success", power chimes).

If you want an image that keeps the vendor prompts, build it against **your own** camera's stock
`/system` partition. This never redistributes vendor content — it stays on your machine.

## 1. Dump your stock `/system` (mtd4)

On the camera (serial root shell), copy the partition off and get it to your PC (SD or TFTP):

```sh
cat /dev/mtd4 > /mnt/sda0/mtd4_system_STOCK.bin
md5sum /mnt/sda0/mtd4_system_STOCK.bin      # record it
```

This is a raw squashfs image padded to the partition size; `unsquashfs` reads it directly.

## 2. Point the stock-based builder at it

`src/build_integrated.sh` unsquashes your stock `/system`, **adds** our files on top (so the
vendor voice prompts are preserved), sets your creds, and repacks. Edit the `STOCK_MTD4` path near
the top of the script to your dump, or drop it where the script expects, then:

```sh
DEVPW=<your-unit-devpw> VUID=<your-unit-vuid> bash src/build_integrated.sh
```

It writes `mtd4_integrated.bin` and verifies the repack (re-unsquashes, checks the shim md5s and
the LD_PRELOAD line). If the squashfs exceeds the `mtd4` budget (~384 KiB usable), trim unused
`/system/www` language voice-prompt folders (e.g. keep `EN`, drop `CN`/`BN`) — the script warns
when it's over budget.

## 3. Flash it

Same as any image — **back up your current `mtd4` first**, then `flashcp`. See
[FLASHING.md](FLASHING.md).

## Which builder should I use?

| | `build_clean_image.sh` (committed image) | `build_integrated.sh` (from your stock) |
|---|---|---|
| Vendor voice prompts | **removed** | **kept** |
| Needs your stock `/system` dump | no | **yes** |
| Redistributable output | yes (no vendor content) | no — keep it private (contains vendor media) |
| Everything else (video/ONVIF/audio/shims) | identical | identical |

Both produce a functionally identical camera for streaming; the only difference is the spoken
prompts and whether the resulting `.bin` is safe to share.
