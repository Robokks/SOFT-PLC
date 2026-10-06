@echo off
setlocal
cd /d "%~dp0"
where py >nul 2>nul
if not errorlevel 1 (
  py -3 tools\plc_studio.py
) else (
  python tools\plc_studio.py
)
if errorlevel 1 (
  echo.
  echo Install Python 3.10 or later and Visual Studio 2022 Build Tools with Desktop development with C++.
  pause
)
