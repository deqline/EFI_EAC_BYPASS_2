@echo off
REM Build the EFI memory client (Windows user-mode)
REM Requires Visual Studio 2022 Build Tools

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
)

cl /EHsc /W4 /std:c++17 /O2 client.cpp /Fe:efi_client.exe /link advapi32.lib

if exist "efi_client.exe" (
    echo [+] Built: efi_client.exe
) else (
    echo [!] Build failed.
)
