@echo off
setlocal
cd /d "%~dp0"
if exist "portable\python\python.exe" (
  "portable\python\python.exe" tools\plc_studio.py %*
  goto finished
)
where py >nul 2>nul
if not errorlevel 1 (
  py -3 tools\plc_studio.py %*
) else (
  python tools\plc_studio.py %*
)
:finished
if errorlevel 1 (
  echo.
  echo Use the Windows Portable package for bundled Python and GCC.
  echo Extract the complete ZIP before starting. See docs\windows-studio.md.
  pause
)
