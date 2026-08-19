@echo off
REM Build Mettle word_freq benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building word_freq.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\word_freq\word_freq.mettle -o examples\word_freq\word_freq.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\word_freq\word_freq_c.exe examples\word_freq\word_freq.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\word_freq\word_freq.exe
echo   C:       examples\word_freq\word_freq_c.exe
