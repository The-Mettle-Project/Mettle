@echo off
REM Build Mettle radix_sort benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building radix_sort.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\radix_sort\radix_sort.mettle -o examples\radix_sort\radix_sort.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\radix_sort\radix_sort_c.exe examples\radix_sort\radix_sort.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\radix_sort\radix_sort.exe
echo   C:       examples\radix_sort\radix_sort_c.exe
