@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\launch_system_link.ps1"
if errorlevel 1 pause
