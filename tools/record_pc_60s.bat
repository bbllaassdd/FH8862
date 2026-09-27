@echo off
setlocal

rem Pass 1235 as the first argument to record the substream instead.
set "PORT=1234"
if not "%~1"=="" set "PORT=%~1"

set "VLC=%ProgramFiles%\VideoLAN\VLC\vlc.exe"
if not exist "%VLC%" (
    echo VLC was not found at: %VLC%
    exit /b 1
)

set "OUTPUT=%CD%\project_pc_port%PORT%_60s.ts"
echo Recording udp://@:%PORT% for 60 seconds...
"%VLC%" "udp://@:%PORT%" --network-caching=300 --run-time=60 --sout="#standard{access=file,mux=ts,dst=%OUTPUT%}" vlc://quit

echo Finished: %OUTPUT%
endlocal
