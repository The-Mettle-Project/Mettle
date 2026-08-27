@echo off
REM Build Mettle life_bits benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building life_bits.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\life_bits\life_bits.mettle -o examples\life_bits\life_bits.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\life_bits\life_bits_c.exe examples\life_bits\life_bits.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\life_bits\life_bits.exe
echo   C:       examples\life_bits\life_bits_c.exe
