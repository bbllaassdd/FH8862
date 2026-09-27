@echo off
setlocal
cd /d "%~dp0"
title FH8862 Demo Console

echo Starting FH8862 presentation console...
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass ^
  -File "%~dp0dashboard\server.ps1"

if errorlevel 1 (
    echo.
    echo The console stopped with an error.
    pause
)
endlocal
