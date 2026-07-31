@echo off
REM Double-click to launch the BW4 Camera Flasher GUI.
REM Uses -STA (required for the Windows Forms UI) and bypasses execution policy
REM for this one script only (nothing is installed).
REM The kit needs files that are NOT in the BW4 repo (both DFU tools + an 8 MiB
REM full-chip image) - the GUI checks for them and tells you what to fetch.
REM See README.txt in this folder before flashing: Step 2 is a WHOLE-CHIP write and
REM resets the camera's NVS (Wi-Fi credentials erased).
setlocal
if not exist "%~dp0BW4-Flasher.ps1" (
  echo BW4-Flasher.ps1 is missing from "%~dp0".
  echo Copy the whole tools\windows-flasher folder, then run this again.
  pause
  exit /b 1
)
if not exist "%~dp0kit-preflight.ps1" (
  echo kit-preflight.ps1 is missing from "%~dp0".
  echo Copy the whole tools\windows-flasher folder, then run this again.
  pause
  exit /b 1
)
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0BW4-Flasher.ps1"
