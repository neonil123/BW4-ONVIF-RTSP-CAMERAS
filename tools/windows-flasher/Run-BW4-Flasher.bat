@echo off
REM Double-click to launch the BW4 Camera Flasher GUI.
REM Uses -STA (required for the Windows Forms UI) and bypasses execution policy
REM for this one script only (nothing is installed).
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0BW4-Flasher.ps1"
