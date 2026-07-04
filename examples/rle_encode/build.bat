@echo off
REM Build Mettle rle_encode benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building rle_encode.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\rle_encode\rle_encode.mettle -o examples\rle_encode\rle_encode.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\rle_encode\rle_encode_c.exe examples\rle_encode\rle_encode.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\rle_encode\rle_encode.exe
echo   C:       examples\rle_encode\rle_encode_c.exe
