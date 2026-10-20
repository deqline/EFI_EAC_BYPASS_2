@echo off
setlocal

set EDK2_ROOT=C:\Users\admob\Documents\Hacks\Deceit\EFI\MemDriverPkg\edkII
set USB=E:

echo ============================================
echo   MemDriver EFI - Build and Deploy to USB
echo ============================================
echo.

REM --- Check USB ---
if not exist %USB%\ (
    echo [!] USB drive %USB% not found.
    echo     Insert your USB key and try again.
    exit /b 1
)
echo [+] USB drive %USB% found

REM --- VS2022 environment ---
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [!] VS2022 Build Tools not found.
    exit /b 1
)
echo [+] VS2022 environment loaded

REM --- NASM ---
set PATH=C:\NASM\nasm-2.16.03;%PATH%
set NASM_PREFIX=C:\NASM\nasm-2.16.03\
nasm --version >nul 2>&1
if errorlevel 1 (
    echo [!] NASM not found at C:\NASM\nasm-2.16.03
    exit /b 1
)
echo [+] NASM found

REM --- EDK2 setup ---
cd /d "%EDK2_ROOT%"
call "%EDK2_ROOT%\edksetup.bat" >nul 2>&1
echo [+] EDK2 workspace ready

REM --- Build ---
echo.
echo [*] Building MemDriver + Loader...
echo.
build -a X64 -t VS2022 -p MemDriverPkg/MemDriverPkg.dsc -b RELEASE
if errorlevel 1 (
    echo.
    echo [!] Build failed.
    exit /b 1
)

set OUT=%EDK2_ROOT%\Build\MemDriverPkg\RELEASE_VS2022\X64

if not exist "%OUT%\MemDriver.efi" (
    echo [!] MemDriver.efi not found in build output.
    exit /b 1
)
if not exist "%OUT%\Loader.efi" (
    echo [!] Loader.efi not found in build output.
    exit /b 1
)

echo.
echo [+] Build succeeded
echo     MemDriver.efi : %OUT%\MemDriver.efi
echo     Loader.efi    : %OUT%\Loader.efi

REM --- Deploy to USB ---
echo.
echo [*] Deploying to %USB%...

if not exist %USB%\EFI\Boot mkdir %USB%\EFI\Boot

copy /y "%OUT%\Loader.efi"    "%USB%\EFI\Boot\bootx64.efi" >nul
copy /y "%OUT%\MemDriver.efi" "%USB%\MemDriver.efi"        >nul

echo.
echo ============================================
echo   USB layout (%USB%):
echo.
echo     EFI\Boot\bootx64.efi   (Loader)
echo     MemDriver.efi           (DXE driver)
echo ============================================
echo.
echo [+] Done. Boot from USB to load the driver.

endlocal
