@echo off
REM ==========================================================================
REM mod-bot-social: patch installer for mod-playerbots (double-click wrapper)
REM
REM All logic lives in apply-playerbots-patches.ps1 - one implementation, so
REM the two entry points cannot drift apart. Arguments are passed through:
REM   apply-playerbots-patches.bat -Check
REM   apply-playerbots-patches.bat -Revert
REM ==========================================================================
setlocal

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0apply-playerbots-patches.ps1" -SkipPause %*
set RC=%ERRORLEVEL%

echo.
pause
exit /b %RC%
