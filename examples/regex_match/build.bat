@echo off
REM Build Mettle regex_match benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building regex_match.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\regex_match\regex_match.mettle -o examples\regex_match\regex_match.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\regex_match\regex_match_c.exe examples\regex_match\regex_match.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\regex_match\regex_match.exe
echo   C:       examples\regex_match\regex_match_c.exe
