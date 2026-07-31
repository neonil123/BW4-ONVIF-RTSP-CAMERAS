BW4 Camera Flasher  -  Windows kit
==================================

A no-install Windows GUI that does two things for someone with no Linux, no serial cable
and no command line:
  Step 1  write a Wi-Fi wifi.ini onto an SD card
  Step 2  write a FULL 8 MiB firmware image to the camera over USB-DFU

As shipped in the repository this kit CANNOT RUN: it needs three files that are not (and
never will be) part of the repo. See "FILES YOU MUST SUPPLY" below - the scripts check for
each of them before touching the camera and tell you exactly what is missing.


READ THIS FIRST  -  this kit breaks the project's normal safety rule on purpose
-------------------------------------------------------------------------------
Everywhere else in this project the rule is:

    "Only /dev/mtd4 is ever written."
    "NEVER DFU unless the camera won't boot at all."
        -- docs/FLASHING.md, golden rules #1 and #2

That rule is right, and this kit does the opposite: it writes the WHOLE 8 MiB chip. The
consequence, up front:

  * The mtd5 NVS partition is RESET TO FACTORY. The Wi-Fi credentials stored there are
    ERASED - which is precisely why Step 1 exists: the camera re-provisions from the SD
    card's wifi.ini on its next boot.
  * The camera comes back on its factory default settings, and you must re-read / re-set
    its per-unit device password before you can build a working image for it
    (docs/ARCHITECTURE.md -> "Device password").
  * mtd0 (boot), mtd2 (kernel), mtd3 (rootfs) and the appfs are overwritten too, so the
    image you write is the entire firmware - not an overlay.

Both paths are legitimate; they are for different situations:

  Normal path  ->  docs/FLASHING.md
      Camera boots and you can attach a 3.3 V USB-serial adapter: back up mtd4, then
      flashcp your image to /dev/mtd4. Nothing else is touched, Wi-Fi creds survive, and
      your own mtd4 backup is a guaranteed revert. THIS IS THE DEFAULT. If you can do it,
      close this kit and do that instead.

  This kit    ->  docs/HARDWARE_DFU.md
      (a) the unit will not boot at all and cannot be reached over serial/SD/TFTP, or
      (b) first conversion by an owner who has no serial console (every non-DFU write needs
          a UART). You are choosing to pay the NVS wipe + re-onboarding.

Prerequisite for (a)/(b) alike: opening the camera and shorting two flash pins during
power-on. Read docs/HARDWARE_DFU.md (disassembly, ESD/Li-ion safety, pin photo) first.


BEFORE YOU GIVE A FLASHED IMAGE - OR THIS KIT - TO ANYONE ELSE
---------------------------------------------------------------
A full-chip image is a byte copy of one specific camera's flash, INCLUDING that unit's
mtd5 NVS. If you dumped it off your own camera, it carries that camera's identity.

  Fine  : flashing that image onto YOUR OWN cameras.
  Not OK: handing it to someone else as-is. Every unit you flash with it ends up claiming
          the same device identity, and you have handed over your own unit's secrets.

"Sanitized" means, concretely, an image whose mtd5 NVS carries:
  * NO per-unit device id (vuid) and NO per-unit device password / key - the fields are
    blank or factory-default, not your unit's values;
  * NO Wi-Fi credentials (SSID / PSK) and no saved cloud/account binding;
so that the camera re-provisions from scratch on first boot: it picks up Wi-Fi from the SD
card's wifi.ini, and its device password is read/set per unit afterwards.

The author's development image is NOT sanitized - it carries cam1's identity. Do not
redistribute it. And do not commit any full-chip image to the repository: it contains the
vendor U-Boot/kernel/rootfs (NOTICE.md: no vendor firmware is redistributed) as well as
those per-unit secrets. The repo's .gitignore blocks *full_backup*.bin and *stock*.bin,
which is why this kit's default image name is bw4_full_backup.bin.


FILES YOU MUST SUPPLY  (none of them are in the repo)
------------------------------------------------------
Shipped in this folder (the kit itself):
  BW4-Flasher.ps1        the GUI
  flash-console.ps1      the flasher, runs in its own console window
  kit-preflight.ps1      the shared "is everything here?" checks
  Run-BW4-Flasher.bat    launcher (double-click this)
  README.txt             this file

You add these three, in exactly these locations:

  tools\thingino-dfu\thingino-dfu.exe   + its firmware\ folder
      Role   : bootstraps the SoC USB BootROM (step 1 of the flash; -b only)
      Get it : https://github.com/themactep/thingino-firmware
      Note   : copy the whole upstream folder - the .exe alone may not carry the payloads
               it uploads to the BootROM.

  tools\dfu-util.exe
      Role   : performs the actual transfer (dfu-util -a flash -D / -U)
      Get it : https://dfu-util.sourceforge.net
      Note   : thingino-dfu's own -w/-r do NOT work on this camera - U-Boot's DFU gadget
               re-enumerates with the same a108:c309 ID and it mis-detects it.

  firmware\bw4_full_backup.bin      (8,388,608 bytes exactly)
      Role   : the firmware that gets written
      Get it : nowhere - you make it yourself. From a camera that still boots:
                   thingino-dfu.exe -b --wait --cpu t23zn
                   dfu-util.exe -a flash -U bw4_full_backup.bin
               Keep it off the repo and off the internet (see the section above).
      Note   : firmware/mtd4_integrated.bin in the repo is NOT this file. That is the
               393,216-byte mtd4 overlay, flashed with flashcp on the camera - flashing it
               over DFU would write /system on top of U-Boot and kill the unit. The kit
               refuses any image that is not exactly 8,388,608 bytes.
      Also accepted for backward compatibility: firmware\bw4_full_image.bin (but that name
      is not covered by the repo's .gitignore - prefer bw4_full_backup.bin).

Optional: put an MD5 sidecar next to the image (bw4_full_backup.bin.md5, same
"<hex>  <filename>" format as firmware/mtd4_integrated.bin.md5) and the flasher verifies
the image before writing. Or pass -Md5 <hex> to flash-console.ps1.

One-time per PC, not a file: the WinUSB driver bind. Trigger the pin short + power so
Windows shows an unknown "USB VID_A108 & PID_C309" device, run Zadig
(https://zadig.akeo.ie) -> Options / List All Devices -> pick a108:c309 -> WinUSB ->
Install Driver. Without it dfu-util cannot claim the device.

All three downloads are third-party projects with their own licences; nothing in this kit
downloads anything for you.


HOW TO RUN
----------
  Double-click  Run-BW4-Flasher.bat
  (If Windows SmartScreen warns: More info -> Run anyway. Nothing is installed.)

  The GUI reports at startup whether the flashing tools and the 8 MiB image were found.
  "Check required files" prints exactly what is missing, where to download it and where to
  put it. flash-console.ps1 repeats the same check and refuses to start otherwise, so you
  never discover a missing file with the camera already sitting in BootROM.


STEP 1  -  the Wi-Fi SD card
-----------------------------
Pick the SD drive, type your 2.4 GHz Wi-Fi name and password, click "Write to SD card".
It writes wifi.ini to the ROOT of the card:

    SSID=YourNetworkName
    PASSWORD=YourWiFiPassword

That is what the on-camera shim wifi_sd.so reads from /mnt/sda0/wifi.ini on boot
(src/shims/wifi_sd.c; docs/FEATURES.md -> "wifi_sd.so - SD-card WiFi onboarding"). Limits
the GUI enforces because the shim's parser does: SSID 1-32 characters, password empty (open
network) or 8-63 characters, no leading/trailing spaces (the parser trims them), no BOM.
A file that fails those checks is a silent no-op on the camera - it simply never joins - so
the GUI blocks it here instead. Use a FAT/FAT32 card; the camera does not mount NTFS/exFAT.

When the camera joins, the shim renames the file to wifi.ini.applied. That is your success
marker (check it in a card reader if the camera never appears).

SECURITY: your Wi-Fi password sits on the card in PLAINTEXT. Once the camera is online, put
the card back in a PC and DELETE wifi.ini / wifi.ini.applied (or wipe the card). The camera
does not need the file again.

Finding the camera afterwards: look at your router's DHCP client list, or let your NVR find
it by ONVIF / WS-Discovery. The camera does NOT write its IP address anywhere on the SD
card. Give it 1-2 minutes after power-on - the shim waits for the radio and retries.


STEP 2  -  flash the camera (USB-DFU)
--------------------------------------
Hardware prep, from docs/HARDWARE_DFU.md: power off and unplug, ESD precautions, open the
camera, remove the metal shield, then bridge flash pins 5 (DI) + 6 (CLK) on the 25QH64
SPI-NOR while applying USB-C power, hold ~2 s, release. Windows enumerates a108:c309.

Click "Flash camera now". The GUI shows the full-chip / NVS warning, the console window
re-states it and asks you to type YES, then:
  step 1/2  thingino-dfu -b --wait --cpu t23zn      (waits for the BootROM)
  step 2/2  dfu-util -a flash -D <image>            (tight retry loop)

The whole 8 MiB must land inside U-Boot's ~60 s DFU auto-boot window, so the flasher fires
dfu-util in a ~120 ms retry loop rather than polling - a connection that lands early
completes in ~24 s.

After it finishes: reassemble, insert the SD card from Step 1, power on. The camera joins
Wi-Fi, then serves rtsp://<cam-ip>:554/live and ONVIF on port 80.


RECOVERY / TROUBLESHOOTING
--------------------------
  Flash failed or timed out
      Harmless. Re-short pins 5 + 6, re-plug USB-C, click Flash again. The BootROM is
      always reachable this way - that is what makes DFU the rescue channel.
  LIBUSB_ERROR_PIPE mid-write
      Intermittent and known. Re-trigger and retry.
  "no DFU capable device" / LIBUSB_ERROR_NOT_SUPPORTED
      The WinUSB driver bind is missing - run Zadig (see above).
  Camera flashed but never joins Wi-Fi
      Check the card in a reader: if wifi.ini was NOT renamed to wifi.ini.applied the file
      was never accepted (wrong card format, wrong SSID/password length, 5 GHz network).
  Camera boots but you want to go back
      A full-chip DFU has no automatic revert - restore your own full-chip backup the same
      way (dfu-util -a flash -D <your backup>). This is another reason the mtd4 path in
      docs/FLASHING.md is the preferred one: there your own mtd4 backup is the revert.

Verify a write: re-trigger the short, bootstrap again, dfu-util -a flash -U readback.bin,
and compare hashes with the image you flashed.


No cloud. Local RTSP (rtsp://<cam-ip>:554/live) + ONVIF only.
Right-to-repair / authorized-use only - your own hardware. See NOTICE.md.
