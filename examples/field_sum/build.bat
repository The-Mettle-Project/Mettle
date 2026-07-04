@echo off
REM Build Mettle field_sum benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building field_sum.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\field_sum\field_sum.mettle -o examples\field_sum\field_sum.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\field_sum\field_sum_c.exe examples\field_sum\field_sum.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\field_sum\field_sum.exe
echo   C:       examples\field_sum\field_sum_c.exe
