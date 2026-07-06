@echo off
REM Build Mettle crc32 benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building crc32.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\crc32\crc32.mettle -o examples\crc32\crc32.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\crc32\crc32_c.exe examples\crc32\crc32.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\crc32\crc32.exe
echo   C:       examples\crc32\crc32_c.exe
