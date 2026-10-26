@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "GUI_PY=%~dp0.flasher-env\Scripts\pythonw.exe"
set "READY=%~dp0.flasher-env\.ready"
if exist "%READY%" if exist "%GUI_PY%" (
  start "" "%GUI_PY%" "%~dp0flash.py" --run-gui
  exit /b 0
)

title M5 Prom - First Run Setup
echo Preparing M5 Prom Flasher. Missing components will be installed automatically.
echo.
where py >nul 2>&1
if not errorlevel 1 (
    py -3 -c "import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)" >nul 2>&1
    if not errorlevel 1 (
        py -3 "%~dp0flash.py"
        if errorlevel 1 goto :FAILED
        exit /b 0
    )
)
where python >nul 2>&1
if not errorlevel 1 (
    python -c "import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)" >nul 2>&1
    if not errorlevel 1 (
        python "%~dp0flash.py"
        if errorlevel 1 goto :FAILED
        exit /b 0
    )
)
echo Python 3.10 or newer is required for first-run setup.
echo Get Python from https://www.python.org/downloads/windows/
echo Choose "Add Python to PATH" during installation, then double-click this file again.
start "" "https://www.python.org/downloads/windows/"
goto :FAILED
:FAILED
echo.
echo Setup could not finish. Check the message above, then retry.
pause
exit /b 1
