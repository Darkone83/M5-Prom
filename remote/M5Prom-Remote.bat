@echo off
setlocal
cd /d "%~dp0"
title M5 Prom Remote

if not exist "%~dp0.venv\Scripts\python.exe" (
    echo Setting up M5 Prom Remote for first use...
    where py >nul 2>nul
    if not errorlevel 1 (
        py -3 -m venv "%~dp0.venv"
    ) else (
        where python >nul 2>nul
        if errorlevel 1 (
            echo Python 3.10 or newer is required. Install it from https://www.python.org/downloads/
            pause
            exit /b 1
        )
        python -m venv "%~dp0.venv"
    )
    if errorlevel 1 (
        echo Could not create the local Python environment.
        pause
        exit /b 1
    )
)
"%~dp0.venv\Scripts\python.exe" "%~dp0bootstrap.py"
if errorlevel 1 (
    echo.
    echo M5 Prom Remote could not start. Check the message above and try again.
    pause
    exit /b 1
)
endlocal
