@echo off
REM Build Mettle huffman benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building huffman.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\huffman\huffman.mettle -o examples\huffman\huffman.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\huffman\huffman_c.exe examples\huffman\huffman.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\huffman\huffman.exe
echo   C:       examples\huffman\huffman_c.exe
