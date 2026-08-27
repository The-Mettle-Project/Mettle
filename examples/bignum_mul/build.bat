@echo off
REM Build Mettle bignum_mul benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building bignum_mul.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\bignum_mul\bignum_mul.mettle -o examples\bignum_mul\bignum_mul.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\bignum_mul\bignum_mul_c.exe examples\bignum_mul\bignum_mul.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\bignum_mul\bignum_mul.exe
echo   C:       examples\bignum_mul\bignum_mul_c.exe
