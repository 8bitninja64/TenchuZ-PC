@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup_windows.ps1"
set "setupResult=%errorlevel%"
pause
exit /b %setupResult%
