@echo off
REM Build Mettle Hex Dump Utility
REM argc/argv startup is emitted directly through CRT __getmainargs.
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building hexdump.mettle...
bin\mettle.exe --build examples\hexdump\hexdump.mettle -o examples\hexdump\hexdump.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build successful! Run: hexdump.exe ^<filename^>
    echo Example: examples\hexdump\hexdump.exe examples\hexdump\hexdump.mettle
) else (
    echo Link failed.
    exit /b 1
)
