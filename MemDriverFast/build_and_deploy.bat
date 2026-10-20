@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM  MemDriverFast - build and deploy to USB
REM
REM  Builds the per-process-scoped driver + loader, builds the
REM  Windows user-mode client, and lays everything out on a
REM  FAT32 USB key ready to boot.
REM
REM  USB target drive defaults to E:.  Pass another letter as
REM  the first argument (e.g. build_and_deploy.bat F) to change.
REM ============================================================

set USB=E:
if not "%~1"=="" set USB=%~1:

set ROOT=%~dp0
set PKG_SRC=%ROOT%pkg
set PKG_NAME=MemDriverFastPkg

REM ---- Auto-locate the EDK II workspace ----
REM Try, in order:
REM   1. %ROOT%edkII                (bundled next to this script)
REM   2. %ROOT%..\MemDriverPkg\edkII
REM   3. %ROOT%..\MemDriverPkg2\edkII
set EDK2_ROOT=
if exist "%ROOT%edkII\edksetup.bat"                      set "EDK2_ROOT=%ROOT%edkII"
if "!EDK2_ROOT!"=="" if exist "%ROOT%..\MemDriverPkg\edkII\edksetup.bat"  set "EDK2_ROOT=%ROOT%..\MemDriverPkg\edkII"
if "!EDK2_ROOT!"=="" if exist "%ROOT%..\MemDriverPkg2\edkII\edksetup.bat" set "EDK2_ROOT=%ROOT%..\MemDriverPkg2\edkII"

if "!EDK2_ROOT!"=="" (
    echo [!] Could not locate an EDK II workspace. Expected one of:
    echo       %ROOT%edkII\
    echo       %ROOT%..\MemDriverPkg\edkII\
    echo       %ROOT%..\MemDriverPkg2\edkII\
    exit /b 1
)

set PKG_DST=!EDK2_ROOT!\%PKG_NAME%

set OUT=!EDK2_ROOT!\Build\%PKG_NAME%\RELEASE_VS2022\X64

echo ============================================
echo   MemDriverFast  -  build + deploy
echo ============================================
echo   USB target  : %USB%
echo   EDK II root : !EDK2_ROOT!
echo   Package src : %PKG_SRC%
echo   Package dst : !PKG_DST!
echo.

REM ---- USB sanity check ----
if not exist %USB%\ (
    echo [!] USB drive %USB% not found.  Pass drive letter as argument.
    exit /b 1
)
echo [+] USB drive %USB% found

REM ---- VS2022 ----
set VCVARS=
for %%D in (
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\Professional"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise"
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
    "C:\Program Files\Microsoft Visual Studio\2022\Community"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
) do (
    if exist "%%~D\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=%%~D\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if "!VCVARS!"=="" (
    echo [!] VS2022 build tools not found.
    exit /b 1
)
call "!VCVARS!" >nul 2>&1
echo [+] VS2022 environment loaded

REM ---- NASM ----
set PATH=C:\NASM\nasm-2.16.03;%PATH%
set NASM_PREFIX=C:\NASM\nasm-2.16.03\
nasm --version >nul 2>&1
if errorlevel 1 (
    echo [!] NASM not found at C:\NASM\nasm-2.16.03
    exit /b 1
)
echo [+] NASM found

REM ---- Stage package into EDK II tree -----------------------------------
REM A prior failed run can leave the destination as a FILE instead of a
REM directory - robocopy then refuses with exit code 16 ("directory name
REM is invalid").  We probe first (using goto to avoid nested if blocks
REM whose branches contain parentheses in echo strings, which cmd would
REM misparse as block terminators).
REM --------------------------------------------------------------------
echo [*] Staging package to !PKG_DST!

if exist "!PKG_DST!\*" goto :stage_ready
if exist "!PKG_DST!" goto :stage_delfile

mkdir "!PKG_DST!" >nul 2>&1
goto :stage_ready

:stage_delfile
echo     destination was a stray file - deleting
del /f /q "!PKG_DST!" >nul 2>&1
if exist "!PKG_DST!" (
    echo [!] Could not remove stray file at !PKG_DST!
    exit /b 1
)
mkdir "!PKG_DST!" >nul 2>&1

:stage_ready
robocopy "%PKG_SRC%" "!PKG_DST!" /MIR /NFL /NDL /NJH /NJS /NC /NS /NP >nul
set RC_ERR=%errorlevel%
if %RC_ERR% GEQ 8 (
    echo [!] robocopy failed, exit code %RC_ERR%.
    echo     Target may be in use by a prior build. Close any editors or
    echo     build windows that have files in !PKG_DST! open and retry.
    exit /b 1
)

REM Confirm the key files really arrived
set MISSING=
for %%F in (MemDriver.c PhysMem.nasm MemDriver.inf Loader.c Loader.inf %PKG_NAME%.dsc) do (
    if not exist "!PKG_DST!\%%F" set "MISSING=!MISSING! %%F"
)
if not "!MISSING!"=="" (
    echo [!] Missing after stage:!MISSING!
    exit /b 1
)
echo [+] Package staged

REM ---- Build driver + loader --------------------------------------------
REM The EDK II `build` command is extremely chatty - it echoes every
REM nasm/cl.exe/link.exe/Trim/GenFw/copy invocation with full argument
REM lists.  We redirect everything to a log file.  On success, print a
REM short summary; on failure, dump the log so the user can see what went
REM wrong.
REM --------------------------------------------------------------------
set BUILD_LOG=!EDK2_ROOT!\build-!PKG_NAME!.log
pushd "!EDK2_ROOT!"
call "!EDK2_ROOT!\edksetup.bat" >nul 2>&1
echo.
echo [*] Building %PKG_NAME% ^(log: !BUILD_LOG!^)...
REM 'build' is a .bat; without 'call', cmd exits the parent script on return
call build -a X64 -t VS2022 -p %PKG_NAME%/%PKG_NAME%.dsc -b RELEASE >"!BUILD_LOG!" 2>&1
set BUILD_ERR=%errorlevel%
popd

if %BUILD_ERR% neq 0 (
    echo.
    echo [!] Build failed. Last lines of !BUILD_LOG!:
    echo ----------------------------------------------------------------
    powershell -NoProfile -Command "Get-Content -Tail 50 '!BUILD_LOG!'" 2>nul
    echo ----------------------------------------------------------------
    exit /b 1
)

if not exist "!OUT!\MemDriver.efi" (
    echo [!] MemDriver.efi not found in !OUT!
    exit /b 1
)
if not exist "!OUT!\Loader.efi" (
    echo [!] Loader.efi not found in !OUT!
    exit /b 1
)
echo [+] Driver + loader built
echo     !OUT!\MemDriver.efi
echo     !OUT!\Loader.efi

REM ---- Deploy to USB -----------------------------------------------------
REM Two EFI artefacts land on the USB.  Pulled from well-known paths; if
REM one is missing we bail with a clear message rather than deploy half a
REM boot medium.  Loader.efi is RENAMED to bootx64.efi on the way across.
REM The Windows user-mode client is built separately (see EFI\client\).
REM --------------------------------------------------------------------
echo.
echo [*] Discovering build artefacts...

set LOADER_SRC=!OUT!\Loader.efi
set DRIVER_SRC=!OUT!\MemDriver.efi

set MISSING=
if not exist "!LOADER_SRC!" set "MISSING=!MISSING! Loader.efi(!LOADER_SRC!)"
if not exist "!DRIVER_SRC!" set "MISSING=!MISSING! MemDriver.efi(!DRIVER_SRC!)"
if not "!MISSING!"=="" (
    echo [!] Build artefact missing:!MISSING!
    exit /b 1
)
echo     Loader.efi    : !LOADER_SRC!
echo     MemDriver.efi : !DRIVER_SRC!

set USB_BOOT=%USB%\EFI\Boot
set USB_LOADER=%USB_BOOT%\bootx64.efi
set USB_DRIVER=%USB%\MemDriver.efi

echo.
echo [*] Deploying to %USB%...
if not exist "%USB_BOOT%" mkdir "%USB_BOOT%" >nul 2>&1
if not exist "%USB_BOOT%" (
    echo [!] Could not create %USB_BOOT%
    exit /b 1
)

REM Loader.efi -> bootx64.efi (that's the UEFI boot-entry name)
copy /y "!LOADER_SRC!" "%USB_LOADER%" >nul
if errorlevel 1 ( echo [!] Failed to copy Loader.efi -^> bootx64.efi & exit /b 1 )
echo     [+] Loader.efi    -^> %USB_LOADER%

copy /y "!DRIVER_SRC!" "%USB_DRIVER%" >nul
if errorlevel 1 ( echo [!] Failed to copy MemDriver.efi & exit /b 1 )
echo     [+] MemDriver.efi -^> %USB_DRIVER%

echo.
echo ============================================
echo   USB layout (%USB%):
echo.
echo     EFI\Boot\bootx64.efi    (Loader - auto-loads MemDriver then chains to Windows)
echo     MemDriver.efi            (runtime driver - per-process PML4 scope)
echo ============================================
echo.
echo [+] Done.
echo     1. Reboot with Secure Boot disabled, boot from %USB%
echo     2. The loader auto-starts the driver and chains to Windows

endlocal
