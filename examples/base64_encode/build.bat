@echo off
REM Build Mettle base64_encode benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building base64_encode.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\base64_encode\base64_encode.mettle -o examples\base64_encode\base64_encode.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\base64_encode\base64_encode_c.exe examples\base64_encode\base64_encode.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\base64_encode\base64_encode.exe
echo   C:       examples\base64_encode\base64_encode_c.exe
