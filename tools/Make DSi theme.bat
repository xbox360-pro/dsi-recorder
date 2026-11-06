@echo off
rem Drag a picture onto this file to turn it into theme.bmp for DSi Recorder.
if "%~1"=="" (
    echo Drag a picture onto this file to make theme.bmp for DSi Recorder.
    echo.
    pause
    exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0convert-theme.ps1" "%~1"
echo.
pause
