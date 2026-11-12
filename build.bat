@echo off
rem ===================================================================
rem  NetSwitch build script  (MinGW-w64, any variant incl. UCRT builds)
rem  Output: NetSwitch.exe  -- no CRT / api-ms-win-crt dependency,
rem          subsystem 6.01 so it starts on Windows 7.
rem ===================================================================
setlocal enabledelayedexpansion

set MINGW=C:\mingw64\bin
if not exist "%MINGW%\gcc.exe" set MINGW=

if "%MINGW%"=="" (
    set GCC=gcc
    set RC=windres
) else (
    set GCC=%MINGW%\gcc.exe
    set RC=%MINGW%\windres.exe
)

"%GCC%" --version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] gcc not found. Install MinGW-w64 or add its bin folder to PATH.
    exit /b 1
)

echo [1/2] compiling resources ...
"%RC%" NetSwitch.rc -O coff -o NetSwitch.res.o
if errorlevel 1 exit /b 1

echo [2/2] compiling and linking ...
"%GCC%" -O2 -s -mwindows -ffreestanding -fno-builtin -fno-stack-protector ^
    -nostdlib -nostartfiles -nodefaultlibs -Wl,-e,AppEntry ^
    -Wl,--major-subsystem-version,6 -Wl,--minor-subsystem-version,1 ^
    -o NetSwitch.exe NetSwitch.c NetSwitch.res.o ^
    -lkernel32 -luser32 -lgdi32 -ladvapi32 -lshell32 -lsetupapi -liphlpapi -lcfgmgr32
if errorlevel 1 exit /b 1

del /q NetSwitch.res.o >nul 2>&1
echo.
echo Done. Output: NetSwitch.exe
echo Verify imports with:  objdump -p NetSwitch.exe ^| findstr "DLL Name"
