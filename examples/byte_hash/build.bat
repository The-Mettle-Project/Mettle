@echo off
REM Build Mettle byte-hash benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building byte_hash.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\byte_hash\byte_hash.mettle -o examples\byte_hash\byte_hash.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\byte_hash\byte_hash_c.exe examples\byte_hash\byte_hash.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\byte_hash\byte_hash.exe
echo   C:       examples\byte_hash\byte_hash_c.exe
