# kit-preflight.ps1 -- shared "is this kit complete?" checks.
#
# Dot-sourced by BW4-Flasher.ps1 (the GUI) and flash-console.ps1 (the flasher) so both give
# the SAME diagnosis, up front, naming the exact file / where to get it / where to put it --
# instead of failing obscurely once the camera is already sitting in BootROM.
#
# Three things this kit needs are deliberately NOT in the BW4 repo and never will be:
#
#   1. tools\thingino-dfu\thingino-dfu.exe  (+ its firmware\ folder)  third-party, upstream
#   2. tools\dfu-util.exe                                            third-party, upstream
#   3. firmware\bw4_full_backup.bin  -- an 8 MiB WHOLE-CHIP image. It contains the vendor
#      U-Boot, kernel and rootfs (the repo redistributes no vendor firmware -- see
#      NOTICE.md) and, if it came off a camera, that unit's mtd5 NVS: WiFi credentials and
#      the per-unit device password. It must never be committed. Note that the repo's
#      .gitignore covers *full_backup*.bin / *stock*.bin -- which is exactly why this kit
#      prefers the name bw4_full_backup.bin.
#
# You supply all three locally. Nothing in this kit downloads anything.

$BW4_FULL_IMAGE_BYTES = 8388608    # 8 MiB = the whole 25QH64 SPI-NOR
$BW4_MTD4_BYTES       = 393216     # 0x60000 = the mtd4 overlay (NOT a DFU image)

$BW4_URL_DFUUTIL = 'https://dfu-util.sourceforge.net'
$BW4_URL_THINGINO = 'https://github.com/themactep/thingino-firmware'
$BW4_URL_ZADIG   = 'https://zadig.akeo.ie'

function Get-BW4FirstFile {
  param([string[]]$Candidates)
  foreach ($c in $Candidates) {
    if ($c -and (Test-Path -LiteralPath $c -PathType Leaf)) { return $c }
  }
  return $null
}

function Get-BW4FirstDir {
  param([string[]]$Candidates)
  foreach ($c in $Candidates) {
    if ($c -and (Test-Path -LiteralPath $c -PathType Container)) { return $c }
  }
  return $null
}

# Returns one object per required item:
#   Name, Role, Candidates[], Found (path or $null), Source, Place, Severity ('required'|'advisory')
function Get-BW4Requirements {
  param(
    [Parameter(Mandatory=$true)][string]$Base,
    [string]$Image = ''
  )

  $reqs = @()

  # --- 1. thingino-dfu (BootROM bootstrap only: -b) ---------------------------------
  $dfuCand = @(
    (Join-Path $Base 'tools\thingino-dfu\thingino-dfu.exe'),
    (Join-Path $Base 'tools\thingino-dfu.exe')
  )
  $dfu = Get-BW4FirstFile $dfuCand
  $reqs += [pscustomobject]@{
    Name       = 'thingino-dfu.exe'
    Role       = 'bootstraps the SoC USB BootROM (step 1 of 2)'
    Candidates = $dfuCand
    Found      = $dfu
    Source     = "$BW4_URL_THINGINO  (third-party project -- not shipped in this repo)"
    Place      = (Join-Path $Base 'tools\thingino-dfu\thingino-dfu.exe')
    Severity   = 'required'
  }

  # --- 2. thingino-dfu's firmware\ payload folder ------------------------------------
  $fwCand = @()
  if ($dfu) { $fwCand += (Join-Path (Split-Path -Parent $dfu) 'firmware') }
  $fwCand += (Join-Path $Base 'tools\thingino-dfu\firmware')
  $fwDir = Get-BW4FirstDir $fwCand
  $reqs += [pscustomobject]@{
    Name       = 'thingino-dfu firmware\ folder'
    Role       = 'the stage-1/stage-2 payloads thingino-dfu -b uploads to the BootROM'
    Candidates = $fwCand
    Found      = $fwDir
    Source     = "ships alongside thingino-dfu.exe -- copy the WHOLE upstream folder, not just the .exe ($BW4_URL_THINGINO)"
    Place      = (Join-Path $Base 'tools\thingino-dfu\firmware\')
    Severity   = 'advisory'   # bootstrap may still work if the build embeds its payloads
  }

  # --- 3. dfu-util (all transfers) ---------------------------------------------------
  $duCand = @(
    (Join-Path $Base 'tools\dfu-util.exe'),
    (Join-Path $Base 'tools\dfu-util\dfu-util.exe')
  )
  $du = Get-BW4FirstFile $duCand
  $reqs += [pscustomobject]@{
    Name       = 'dfu-util.exe'
    Role       = 'writes the image over USB-DFU (step 2 of 2)'
    Candidates = $duCand
    Found      = $du
    Source     = "$BW4_URL_DFUUTIL  (third-party project -- not shipped in this repo)"
    Place      = (Join-Path $Base 'tools\dfu-util.exe')
    Severity   = 'required'
  }

  # --- 4. the 8 MiB full-chip image --------------------------------------------------
  if ($Image) {
    $imgCand = @($Image)
  } else {
    $imgCand = @(
      (Join-Path $Base 'firmware\bw4_full_backup.bin'),
      (Join-Path $Base 'firmware\bw4_full_image.bin')
    )
  }
  $img = Get-BW4FirstFile $imgCand
  $reqs += [pscustomobject]@{
    Name       = 'bw4_full_backup.bin  (8 MiB whole-chip image)'
    Role       = 'the firmware actually written to the camera'
    Candidates = $imgCand
    Found      = $img
    Source     = @"
NOT in this repo, and it never can be: a full-chip image carries the vendor U-Boot,
               kernel and rootfs (NOTICE.md -- no vendor firmware is redistributed) plus the
               mtd5 NVS of whatever unit it was dumped from (WiFi creds + per-unit device
               password). You must supply your own. To dump one from a camera that boots:
                   thingino-dfu.exe -b --wait --cpu t23zn
                   dfu-util.exe -a flash -U bw4_full_backup.bin
               firmware\mtd4_integrated.bin in the repo is NOT this file -- that is the
               393,216-byte mtd4 overlay, flashed with flashcp (see docs/FLASHING.md)
"@
    Place      = (Join-Path $Base 'firmware\bw4_full_backup.bin')
    Severity   = 'required'
  }

  return $reqs
}

# Human-readable block for one unmet requirement.
function Format-BW4Problem {
  param([Parameter(Mandatory=$true)]$Req)
  $tag = 'MISSING'
  if ($Req.Severity -eq 'advisory') { $tag = 'NOT FOUND (may still work)' }
  $lines = @()
  $lines += ("{0}: {1}" -f $tag, $Req.Name)
  $lines += ("    what for  : {0}" -f $Req.Role)
  $first = $true
  foreach ($c in $Req.Candidates) {
    if ($first) { $lines += ("    looked for: {0}" -f $c); $first = $false }
    else        { $lines += ("                {0}" -f $c) }
  }
  $lines += ("    get it    : {0}" -f $Req.Source)
  $lines += ("    put it at : {0}" -f $Req.Place)
  return ($lines -join "`r`n")
}

# Validates the image FILE ITSELF. Returns $null when fine, else an explanatory string.
# This gate exists because dfu-util -a flash -D writes from offset 0: handing it the
# 384 KiB mtd4 overlay would overwrite U-Boot with /system and leave a dead camera.
function Test-BW4ImageSize {
  param([Parameter(Mandatory=$true)][string]$Path)
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "image not found: $Path" }
  $len = (Get-Item -LiteralPath $Path).Length
  if ($len -eq $BW4_FULL_IMAGE_BYTES) { return $null }
  $msg = @()
  $msg += "WRONG IMAGE SIZE -- refusing to flash."
  $msg += "  file  : $Path"
  $msg += ("  size  : {0:N0} bytes" -f $len)
  $msg += ("  needed: {0:N0} bytes (8 MiB -- the whole SPI-NOR chip)" -f $BW4_FULL_IMAGE_BYTES)
  if ($len -eq $BW4_MTD4_BYTES) {
    $msg += ""
    $msg += "  That is the size of the mtd4 overlay (firmware/mtd4_integrated.bin)."
    $msg += "  mtd4 images are NOT flashed over DFU -- dfu-util writes from offset 0, so this"
    $msg += "  would overwrite U-Boot with /system and brick the camera. Flash mtd4 with"
    $msg += "  flashcp on the camera instead: see docs/FLASHING.md."
  }
  return ($msg -join "`r`n")
}

# One-shot summary. Returns an object: Ready (bool), Missing[], Advisories[], Text
function Get-BW4Readiness {
  param(
    [Parameter(Mandatory=$true)][string]$Base,
    [string]$Image = ''
  )
  $reqs      = Get-BW4Requirements -Base $Base -Image $Image
  $missing   = @($reqs | Where-Object { -not $_.Found -and $_.Severity -eq 'required' })
  $advisory  = @($reqs | Where-Object { -not $_.Found -and $_.Severity -eq 'advisory' })
  $blocks    = @()
  foreach ($m in $missing)  { $blocks += (Format-BW4Problem $m) }
  foreach ($a in $advisory) { $blocks += (Format-BW4Problem $a) }

  # size gate only if we actually have an image
  $imgReq = $reqs | Where-Object { $_.Name -like 'bw4_full_backup.bin*' }
  $sizeProblem = $null
  if ($imgReq.Found) {
    $sizeProblem = Test-BW4ImageSize -Path $imgReq.Found
    if ($sizeProblem) { $blocks += $sizeProblem }
  }

  return [pscustomobject]@{
    Requirements = $reqs
    Missing      = $missing
    Advisories   = $advisory
    SizeProblem  = $sizeProblem
    Ready        = (($missing.Count -eq 0) -and (-not $sizeProblem))
    Text         = ($blocks -join "`r`n`r`n")
  }
}

# The one warning that must never be buried: what a full-chip DFU costs you.
$BW4_NVS_WARNING = @"
THIS IS A FULL 8 MiB WHOLE-CHIP WRITE -- IT RESETS THE CAMERA'S NVS (mtd5).

  * The WiFi credentials stored in mtd5 are ERASED. The camera re-provisions from the
    wifi.ini on the SD card on its next boot (that is what Step 1 writes).
  * The unit comes back on its FACTORY default settings until you re-read/re-set its
    per-unit device password.

The rest of the BW4 project deliberately does NOT do this. Its golden rule is
"only /dev/mtd4 is ever written" and "NEVER DFU unless the camera won't boot at all",
because an mtd4 flashcp keeps mtd0/2/3/5 stock and is fully revert-safe.

This kit is the OTHER path, for one specific case: a first conversion by someone with no
serial console (every non-DFU write needs a UART), or a unit that no longer boots. If you
can get a serial root shell, close this and use docs/FLASHING.md instead.
"@
