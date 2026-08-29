@echo off
REM =========================================================================
REM mod-bot-social: Patch installer for mod-playerbots
REM =========================================================================
setlocal enabledelayedexpansion

set SCRIPT_DIR=%~dp0
set MODULES_ROOT=%SCRIPT_DIR%..
set PLAYERBOTS_DIR=%MODULES_ROOT%\mod-playerbots

echo.
echo [mod-bot-social] Playerbots Patch Installer
echo.

REM --- Check if mod-playerbots exists ---
if not exist "%PLAYERBOTS_DIR%\.git" (
    echo [ERROR] mod-playerbots not found at %PLAYERBOTS_DIR%
    echo [ERROR] Clone mod-playerbots first, then re-run this script.
    pause
    exit /b 1
)

REM --- Check git is available ---
where git >nul 2>&1
if errorlevel 1 (
    echo [ERROR] git.exe not found in PATH.
    echo [ERROR] Install Git for Windows from https://git-scm.com/download/win
    pause
    exit /b 1
)

REM --- Apply patches ---
REM %SCRIPT_DIR% already ends with a backslash, but be defensive.
if "%SCRIPT_DIR:~-1%" neq "\" set SCRIPT_DIR=%SCRIPT_DIR%\
set PATCHES_DIR=%SCRIPT_DIR%patches
set APPLIED_COUNT=0
set FAILED_COUNT=0

for %%P in ("%PATCHES_DIR%\*.patch") do (
    set PATCH_FILE=%%P
    set PATCH_NAME=%%~nxP
    
    echo [INFO] Processing !PATCH_NAME!...
    
    REM Extract target slug from filename (02-playerbots-...)
    set SLUG=
    for /f "tokens=2 delims=-" %%S in ("!PATCH_NAME!") do set SLUG=%%S
    
    if "!SLUG!"=="" (
        echo [WARN] Cannot parse target from !PATCH_NAME! - skipping
        goto :next_patch
    )
    
    if not "!SLUG!"=="playerbots" (
        echo [WARN] Patch targets !SLUG!, not playerbots - skipping
        goto :next_patch
    )
    
    REM Check if already applied by looking for marker in target file
    REM The marker is declared in manifest.json; we extract the target file from the patch itself
    REM git adds a TAB + timestamp after the filename on the +++ b/ line, so we stop at the first TAB
    set TARGET_FILE=
    for /f "tokens=2 delims= " %%F in ('findstr /r "^+++ b/" "!PATCH_FILE!"') do (
        set TARGET_FILE=%%F
        goto :found_target
    )
    :found_target
    set TARGET_FILE=!TARGET_FILE:b/=!
    REM Strip everything after the first TAB (timestamp)
    for /f "delims=	" %%T in ("!TARGET_FILE!") do set TARGET_FILE=%%T
    
    if "!TARGET_FILE!"=="" (
        echo [WARN] Cannot determine target file from !PATCH_NAME! - skipping
        goto :next_patch
    )
    
    set FULL_TARGET=%PLAYERBOTS_DIR%\!TARGET_FILE!
    
    REM Check for applied marker (hardcoded for now; could parse manifest.json)
    if exist "!FULL_TARGET!" (
        findstr /c:"outlive its leader's character row" "!FULL_TARGET!" >nul 2>&1
        if not errorlevel 1 (
            echo [OK] Already applied - skipping
            set /a APPLIED_COUNT+=1
            goto :next_patch
        )
        findstr /c:"Koennen pro Bot statt einheitlich" "!FULL_TARGET!" >nul 2>&1
        if not errorlevel 1 (
            echo [OK] Already applied - skipping
            set /a APPLIED_COUNT+=1
            goto :next_patch
        )
    )
    
    REM Try git apply. Use -C <dir> instead of pushd/popd to avoid TTY hangs.
    git -C "%PLAYERBOTS_DIR%" apply --check --whitespace=nowarn "!PATCH_FILE!" >nul 2>&1
    if not errorlevel 1 (
        git -C "%PLAYERBOTS_DIR%" apply --whitespace=nowarn "!PATCH_FILE!" >nul 2>&1
        if not errorlevel 1 (
            echo [OK] Applied via git apply
            set /a APPLIED_COUNT+=1
            goto :next_patch
        )
    )
    
    REM Fallback: copy pre-patched file
    set PREPATCHED_DIR=%PATCHES_DIR%\!SLUG!
    set PREPATCHED_FILE=!PREPATCHED_DIR!\!TARGET_FILE!
    
    if exist "!PREPATCHED_FILE!" (
        for %%D in ("!FULL_TARGET!") do set TARGET_DIR=%%~dpD
        if not exist "!TARGET_DIR!" mkdir "!TARGET_DIR!"
        copy /y "!PREPATCHED_FILE!" "!FULL_TARGET!" >nul
        echo [OK] Applied via pre-patched copy
        set /a APPLIED_COUNT+=1
    ) else (
        echo [ERROR] Could not apply - no pre-patched file at !PREPATCHED_FILE!
        set /a FAILED_COUNT+=1
    )
    
    :next_patch
)

echo.
echo [SUMMARY] Applied: !APPLIED_COUNT! / Failed: !FAILED_COUNT!
echo.

if !FAILED_COUNT! gtr 0 (
    echo [WARN] Some patches failed. See README.md for manual installation steps.
)

pause
exit /b 0
