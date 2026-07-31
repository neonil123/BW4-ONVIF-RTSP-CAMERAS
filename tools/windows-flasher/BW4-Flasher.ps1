# BW4-Flasher.ps1 -- simple Windows GUI for non-technical users.
#  Step 1: write a wifi.ini onto an SD card (consumed on first boot by wifi_sd.so).
#  Step 2: flash the camera over USB-DFU (launches flash-console.ps1 in its own window).
#
# Step 2 writes a FULL 8 MiB whole-chip image. That resets the mtd5 NVS partition, so the
# camera's WiFi credentials are erased -- which is exactly why Step 1 exists. The BW4
# project's normal deploy never does this (it writes only mtd4 with flashcp from a serial
# root shell -- docs/FLASHING.md). This kit is the no-serial / won't-boot path.
#
# Launch via Run-BW4-Flasher.bat (needs -STA). No install required.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$Base = $PSScriptRoot

# Shared missing-file diagnosis, used by both this GUI and flash-console.ps1.
$PreflightPath = Join-Path $Base 'kit-preflight.ps1'
$HavePreflight = Test-Path -LiteralPath $PreflightPath
if ($HavePreflight) { . $PreflightPath }

# wifi_sd.so limits -- these MUST match src/shims/parse_ini.h in the BW4 repo.
$WSD_SSID_MAX = 32     # IEEE 802.11 SSID max
$WSD_PWD_MIN  = 8      # WPA/WPA2-PSK min
$WSD_PWD_MAX  = 63     # WPA/WPA2-PSK max ASCII passphrase

function Get-RemovableDrives {
  try {
    Get-CimInstance Win32_LogicalDisk -Filter "DriveType=2" -ErrorAction Stop |
      ForEach-Object {
        $fs = $_.FileSystem
        if (-not $fs) { $fs = "?" }
        "{0}  {1} [{2}]" -f $_.DeviceID, $_.VolumeName, $fs
      }
  } catch { @() }
}

$form = New-Object System.Windows.Forms.Form
$form.Text = "BW4 Camera Flasher"
$form.Size = New-Object System.Drawing.Size(580,740)
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedSingle"
$form.MaximizeBox = $false

$title = New-Object System.Windows.Forms.Label
$title.Text = "BW4 Camera Setup"
$title.Font = New-Object System.Drawing.Font("Segoe UI",16,[System.Drawing.FontStyle]::Bold)
$title.Location = New-Object System.Drawing.Point(20,12)
$title.Size = New-Object System.Drawing.Size(500,30)
$form.Controls.Add($title)

$banner = New-Object System.Windows.Forms.Label
$banner.Text = "Step 2 is a FULL-CHIP USB flash: it resets the camera's NVS, so its saved Wi-Fi" +
               " credentials are ERASED (that is what the SD card in Step 1 is for) and the unit" +
               " returns to factory settings. The project's normal update writes only mtd4 and" +
               " keeps everything else stock -- if your camera still boots and you can attach a" +
               " serial console, use docs/FLASHING.md instead of this kit."
$banner.Location = New-Object System.Drawing.Point(20,46)
$banner.Size = New-Object System.Drawing.Size(520,72)
$banner.ForeColor = [System.Drawing.Color]::Firebrick
$form.Controls.Add($banner)

# ---------- Step 1: WiFi SD ----------
$g1 = New-Object System.Windows.Forms.GroupBox
$g1.Text = "Step 1  -  Create the Wi-Fi SD card"
$g1.Location = New-Object System.Drawing.Point(20,124)
$g1.Size = New-Object System.Drawing.Size(520,250)
$form.Controls.Add($g1)

$lblDrive = New-Object System.Windows.Forms.Label
$lblDrive.Text = "SD card drive:"; $lblDrive.Location = New-Object System.Drawing.Point(20,32); $lblDrive.Size = New-Object System.Drawing.Size(110,22)
$g1.Controls.Add($lblDrive)
$cbDrive = New-Object System.Windows.Forms.ComboBox
$cbDrive.Location = New-Object System.Drawing.Point(135,29); $cbDrive.Size = New-Object System.Drawing.Size(255,24); $cbDrive.DropDownStyle = "DropDownList"
$g1.Controls.Add($cbDrive)
$btnRefresh = New-Object System.Windows.Forms.Button
$btnRefresh.Text = "Refresh"; $btnRefresh.Location = New-Object System.Drawing.Point(400,28); $btnRefresh.Size = New-Object System.Drawing.Size(95,26)
$g1.Controls.Add($btnRefresh)

$lblSsid = New-Object System.Windows.Forms.Label
$lblSsid.Text = "Wi-Fi name (SSID):"; $lblSsid.Location = New-Object System.Drawing.Point(20,70); $lblSsid.Size = New-Object System.Drawing.Size(115,22)
$g1.Controls.Add($lblSsid)
$tbSsid = New-Object System.Windows.Forms.TextBox
$tbSsid.Location = New-Object System.Drawing.Point(135,67); $tbSsid.Size = New-Object System.Drawing.Size(360,24)
$tbSsid.MaxLength = $WSD_SSID_MAX
$g1.Controls.Add($tbSsid)

$lblPass = New-Object System.Windows.Forms.Label
$lblPass.Text = "Wi-Fi password:"; $lblPass.Location = New-Object System.Drawing.Point(20,105); $lblPass.Size = New-Object System.Drawing.Size(115,22)
$g1.Controls.Add($lblPass)
$tbPass = New-Object System.Windows.Forms.TextBox
$tbPass.Location = New-Object System.Drawing.Point(135,102); $tbPass.Size = New-Object System.Drawing.Size(360,24)
$tbPass.MaxLength = $WSD_PWD_MAX
$g1.Controls.Add($tbPass)

$note1 = New-Object System.Windows.Forms.Label
$note1.Text = "2.4 GHz network only. SSID 1-$WSD_SSID_MAX characters; password blank (open network) or" +
              " $WSD_PWD_MIN-$WSD_PWD_MAX characters. Writes wifi.ini to the root of the card; the camera reads" +
              " it on first boot and renames it wifi.ini.applied when it joins."
$note1.Location = New-Object System.Drawing.Point(20,132); $note1.Size = New-Object System.Drawing.Size(475,48); $note1.ForeColor = [System.Drawing.Color]::DimGray
$g1.Controls.Add($note1)

$note1b = New-Object System.Windows.Forms.Label
$note1b.Text = "Your Wi-Fi password is stored on the card in PLAINTEXT -- delete wifi.ini / wifi.ini.applied once the camera is online."
$note1b.Location = New-Object System.Drawing.Point(20,182); $note1b.Size = New-Object System.Drawing.Size(475,32); $note1b.ForeColor = [System.Drawing.Color]::Firebrick
$g1.Controls.Add($note1b)

$btnWrite = New-Object System.Windows.Forms.Button
$btnWrite.Text = "Write to SD card"; $btnWrite.Location = New-Object System.Drawing.Point(135,212); $btnWrite.Size = New-Object System.Drawing.Size(160,30)
$g1.Controls.Add($btnWrite)
$lblSdStatus = New-Object System.Windows.Forms.Label
$lblSdStatus.Location = New-Object System.Drawing.Point(305,217); $lblSdStatus.Size = New-Object System.Drawing.Size(195,24); $lblSdStatus.Font = New-Object System.Drawing.Font("Segoe UI",9,[System.Drawing.FontStyle]::Bold)
$g1.Controls.Add($lblSdStatus)

# ---------- Step 2: Flash ----------
$g2 = New-Object System.Windows.Forms.GroupBox
$g2.Text = "Step 2  -  Flash the camera (USB-DFU, full chip)"
$g2.Location = New-Object System.Drawing.Point(20,384); $g2.Size = New-Object System.Drawing.Size(520,268)
$form.Controls.Add($g2)

$note2 = New-Object System.Windows.Forms.Label
$note2.Text = "One-time per PC: bind the WinUSB driver with Zadig (zadig.akeo.ie) to the a108:c309" +
              " device, or the flasher cannot see the camera.`r`n" +
              "Before clicking Flash:`r`n" +
              "  1. Open the camera and remove the metal shield (docs/HARDWARE_DFU.md).`r`n" +
              "  2. Bridge flash pins 5 + 6 with tweezers.`r`n" +
              "  3. While holding the short, plug in USB-C.`r`n" +
              "Then click Flash and follow the console window."
$note2.Location = New-Object System.Drawing.Point(20,26); $note2.Size = New-Object System.Drawing.Size(480,130)
$g2.Controls.Add($note2)

$lblTools = New-Object System.Windows.Forms.Label
$lblTools.Location = New-Object System.Drawing.Point(20,160); $lblTools.Size = New-Object System.Drawing.Size(480,40)
$lblTools.Font = New-Object System.Drawing.Font("Segoe UI",9,[System.Drawing.FontStyle]::Bold)
$g2.Controls.Add($lblTools)

$btnFlash = New-Object System.Windows.Forms.Button
$btnFlash.Text = "Flash camera now"; $btnFlash.Location = New-Object System.Drawing.Point(20,208); $btnFlash.Size = New-Object System.Drawing.Size(240,42)
$btnFlash.Font = New-Object System.Drawing.Font("Segoe UI",11,[System.Drawing.FontStyle]::Bold)
$g2.Controls.Add($btnFlash)

$btnCheck = New-Object System.Windows.Forms.Button
$btnCheck.Text = "Check required files"; $btnCheck.Location = New-Object System.Drawing.Point(275,212); $btnCheck.Size = New-Object System.Drawing.Size(160,34)
$g2.Controls.Add($btnCheck)

$lblFoot = New-Object System.Windows.Forms.Label
$lblFoot.Text = "Disassembly + pins: docs/HARDWARE_DFU.md   |   Non-destructive mtd4 path: docs/FLASHING.md" +
                "`r`nRequired files and how to sanitize an image before sharing it: README.txt (this folder)."
$lblFoot.Location = New-Object System.Drawing.Point(20,660); $lblFoot.Size = New-Object System.Drawing.Size(520,40); $lblFoot.ForeColor = [System.Drawing.Color]::DimGray
$form.Controls.Add($lblFoot)

# ---------- behavior ----------
$refresh = { $cbDrive.Items.Clear(); foreach($d in (Get-RemovableDrives)){ [void]$cbDrive.Items.Add($d) }; if($cbDrive.Items.Count -gt 0){ $cbDrive.SelectedIndex = 0 } }
$btnRefresh.Add_Click($refresh)
& $refresh

# Pre-flight for Step 2. Returns $true when everything needed is present; otherwise shows
# the specific file(s), where to get them and where to put them, and returns $false.
function Test-FlashReady {
  param([switch]$Quiet)
  if (-not $HavePreflight) {
    [System.Windows.Forms.MessageBox]::Show(
      "kit-preflight.ps1 is missing from`r`n$Base`r`n`r`nIt ships with this kit -- re-copy the whole tools\windows-flasher folder.",
      "Kit incomplete") | Out-Null
    return $false
  }
  $rep = Get-BW4Readiness -Base $Base
  if ($rep.Ready) {
    if (-not $Quiet) {
      [System.Windows.Forms.MessageBox]::Show("All required files are present:`r`n`r`n" +
        (($rep.Requirements | Where-Object { $_.Found } | ForEach-Object { "  " + $_.Name + "`r`n    " + $_.Found }) -join "`r`n"),
        "Ready to flash") | Out-Null
    }
    return $true
  }
  [System.Windows.Forms.MessageBox]::Show(
    "This kit is incomplete, so flashing would fail. Nothing has been written.`r`n`r`n" +
    $rep.Text + "`r`n`r`n" +
    "These files are third-party or contain vendor firmware, so they are deliberately not part of the BW4 repository. See README.txt.",
    "Missing files", [System.Windows.Forms.MessageBoxButtons]::OK,
    [System.Windows.Forms.MessageBoxIcon]::Error) | Out-Null
  return $false
}

# status line under Step 2, refreshed at startup
$updateTools = {
  if (-not $HavePreflight) {
    $lblTools.ForeColor = [System.Drawing.Color]::Firebrick
    $lblTools.Text = "kit-preflight.ps1 missing -- this kit folder is incomplete."
    return
  }
  $rep = Get-BW4Readiness -Base $Base
  if ($rep.Ready) {
    $lblTools.ForeColor = [System.Drawing.Color]::DarkGreen
    $lblTools.Text = "Flashing tools + 8 MiB image found."
  } else {
    $lblTools.ForeColor = [System.Drawing.Color]::Firebrick
    $names = ($rep.Missing | ForEach-Object { $_.Name }) -join ", "
    if (-not $names) { $names = "image problem" }
    $lblTools.Text = "Not ready: $names`r`nClick 'Check required files' for what to download and where to put it."
  }
}
& $updateTools
$btnCheck.Add_Click({ [void](Test-FlashReady) ; & $updateTools })

$btnWrite.Add_Click({
  $lblSdStatus.ForeColor = [System.Drawing.Color]::Black; $lblSdStatus.Text = ""
  if($cbDrive.SelectedItem -eq $null){ $lblSdStatus.ForeColor=[System.Drawing.Color]::Red; $lblSdStatus.Text="Pick a drive"; return }
  $ssid = $tbSsid.Text.Trim()
  $pass = $tbPass.Text

  # Validate exactly what src/shims/parse_ini.h accepts, so a card this GUI writes can
  # never be silently ignored by the camera (a bad file is a clean no-op on the device --
  # it just never joins, with no feedback).
  if($ssid.Length -lt 1){ $lblSdStatus.ForeColor=[System.Drawing.Color]::Red; $lblSdStatus.Text="Enter the Wi-Fi name"; return }
  if($ssid.Length -gt $WSD_SSID_MAX){
    $lblSdStatus.ForeColor=[System.Drawing.Color]::Red; $lblSdStatus.Text="SSID over $WSD_SSID_MAX chars"
    [System.Windows.Forms.MessageBox]::Show("The camera accepts an SSID of 1-$WSD_SSID_MAX characters (802.11 limit). Yours is $($ssid.Length).","Cannot write") | Out-Null
    return }
  if($pass.Length -gt 0 -and ($pass.Length -lt $WSD_PWD_MIN -or $pass.Length -gt $WSD_PWD_MAX)){
    $lblSdStatus.ForeColor=[System.Drawing.Color]::Red; $lblSdStatus.Text="Password $WSD_PWD_MIN-$WSD_PWD_MAX chars"
    [System.Windows.Forms.MessageBox]::Show("The camera accepts a WPA/WPA2 password of $WSD_PWD_MIN-$WSD_PWD_MAX characters, or an empty one for an open network. Yours is $($pass.Length).","Cannot write") | Out-Null
    return }
  if($pass -ne $pass.Trim()){
    $lblSdStatus.ForeColor=[System.Drawing.Color]::Red; $lblSdStatus.Text="No leading/trailing spaces"
    [System.Windows.Forms.MessageBox]::Show("The camera's parser trims whitespace around values, so a password that starts or ends with a space or tab cannot be delivered this way.","Cannot write") | Out-Null
    return }
  if($tbSsid.Text -ne $ssid){
    [System.Windows.Forms.MessageBox]::Show("Leading/trailing spaces were removed from the Wi-Fi name -- the camera's parser trims them, so they cannot be part of the SSID.","Note") | Out-Null
  }
  $nonAscii = $false
  foreach($ch in ($ssid + $pass).ToCharArray()){ if([int]$ch -gt 126 -or [int]$ch -lt 32){ $nonAscii = $true } }
  if($nonAscii){
    $r = [System.Windows.Forms.MessageBox]::Show("The Wi-Fi name or password contains non-ASCII characters. The file is written as UTF-8; the camera's firmware may not interpret them the same way, so the join may fail.`r`n`r`nWrite anyway?","Non-ASCII characters",[System.Windows.Forms.MessageBoxButtons]::OKCancel)
    if($r -ne [System.Windows.Forms.DialogResult]::OK){ $lblSdStatus.Text=""; return }
  }

  $drv = ($cbDrive.SelectedItem.ToString() -split '\s')[0]   # "E:"
  if(-not (Test-Path -LiteralPath ($drv + "\"))){
    $lblSdStatus.ForeColor=[System.Drawing.Color]::Red; $lblSdStatus.Text="Drive gone - Refresh"; return }

  # The camera mounts the card at /mnt/sda0; its kernel only handles FAT.
  try {
    $vol = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$drv'" -ErrorAction Stop
    if($vol -and $vol.FileSystem -and ($vol.FileSystem -notmatch '^FAT')){
      $r = [System.Windows.Forms.MessageBox]::Show("This card is formatted $($vol.FileSystem). The camera mounts the SD card as FAT/FAT32 -- it will probably not read an $($vol.FileSystem) card, and will silently never join Wi-Fi.`r`n`r`nWrite anyway?","Card format",[System.Windows.Forms.MessageBoxButtons]::OKCancel)
      if($r -ne [System.Windows.Forms.DialogResult]::OK){ $lblSdStatus.Text=""; return }
    }
  } catch { }

  # wifi_sd.so reads /mnt/sda0/wifi.ini -> the ROOT of the card. Keys/format per
  # src/shims/parse_ini.h: "KEY=value" lines, LF, '#' comments allowed, no BOM.
  $path = Join-Path ($drv + "\") "wifi.ini"
  $content = "# BW4 wifi_sd onboarding file - read once on boot, then renamed wifi.ini.applied.`n" +
             "# Contains your Wi-Fi password in plaintext: delete it after the camera is online.`n" +
             "SSID=$ssid`n" +
             "PASSWORD=$pass`n"
  try {
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)   # a BOM would break the first key
    [System.IO.File]::WriteAllText($path, $content, $utf8NoBom)
    $lblSdStatus.ForeColor=[System.Drawing.Color]::Green; $lblSdStatus.Text="Written to $path"
    [System.Windows.Forms.MessageBox]::Show(
      "wifi.ini written to $path`r`n`r`nSafely eject the card and put it in the camera.`r`n`r`nOn the first boot after flashing, the camera reads it, joins your Wi-Fi and renames the file wifi.ini.applied. Find the camera's IP on your router's DHCP list or via ONVIF discovery from your NVR.`r`n`r`nAfterwards, put the card back in this PC and DELETE wifi.ini / wifi.ini.applied -- your Wi-Fi password is on it in plaintext.",
      "Done") | Out-Null
  } catch {
    $lblSdStatus.ForeColor=[System.Drawing.Color]::Red; $lblSdStatus.Text="Write failed"
    [System.Windows.Forms.MessageBox]::Show("Could not write to $path`r`n$($_.Exception.Message)","Error") | Out-Null
  }
})

$btnFlash.Add_Click({
  $script = Join-Path $Base "flash-console.ps1"
  if(-not (Test-Path -LiteralPath $script)){
    [System.Windows.Forms.MessageBox]::Show("flash-console.ps1 is missing from`r`n$Base`r`n`r`nIt ships with this kit -- re-copy the whole tools\windows-flasher folder.","Kit incomplete") | Out-Null
    return }
  if(-not (Test-FlashReady -Quiet)){ & $updateTools; return }

  $warn = "FULL-CHIP WRITE - read before you continue.`r`n`r`n" +
          "This writes the whole 8 MiB flash, which RESETS the camera's NVS (mtd5):`r`n" +
          "  - the saved Wi-Fi credentials are ERASED (the camera re-provisions from the wifi.ini you wrote in Step 1);`r`n" +
          "  - the unit returns to its factory settings and default device password.`r`n`r`n" +
          "The BW4 project's normal update never does this: it writes only mtd4 and leaves the bootloader, kernel, rootfs and NVS untouched (docs/FLASHING.md - 'only mtd4 is ever written', 'NEVER DFU unless the camera won't boot at all').`r`n`r`n" +
          "Use this kit only if the camera will not boot, or if you have no serial console for the mtd4 path.`r`n`r`n" +
          "Make sure flash pins 5 + 6 are shorted and you are ready to plug in USB-C when asked.`r`n`r`n" +
          "Start the full-chip flash now?"
  $ok = [System.Windows.Forms.MessageBox]::Show($warn,"Flash camera (full chip)",
        [System.Windows.Forms.MessageBoxButtons]::OKCancel,[System.Windows.Forms.MessageBoxIcon]::Warning)
  if($ok -ne [System.Windows.Forms.DialogResult]::OK){ return }
  Start-Process -FilePath "powershell.exe" -ArgumentList @("-NoProfile","-ExecutionPolicy","Bypass","-File","`"$script`"") -WindowStyle Normal
})

[void]$form.ShowDialog()
