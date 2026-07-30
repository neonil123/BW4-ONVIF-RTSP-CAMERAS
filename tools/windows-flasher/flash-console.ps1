# flash-console.ps1 -- runs in its own console window (launched by the GUI).
# Bootstraps the SoC USB BootROM then writes the full image via dfu-util, with the
# tight-retry loop the camera's ~60s DFU auto-boot window needs. Proven logic
# (see scratchpad/flash_full_dfu.ps1). Paths resolve relative to this kit folder,
# falling back to the dev tree so it works in-place during development.
param(
  [string]$Image = "",
  [string]$Md5   = ""
)
$ErrorActionPreference = 'Continue'
$Base = $PSScriptRoot

function Resolve-First($candidates){ foreach($c in $candidates){ if($c -and (Test-Path $c)){ return $c } } return $null }

$dfu = Resolve-First @(
  "$Base\tools\thingino-dfu\thingino-dfu.exe",
  "$Base\tools\thingino-dfu.exe"
)
$du = Resolve-First @(
  "$Base\tools\dfu-util.exe",
  "$Base\tools\dfu-util\dfu-util.exe"
)
if(-not $Image){ $Image = "$Base\firmware\bw4_full_image.bin" }

Write-Host "=== BW4 camera flasher ===" -ForegroundColor Cyan
Write-Host "tool (bootstrap): $dfu"
Write-Host "tool (transfer) : $du"
Write-Host "image           : $Image"
if(-not $dfu -or -not $du){ Write-Host "ERROR: flashing tools not found next to this app." -ForegroundColor Red; Read-Host "Press Enter to close"; exit 1 }
if(-not $Image -or -not (Test-Path $Image)){ Write-Host "ERROR: firmware image not found." -ForegroundColor Red; Read-Host "Press Enter to close"; exit 1 }

if($Md5){
  $m = (Get-FileHash $Image -Algorithm MD5).Hash.ToLower()
  Write-Host "image md5=$m expected=$Md5 $(if($m -eq $Md5){'OK'}else{'MISMATCH'})" -ForegroundColor $(if($m -eq $Md5){'Green'}else{'Red'})
  if($m -ne $Md5){ Read-Host "Press Enter to close"; exit 2 }
}

Write-Host ""
Write-Host "STEP 1/2  Waiting for the camera in update mode..." -ForegroundColor Yellow
Write-Host "  With the camera OPEN and pins 5+6 shorted, plug in USB-C now (see the disassembly guide)."
& $dfu -b --wait --cpu t23zn
Write-Host "bootstrap exit: $LASTEXITCODE"

Write-Host ""
Write-Host "STEP 2/2  Writing firmware (must finish within ~60s)..." -ForegroundColor Yellow
$connected = $false
for($i=1; $i -le 400; $i++){
  $r = & $du -a flash -D $Image 2>&1 | Out-String
  if($r -match 'Download.*100%' -or $r -match 'Download done' -or $r -match 'Claiming USB DFU'){
    Write-Host $r
    if($r -match 'Download done' -or $r -match '100%'){ $connected = $true; break }
  }
  Start-Sleep -Milliseconds 120
}
Write-Host ""
if($connected){
  Write-Host "SUCCESS - firmware written. The camera will reboot on its own." -ForegroundColor Green
  Write-Host "Now: reassemble, insert the WiFi SD card, and power on. It joins WiFi and writes its"
  Write-Host "IP to the SD card (cam_ip.txt). Give it ~1-2 minutes."
} else {
  Write-Host "Did not complete in time. Re-short pins 5+6, re-plug USB-C, and run Flash again." -ForegroundColor Red
}
Read-Host "Press Enter to close"
