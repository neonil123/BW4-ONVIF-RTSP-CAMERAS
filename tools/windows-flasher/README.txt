BW4 Camera Flasher  -  Windows kit
==================================

A no-install Windows app to convert a BW4 camera to local RTSP/ONVIF firmware.
Two steps: (1) make a WiFi SD card, (2) flash the camera over USB.

HOW TO RUN
  Double-click  Run-BW4-Flasher.bat
  (If Windows SmartScreen warns: More info -> Run anyway. Nothing is installed.)

WHAT IT DOES
  Step 1  Create the WiFi SD card
     Pick the SD drive, type your 2.4 GHz WiFi name + password, click "Write to SD".
     It writes a small wifi.ini file the camera reads on first boot to join WiFi
     with no phone app.

  Step 2  Flash the camera (USB)
     ONE-TIME per PC: install the USB driver first, or flashing can't see the camera.
     Trigger the short + power so Windows shows a "USB VID_A108 & PID_C309" device,
     run Zadig (https://zadig.akeo.ie), List All Devices, pick a108:c309, install WinUSB.
     Then:
     This needs the one-time hardware step (see the guide): open the camera,
     remove the metal shield, and bridge the two flash pins (5 + 6) while plugging
     in USB-C. Click "Flash camera now" and follow the console window.
     After it finishes: reassemble, insert the SD card, power on. The camera joins
     WiFi, and writes its IP address to a text file on the SD card so you can find it.

REQUIRED KIT LAYOUT (place these next to the app before distributing)
  BW4-Flasher.ps1        <- the GUI (included)
  flash-console.ps1      <- the flasher (included)
  Run-BW4-Flasher.bat    <- launcher (included)
  tools\thingino-dfu\thingino-dfu.exe   (+ its firmware\ folder)
  tools\dfu-util.exe
  firmware\bw4_full_image.bin           <- the 8 MB image to flash

  Get the DFU tools (dfu-util, thingino-dfu) from their upstream projects and
  place them under tools\ as shown. The firmware image is NOT included here (it
  contains vendor bootloader/kernel/rootfs) -- supply your own per the notes below.

IMPORTANT BEFORE YOU SHARE THIS KIT WITH OTHER PEOPLE
  The full 8 MB image (firmware\bw4_full_image.bin) resets the camera's NVS to
  factory. Use an image whose NVS has NO per-unit identity baked in (the device
  ID / key must be blanked). The current dev image carries cam1's identity and is
  fine for your OWN cameras but must be sanitized before handing to others.

RECOVERY
  If a flash fails or the camera won't boot, just repeat Step 2 (re-short pins,
  re-plug USB, click Flash again). The BootROM is always reachable this way.

No cloud. Local RTSP (rtsp://<cam-ip>:554/live) + ONVIF only.
