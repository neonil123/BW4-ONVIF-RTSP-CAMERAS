# Hardware Disassembly & BootROM / USB-DFU Entry

This is the hardware procedure that puts a BW4 camera into USB update mode (the SoC's USB
BootROM) so a PC can write firmware to it over USB-DFU. It means taking the camera apart,
handling a Li-ion cell, and bridging two tiny flash pins during power-on. It is not a casual
step and it does require a steady hand.

> ## ⛔ Before you decide to DFU — read this
> **USB-DFU is a whole-chip write.** It rewrites the **`mtd5` NVS** partition, which **erases
> the Wi-Fi credentials**. After a full DFU you must re-onboard the camera and **re-read its
> per-unit device password** ([ARCHITECTURE.md](ARCHITECTURE.md) → *Device password*) before
> you can build a working image again.
>
> That is why this repo's **golden rule #2** is: **"NEVER DFU unless the camera won't boot at
> all."** See [FLASHING.md → *The golden rules*](FLASHING.md#the-golden-rules).
>
> **The documented stock → custom conversion needs no DFU whatsoever.** It is a serial root
> shell plus `flashcp` to `mtd4` (or the same write over SD / TFTP / the U-Boot `sf` prompt).
> It touches **only `mtd4`**, leaves `mtd0/2/3/5` stock, keeps your Wi-Fi creds, and is fully
> revert-safe from a backup you take yourself. **That is the default path — start at
> [FLASHING.md](FLASHING.md), not here.**

---

## When this procedure is the right tool

Two cases only:

- **(a) Rescue — the unit will not boot at all** and cannot be reached over serial, SD or
  TFTP. This is exactly the situation golden rule #2 carves out. Accept the NVS wipe, restore
  your own full-chip backup, then re-onboard.
- **(b) Optional — you have no serial access.** Every non-DFU write needs a console: the root
  shell for `flashcp`, the U-Boot `=>` prompt for `sf`, and the device-password read all run
  over UART. If you cannot or will not attach a 3.3 V USB-serial adapter (see *Serial / UART
  console* below), full-chip DFU is the only remaining way in — but you are **choosing** to pay
  the NVS wipe, and you must then re-onboard, e.g. with the `wifi_sd` SD-card method
  ([FEATURES.md](FEATURES.md#wifi_sdso--sd-card-wifi-onboarding)). The kit in
  [`tools/windows-flasher/`](../tools/windows-flasher/) is built around exactly this trade.

**If the camera boots, do not do this.** Soldering or clipping three UART pads is cheaper,
safer and reversible; a whole-chip DFU is not. And routine updates never need the case opened
at all — you flash `mtd4` over SD/TFTP/serial and leave the enclosure sealed.

---

> ## ⚠️ Safety — read before you start
> - **Power off and unplug** (USB-C out, battery unclipped) before you open anything.
> - **ESD:** ground yourself (wrist strap or touch bare metal) and work on a
>   **non-conductive surface**. The bare mainboard is static-sensitive.
> - **Li-ion care:** the 18650 cell (3.7 V / 4000 mAh) is a separate connector. Do **not**
>   puncture, crush, short, or bend it. Handle it gently and set it aside safely.
> - **Only short pins 5 + 6** of the flash chip. Do **not** let the tool touch neighbouring
>   pins or any power rail — the wrong pins can damage the board.

---

## What you need

- Small **Phillips screwdriver** (the enclosure/board screws are tiny).
- **Plastic pry tool / spatula** to peel the silicone cover and split the enclosure.
- **Fine tweezers or a thin jumper wire** to bridge the two flash pins.
- **USB-C cable** (any that carries power; a data cable is fine).
- A **3.3 V USB-serial (UART) adapter** — not for DFU itself, but you will want the console to
  verify the result and to read the device password. See *Serial / UART console* below.
- A **Windows PC** with the DFU tools **and the USB driver installed** (one-time per PC — see
  *Install the USB driver* below).

> ### The DFU tools are **not** shipped in this repo
> `thingino-dfu`, `dfu-util` and Zadig are third-party projects. Nothing under `tools/` here
> contains them — `tools/` holds only [`windows-flasher/`](../tools/windows-flasher/) (PowerShell
> scripts). Download them yourself:
>
> | Tool | Role | Upstream |
> |---|---|---|
> | `dfu-util` | all transfers (`-D` write / `-U` readback) | <https://dfu-util.sourceforge.net> |
> | `thingino-dfu` | BootROM bootstrap only (`-b`) | [themactep/thingino-firmware](https://github.com/themactep/thingino-firmware) |
> | Zadig | one-time WinUSB driver bind | <https://zadig.akeo.ie> |
>
> The Windows kit expects you to drop them beside it as `tools\thingino-dfu\thingino-dfu.exe`
> (+ its `firmware\` folder) and `tools\dfu-util.exe` — see
> [`tools/windows-flasher/README.txt`](../tools/windows-flasher/README.txt). Paths like
> `tools/dfu-util/…` elsewhere in the docs refer to **your** local layout, not to files in this
> repository. No vendor firmware image ships here either.

---

## Disassembly (in order)

1. **Remove the 2 screws under the silicone cover.** Peel back the silicone weather cover to
   expose them, then unscrew both.
2. **Extract the camera module from the plastic enclosure.** Ease it out with the plastic pry
   tool; take your time so you don't stress the ribbon or wiring.
3. **Unscrew the board and the battery.** The **18650 Li-ion (3.7 V / 4000 mAh)** is on its
   own connector — unclip it gently and set it aside. Do not puncture or short it.
4. **Remove the ESD / metal shield** to fully expose the mainboard and the flash chip.
5. **Short the two flash pins during power-on** (next section) to force the SoC into USB
   BootROM mode.

The board silkscreen reads **`VP_BW6H-DE-M_MAIN_V1`**. The SoC is an Ingenic **T23N**; WiFi is
an **AIC8800**; the flash you are about to short is the **25QH64** SPI-NOR (body marked
`25QH64DHIQ`).

---

## Enter BootROM (short pins 5 + 6, then power on)

The target is the **25QH64 SPI-NOR flash** — an **8-pin SOIC** on the mainboard. You are going
to short **pin 5 (DI)** and **pin 6 (CLK)**: the **two right-most pins on the TOP row** of the
chip.

**Finding the pins** — the board silkscreens a small **"5"** next to the top-right pin, and
pin 6 sits immediately to its left. For orientation the corners are silked too: **"1"** =
bottom-left, **"4"** = bottom-right, **"5"** = top-right. So pins 5 and 6 are the top-right
pair.

![Short pins 5 and 6 on the 25QH64 flash](img/board_pins_5_6.jpg)

*(The red circle marks pins 5 + 6. The QR code on the RF shield in this photo is intentionally
blurred for privacy — it is not part of the procedure.)*

**Technique:**

1. With the shield off and the board on a non-conductive surface, **bridge pins 5 + 6** with
   metal tweezers or a thin jumper — touching only those two pins.
2. **While holding the short**, apply USB-C power (click the battery in / plug the USB-C).
3. **Hold the short ~2 s**, then release.

Shorting the DI/CLK lines corrupts the BootROM's SPI read at power-on, so the SoC falls back
to its **USB BootROM**. The PC then enumerates a new device with **`VID_A108&PID_C309`**
(`a108:c309`). Once you see that, you're in — move to the flash.

---

## Serial / UART console — the pads

**You almost certainly want this instead of DFU.** The whole documented workflow runs over the
serial console: reading your unit's **per-unit device password**
([ARCHITECTURE.md](ARCHITECTURE.md) → *Device password*, a `dd` on `/proc/<pid>/mem` from a root
shell), running `flashcp` against `mtd4`, driving the U-Boot `=>` `sf` path, and confirming the
camera came back up. Hooking up three wires while the case is open is the cheap alternative to a
whole-chip DFU and its NVS wipe.

**The pads.** The camera board carries a silkscreened UART legend — **`GND`**, **`RX`**, **`TX`**
— with a separate **`BOOT`** pad next to it.

**Location — read this honestly.** In the photo above ([`img/board_pins_5_6.jpg`](img/board_pins_5_6.jpg))
the strings `GND`, `RX`, `TX` and `BOOT` are legible in the silkscreen on the component side of
the board, near the **25QH64 flash chip** — the same corner of the board the pin-5/6 short
targets. That photo was taken to document the flash pins, not the UART, and at that resolution
**I could not confirm which physical pad or via each label annotates.** So: don't measure it off
the picture. With the metal shield removed, look on the camera board for the silkscreen text
`GND` / `RX` / `TX` (and `BOOT`) and identify the pads it labels on your own unit, in good light,
before you touch anything. Confirm `GND` with a multimeter against a known ground (a mounting
hole / the shield frame) rather than trusting the legend alone.

**Adapter settings** (these are the values used everywhere else in this repo — see
[FLASHING.md → *Prerequisites*](FLASHING.md#prerequisites) and [TESTING.md](TESTING.md)):

- **115200 8N1**, no flow control.
- **3.3 V logic adapter.** A 5 V adapter can damage the SoC — check your adapter's jumper.
- **DTR/RTS de-asserted.** If your terminal asserts them when it opens the port it will reset or
  hold the board; the repo's PowerShell serial tools all open the port this way.
- **Do not connect the adapter's VCC/3V3 line.** Power the camera from its own USB-C or battery
  and share only ground.

**Wiring** (it is a crossover — signals are named from each end's own point of view):

| Adapter | ↔ | Camera pad |
|---|---|---|
| **RX** | ← | **TX** |
| **TX** | → | **RX** |
| **GND** | ↔ | **GND** |

If you get only silence, swap TX/RX first — that is the usual cause. Once connected, the root
login is documented in [FLASHING.md → *Prerequisites*](FLASHING.md#prerequisites). The console is
flooded by `vp_logcat` on a running camera; `kill $(pidof vp_logcat)` quiets it.

> The **`BOOT`** pad is labelled on the silkscreen but **this repo does not document what it
> does** and no procedure here uses it. The BootROM entry documented above is achieved by
> shorting flash pins 5 + 6, not by touching `BOOT`. Leave it alone.

---

## Install the USB driver (one-time per PC)

Windows needs a **WinUSB / libusb** driver bound to the BootROM device or `dfu-util` can't
claim it (you'll see `LIBUSB_ERROR_NOT_SUPPORTED` / "no DFU capable device"). Do this **once**:

1. Trigger the short + power so the device enumerates (Device Manager shows an unknown
   **`USB VID_A108 & PID_C309`** device).
2. Run **[Zadig](https://zadig.akeo.ie)** (external download — not shipped here) →
   *Options ▸ List All Devices* → select the `a108:c309` device → choose **WinUSB** →
   **Install Driver**.
3. That's it — future flashes on this PC just work. (The U-Boot DFU stage re-enumerates with the
   same `a108:c309` ID, so the one driver covers both stages.)

## Flash (see FLASHING.md for the exact commands)

Once `a108:c309` enumerates (and the driver above is installed), hand off to the PC-side flasher documented in
[FLASHING.md → *Last resort — full DFU restore*](FLASHING.md#last-resort--full-dfu-restore).
In short: `thingino-dfu` **bootstraps** the BootROM (`-b --wait --cpu t23zn`), then
**`dfu-util`** does the actual write/readback (`dfu-util -a flash -D/-U …`). Keep those exact
commands in FLASHING.md — don't re-derive them here. Both binaries are the **third-party
downloads** from *What you need* above; wherever the docs write `tools/thingino-dfu` or
`tools/dfu-util`, that means wherever **you** put them locally.

The image you write is **your own full-chip backup** (or an image you built yourself) — this
repo ships no vendor bootloader/kernel/rootfs, so there is nothing here to DFU onto a camera.

Two things to know while doing it:

- **~60 s window.** U-Boot's DFU loader auto-boots after ~60 s (bootdelay 0), so the whole
  8 MiB transfer has to finish inside that window. If it times out, just re-trigger the short
  and go again.
- **`LIBUSB_ERROR_PIPE`** mid-write is intermittent and harmless — re-trigger the short,
  bootstrap again, and retry the transfer.

---

## Reassembly (reverse order)

Once the flash is verified (see FLASHING.md), put it back together in reverse:

1. **Reseat the ESD / metal shield.**
2. **Reconnect and screw down the battery**, then the board.
3. **Slide the module back into the plastic enclosure.**
4. **Refit the 2 screws** under the silicone cover.
5. **Press the silicone weather cover back over the screws** so the unit is sealed again.

---

## What a full DFU write means for your camera (reversibility)

A routine deploy writes **only `mtd4`** and is fully revert-safe. A full USB-DFU is the
**whole-chip** path, so it also **resets `mtd5` NVS to factory** — the WiFi credentials are
wiped. On first boot after a full DFU the camera therefore:

- **re-provisions WiFi from the SD `wifi.ini`** (drop your SSID/password on the card — see
  [FEATURES.md → *wifi_sd.so — SD-card WiFi onboarding*](FEATURES.md#wifi_sdso--sd-card-wifi-onboarding)),
  and
- comes up on its **factory default password** until you read/set your unit's credentials
  ([ARCHITECTURE.md](ARCHITECTURE.md) → *Device password*).

For everything about the software flash, backups, and the golden "only `mtd4`" rule, see
[FLASHING.md](FLASHING.md).

---

## Recap — when you need this

- **Rescue:** a unit that won't boot at all and can't be recovered over serial/SD/TFTP. This is
  the case golden rule #2 allows.
- **No serial access:** you've decided not to attach a UART adapter, and are accepting the NVS
  wipe + re-onboarding that comes with a full-chip write.

**You do not need this for a stock → custom conversion.** That path is serial root shell +
`flashcp` to `mtd4` and never opens the flash chip — [FLASHING.md](FLASHING.md). If the camera
boots, flash `mtd4` the normal way and leave the case shut.
