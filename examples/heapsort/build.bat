@echo off
REM Build Mettle heapsort benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building heapsort.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\heapsort\heapsort.mettle -o examples\heapsort\heapsort.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\heapsort\heapsort_c.exe examples\heapsort\heapsort.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\heapsort\heapsort.exe
echo   C:       examples\heapsort\heapsort_c.exe
