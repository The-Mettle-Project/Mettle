@echo off
REM Build Mettle link_resolve benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building link_resolve.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\link_resolve\link_resolve.mettle -o examples\link_resolve\link_resolve.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\link_resolve\link_resolve_c.exe examples\link_resolve\link_resolve.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\link_resolve\link_resolve.exe
echo   C:       examples\link_resolve\link_resolve_c.exe
