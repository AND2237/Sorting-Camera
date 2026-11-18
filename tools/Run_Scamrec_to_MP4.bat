@echo off
setlocal
title Sorting Camera - SCAMREC to MP4 Converter
cd /d "%~dp0"

echo Starting SCAMREC to MP4 Converter...
echo.

where py >nul 2>nul
if not errorlevel 1 (
    py -3 "%~dp0scamrec_to_mp4.py"
) else (
    where python >nul 2>nul
    if not errorlevel 1 (
        python "%~dp0scamrec_to_mp4.py"
    ) else (
        echo ERROR: Python was not found. Install Python 3 and make sure it is available on PATH.
    )
)

echo.
echo Converter closed. Press any key to close this window...
pause >nul
endlocal
