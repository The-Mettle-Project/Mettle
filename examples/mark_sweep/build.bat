@echo off
REM Build Mettle mark_sweep benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building mark_sweep.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\mark_sweep\mark_sweep.mettle -o examples\mark_sweep\mark_sweep.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\mark_sweep\mark_sweep_c.exe examples\mark_sweep\mark_sweep.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\mark_sweep\mark_sweep.exe
echo   C:       examples\mark_sweep\mark_sweep_c.exe
