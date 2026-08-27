@echo off
REM Build Mettle diff_lcs benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building diff_lcs.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\diff_lcs\diff_lcs.mettle -o examples\diff_lcs\diff_lcs.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\diff_lcs\diff_lcs_c.exe examples\diff_lcs\diff_lcs.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\diff_lcs\diff_lcs.exe
echo   C:       examples\diff_lcs\diff_lcs_c.exe
