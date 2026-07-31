# flash-console.ps1 -- runs in its own console window (launched by the GUI, or by hand).
#
# Bootstraps the SoC USB BootROM with thingino-dfu, then writes a FULL 8 MiB whole-chip
# image with dfu-util, using the tight retry loop the camera's ~60 s DFU auto-boot window
# needs (see docs/FLASHING.md -> "Last resort -- full DFU restore").
#
# READ THIS FIRST: this is a WHOLE-CHIP write. It resets the mtd5 NVS partition, so the
# camera's WiFi credentials are erased and it returns to factory settings. The BW4 project's
# normal path never does that -- it flashcp's only /dev/mtd4 from a serial root shell and
# keeps mtd0/2/3/5 stock (docs/FLASHING.md, golden rules #1 and #2). Use this kit only when
# there is no serial console, or when the camera no longer boots.
#
# The three files this needs (both DFU tools and the 8 MiB image) are NOT in the repo. They
# are checked for up front -- see kit-preflight.ps1 and README.txt.
param(
  [string]$Image = "",
  [string]$Md5   = "",
  [switch]$Yes           # skip the interactive confirmation (the GUI does NOT pass this)
)
$ErrorActionPreference = 'Continue'
$Base = $PSScriptRoot

$preflight = Join-Path $Base 'kit-preflight.ps1'
if (-not (Test-Path -LiteralPath $preflight)) {
  Write-Host "ERROR: kit-preflight.ps1 is missing from $Base" -ForegroundColor Red
  Write-Host "       It ships with this kit (tools/windows-flasher/). Re-copy the whole folder."
  Read-Host "Press Enter to close"
  exit 1
}
. $preflight

Write-Host "=== BW4 camera flasher (full-chip USB-DFU) ===" -ForegroundColor Cyan
Write-Host ""

# ---------------------------------------------------------------- pre-flight -------
$rep = Get-BW4Readiness -Base $Base -Image $Image

foreach ($r in $rep.Requirements) {
  $state = 'MISSING'
  $col   = 'Red'
  if ($r.Found) { $state = $r.Found; $col = 'Gray' }
  elseif ($r.Severity -eq 'advisory') { $state = 'not found (advisory)'; $col = 'Yellow' }
  Write-Host ("  {0,-42} {1}" -f $r.Name, $state) -ForegroundColor $col
}
Write-Host ""

if (-not $rep.Ready) {
  Write-Host "This kit is not complete -- nothing has been written to the camera." -ForegroundColor Red
  Write-Host ""
  Write-Host $rep.Text
  Write-Host ""
  Write-Host "See README.txt in this folder for the full expected layout, and" -ForegroundColor Yellow
  Write-Host "docs/HARDWARE_DFU.md in the BW4 repo for the tool table and the Zadig driver step." -ForegroundColor Yellow
  Read-Host "Press Enter to close"
  exit 1
}
if ($rep.Advisories.Count -gt 0) {
  Write-Host $rep.Text -ForegroundColor Yellow
  Write-Host ""
}

# resolve the image the readiness check settled on
$imgReq = $rep.Requirements | Where-Object { $_.Name -like 'bw4_full_backup.bin*' }
$Image  = $imgReq.Found
$dfu    = ($rep.Requirements | Where-Object { $_.Name -eq 'thingino-dfu.exe' }).Found
$du     = ($rep.Requirements | Where-Object { $_.Name -eq 'dfu-util.exe' }).Found

# ---------------------------------------------------------------- integrity --------
# Use an <image>.md5 sidecar if present and no -Md5 was passed (same convention as
# firmware/mtd4_integrated.bin.md5 in the repo: "<hex>  <filename>").
if (-not $Md5) {
  $sidecar = "$Image.md5"
  if (Test-Path -LiteralPath $sidecar) {
    $raw = (Get-Content -LiteralPath $sidecar -TotalCount 1)
    if ($raw) { $Md5 = ($raw.Trim() -split '\s+')[0] }
    if ($Md5) { Write-Host "using md5 from $sidecar" }
  }
}
if ($Md5) {
  $m = (Get-FileHash -LiteralPath $Image -Algorithm MD5).Hash.ToLower()
  $Md5 = $Md5.ToLower()
  if ($m -eq $Md5) {
    Write-Host "image md5 $m OK" -ForegroundColor Green
  } else {
    Write-Host "image md5 MISMATCH" -ForegroundColor Red
    Write-Host "  file    : $m"
    Write-Host "  expected: $Md5"
    Write-Host "  Flashing a corrupt image over the whole chip is how cameras die. Aborting."
    Read-Host "Press Enter to close"
    exit 2
  }
}

# ---------------------------------------------------------------- consent ----------
Write-Host ""
Write-Host $BW4_NVS_WARNING -ForegroundColor Yellow
Write-Host ""
Write-Host ("image : {0}" -f $Image)
Write-Host ("size  : {0:N0} bytes" -f (Get-Item -LiteralPath $Image).Length)
Write-Host ""
Write-Host "If this image was dumped from one of YOUR cameras it also carries that unit's" -ForegroundColor Yellow
Write-Host "identity (device id / key in mtd5 NVS). Fine on your own cameras; do not hand it" -ForegroundColor Yellow
Write-Host "to anyone else un-sanitized -- see README.txt." -ForegroundColor Yellow
Write-Host ""
if (-not $Yes) {
  $ans = Read-Host "Type YES to write the full chip (anything else cancels)"
  if ($ans -ne 'YES') { Write-Host "Cancelled. Nothing was written."; exit 0 }
}

# ---------------------------------------------------------------- flash ------------
Write-Host ""
Write-Host "STEP 1/2  Waiting for the camera in update mode..." -ForegroundColor Yellow
Write-Host "  With the camera OPEN and flash pins 5 + 6 shorted, plug in USB-C now."
Write-Host "  (Disassembly + pin photo: docs/HARDWARE_DFU.md in the BW4 repo.)"
Write-Host "  If nothing happens: the one-time WinUSB driver bind may be missing -- run Zadig"
Write-Host "  on the a108:c309 device, see README.txt."
& $dfu -b --wait --cpu t23zn
Write-Host "bootstrap exit: $LASTEXITCODE"

Write-Host ""
Write-Host "STEP 2/2  Writing firmware (must finish inside the ~60 s DFU window)..." -ForegroundColor Yellow
$connected = $false
for ($i = 1; $i -le 400; $i++) {
  $r = & $du -a flash -D $Image 2>&1 | Out-String
  if ($r -match 'Download.*100%' -or $r -match 'Download done' -or $r -match 'Claiming USB DFU') {
    Write-Host $r
    if ($r -match 'Download done' -or $r -match '100%') { $connected = $true; break }
  }
  Start-Sleep -Milliseconds 120
}

Write-Host ""
if ($connected) {
  Write-Host "SUCCESS - firmware written. The camera will reboot on its own." -ForegroundColor Green
  Write-Host ""
  Write-Host "Next:"
  Write-Host "  1. Reassemble, insert the SD card written in Step 1, power on."
  Write-Host "  2. The NVS was reset, so the camera has no WiFi creds -- it re-provisions from"
  Write-Host "     wifi.ini on the card. Allow ~1-2 minutes (the shim waits for the radio, then"
  Write-Host "     retries for up to ~2 min)."
  Write-Host "  3. Find its IP on your router's DHCP client list, or by ONVIF/WS-Discovery from"
  Write-Host "     your NVR. (The camera does NOT write its IP to the SD card.)"
  Write-Host "  4. On success the camera renames wifi.ini to wifi.ini.applied. Put the card back"
  Write-Host "     in your PC and DELETE it -- your WiFi password is on it in plaintext."
  Write-Host "  5. Stream: rtsp://<cam-ip>:554/live   ONVIF: http://<cam-ip>:80/onvif/..."
} else {
  Write-Host "Did not complete in time." -ForegroundColor Red
  Write-Host "  The U-Boot DFU loader auto-boots after ~60 s, so a slow start loses the window."
  Write-Host "  Re-short pins 5 + 6, re-plug USB-C, and run Flash again -- the BootROM is always"
  Write-Host "  reachable this way, so a failed attempt is not fatal."
  Write-Host "  A LIBUSB_ERROR_PIPE mid-write is intermittent and harmless: just retry."
}
Read-Host "Press Enter to close"
