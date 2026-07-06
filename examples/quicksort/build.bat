@echo off
REM Build Mettle quicksort benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building quicksort.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\quicksort\quicksort.mettle -o examples\quicksort\quicksort.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\quicksort\quicksort_c.exe examples\quicksort\quicksort.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\quicksort\quicksort.exe
echo   C:       examples\quicksort\quicksort_c.exe
