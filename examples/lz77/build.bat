@echo off
REM Build Mettle lz77 benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building lz77.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\lz77\lz77.mettle -o examples\lz77\lz77.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\lz77\lz77_c.exe examples\lz77\lz77.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\lz77\lz77.exe
echo   C:       examples\lz77\lz77_c.exe
