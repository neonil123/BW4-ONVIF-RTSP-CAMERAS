# BW4-Flasher.ps1 -- simple Windows GUI for non-technical users.
#  Step 1: write a WiFi wifi.ini onto an SD card.
#  Step 2: flash the camera over USB (launches flash-console.ps1 in its own window).
# Launch via Run-BW4-Flasher.bat (needs -STA). No install required.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$Base = $PSScriptRoot

function Get-RemovableDrives {
  try { Get-CimInstance Win32_LogicalDisk -Filter "DriveType=2" -ErrorAction Stop |
        ForEach-Object { "{0}  {1}" -f $_.DeviceID, ($_.VolumeName) } }
  catch { @() }
}

$form = New-Object System.Windows.Forms.Form
$form.Text = "BW4 Camera Flasher"
$form.Size = New-Object System.Drawing.Size(560,640)
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedSingle"
$form.MaximizeBox = $false

$title = New-Object System.Windows.Forms.Label
$title.Text = "BW4 Camera Setup"
$title.Font = New-Object System.Drawing.Font("Segoe UI",16,[System.Drawing.FontStyle]::Bold)
$title.Location = New-Object System.Drawing.Point(20,15)
$title.Size = New-Object System.Drawing.Size(500,32)
$form.Controls.Add($title)

# ---------- Step 1: WiFi SD ----------
$g1 = New-Object System.Windows.Forms.GroupBox
$g1.Text = "Step 1  -  Create the WiFi SD card"
$g1.Location = New-Object System.Drawing.Point(20,55)
$g1.Size = New-Object System.Drawing.Size(510,235)
$form.Controls.Add($g1)

$lblDrive = New-Object System.Windows.Forms.Label
$lblDrive.Text = "SD card drive:"; $lblDrive.Location = New-Object System.Drawing.Point(20,35); $lblDrive.Size = New-Object System.Drawing.Size(110,22)
$g1.Controls.Add($lblDrive)
$cbDrive = New-Object System.Windows.Forms.ComboBox
$cbDrive.Location = New-Object System.Drawing.Point(135,32); $cbDrive.Size = New-Object System.Drawing.Size(250,24); $cbDrive.DropDownStyle = "DropDownList"
$g1.Controls.Add($cbDrive)
$btnRefresh = New-Object System.Windows.Forms.Button
$btnRefresh.Text = "Refresh"; $btnRefresh.Location = New-Object System.Drawing.Point(395,31); $btnRefresh.Size = New-Object System.Drawing.Size(95,26)
$g1.Controls.Add($btnRefresh)

$lblSsid = New-Object System.Windows.Forms.Label
$lblSsid.Text = "WiFi name (SSID):"; $lblSsid.Location = New-Object System.Drawing.Point(20,75); $lblSsid.Size = New-Object System.Drawing.Size(110,22)
$g1.Controls.Add($lblSsid)
$tbSsid = New-Object System.Windows.Forms.TextBox
$tbSsid.Location = New-Object System.Drawing.Point(135,72); $tbSsid.Size = New-Object System.Drawing.Size(355,24)
$g1.Controls.Add($tbSsid)

$lblPass = New-Object System.Windows.Forms.Label
$lblPass.Text = "WiFi password:"; $lblPass.Location = New-Object System.Drawing.Point(20,110); $lblPass.Size = New-Object System.Drawing.Size(110,22)
$g1.Controls.Add($lblPass)
$tbPass = New-Object System.Windows.Forms.TextBox
$tbPass.Location = New-Object System.Drawing.Point(135,107); $tbPass.Size = New-Object System.Drawing.Size(355,24)
$g1.Controls.Add($tbPass)

$note1 = New-Object System.Windows.Forms.Label
$note1.Text = "Use a 2.4 GHz network. Password must be 8+ characters (or blank for an open network)."
$note1.Location = New-Object System.Drawing.Point(20,138); $note1.Size = New-Object System.Drawing.Size(470,34); $note1.ForeColor = [System.Drawing.Color]::DimGray
$g1.Controls.Add($note1)

$btnWrite = New-Object System.Windows.Forms.Button
$btnWrite.Text = "Write to SD card"; $btnWrite.Location = New-Object System.Drawing.Point(135,175); $btnWrite.Size = New-Object System.Drawing.Size(160,32)
$g1.Controls.Add($btnWrite)
$lblSdStatus = New-Object System.Windows.Forms.Label
$lblSdStatus.Location = New-Object System.Drawing.Point(305,180); $lblSdStatus.Size = New-Object System.Drawing.Size(190,24); $lblSdStatus.Font = New-Object System.Drawing.Font("Segoe UI",9,[System.Drawing.FontStyle]::Bold)
$g1.Controls.Add($lblSdStatus)

# ---------- Step 2: Flash ----------
$g2 = New-Object System.Windows.Forms.GroupBox
$g2.Text = "Step 2  -  Flash the camera (USB)"
$g2.Location = New-Object System.Drawing.Point(20,300); $g2.Size = New-Object System.Drawing.Size(510,220)
$form.Controls.Add($g2)

$note2 = New-Object System.Windows.Forms.Label
$note2.Text = "Before clicking Flash:`r`n  1. Open the camera and remove the metal shield (see the guide).`r`n  2. Bridge flash pins 5 + 6 with tweezers.`r`n  3. While holding the short, plug in USB-C.`r`nThen click Flash and follow the console window."
$note2.Location = New-Object System.Drawing.Point(20,30); $note2.Size = New-Object System.Drawing.Size(470,110)
$g2.Controls.Add($note2)

$btnFlash = New-Object System.Windows.Forms.Button
$btnFlash.Text = "Flash camera now"; $btnFlash.Location = New-Object System.Drawing.Point(135,150); $btnFlash.Size = New-Object System.Drawing.Size(240,42)
$btnFlash.Font = New-Object System.Drawing.Font("Segoe UI",11,[System.Drawing.FontStyle]::Bold)
$g2.Controls.Add($btnFlash)

$lblFoot = New-Object System.Windows.Forms.Label
$lblFoot.Text = "Full guide: docs/HARDWARE_DFU.md   -   Local RTSP/ONVIF, no cloud."
$lblFoot.Location = New-Object System.Drawing.Point(20,535); $lblFoot.Size = New-Object System.Drawing.Size(510,24); $lblFoot.ForeColor = [System.Drawing.Color]::DimGray
$form.Controls.Add($lblFoot)

# ---------- behavior ----------
$refresh = { $cbDrive.Items.Clear(); foreach($d in (Get-RemovableDrives)){ [void]$cbDrive.Items.Add($d) }; if($cbDrive.Items.Count -gt 0){ $cbDrive.SelectedIndex = 0 } }
$btnRefresh.Add_Click($refresh)
& $refresh

$btnWrite.Add_Click({
  $lblSdStatus.ForeColor = [System.Drawing.Color]::Black; $lblSdStatus.Text = ""
  if($cbDrive.SelectedItem -eq $null){ $lblSdStatus.ForeColor=[System.Drawing.Color]::Red; $lblSdStatus.Text="Pick a drive"; return }
  $ssid = $tbSsid.Text.Trim()
  $pass = $tbPass.Text
  if($ssid.Length -lt 1){ $lblSdStatus.ForeColor=[System.Drawing.Color]::Red; $lblSdStatus.Text="Enter SSID"; return }
  if($pass.Length -gt 0 -and $pass.Length -lt 8){ $lblSdStatus.ForeColor=[System.Drawing.Color]::Red; $lblSdStatus.Text="Password 8+ chars"; return }
  $drv = ($cbDrive.SelectedItem.ToString() -split '\s')[0]   # "E:"
  $path = Join-Path ($drv + "\") "wifi.ini"
  $content = "SSID=$ssid`nPASSWORD=$pass`n"
  try {
    [System.IO.File]::WriteAllText($path, $content)
    $lblSdStatus.ForeColor=[System.Drawing.Color]::Green; $lblSdStatus.Text="Written to $path"
    [System.Windows.Forms.MessageBox]::Show("wifi.ini written to $path`r`n`r`nSafely eject the SD and put it in the camera.","Done") | Out-Null
  } catch {
    $lblSdStatus.ForeColor=[System.Drawing.Color]::Red; $lblSdStatus.Text="Write failed"
    [System.Windows.Forms.MessageBox]::Show("Could not write to $path`r`n$($_.Exception.Message)","Error") | Out-Null
  }
})

$btnFlash.Add_Click({
  $script = Join-Path $Base "flash-console.ps1"
  if(-not (Test-Path $script)){ [System.Windows.Forms.MessageBox]::Show("flash-console.ps1 not found next to this app.","Error") | Out-Null; return }
  $ok = [System.Windows.Forms.MessageBox]::Show("Make sure pins 5+6 are shorted and you are ready to plug in USB-C when asked.`r`n`r`nStart flashing now?","Flash camera",[System.Windows.Forms.MessageBoxButtons]::OKCancel)
  if($ok -ne [System.Windows.Forms.DialogResult]::OK){ return }
  Start-Process -FilePath "powershell.exe" -ArgumentList @("-NoProfile","-ExecutionPolicy","Bypass","-File","`"$script`"") -WindowStyle Normal
})

[void]$form.ShowDialog()
