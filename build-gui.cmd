@echo off
rem Opens the build window, build-gui.ps1, in Windows PowerShell - which
rem every Windows has - without leaving a console window open behind it.
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0build-gui.ps1"
