@echo off
if exist "%~dp0tools\bundled-tools.txt" (
  start "" "%~dp0TenchuZ.exe"
  exit /b 0
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup_windows.ps1"
set "setupResult=%errorlevel%"
pause
exit /b %setupResult%
