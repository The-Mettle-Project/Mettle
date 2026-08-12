@echo off
REM Build distinct-type example with compiler profiling
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building distinct.mettle with --profile...
bin\mettle.exe --profile --build --emit-obj --linker internal examples\distinct\distinct.mettle -o examples\distinct\distinct.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Build successful! Running distinct.exe...
examples\distinct\distinct.exe
if %ERRORLEVEL% NEQ 0 (
    echo Program returned non-zero exit code.
    exit /b 1
)

echo.
echo All checks passed (exit code 0).
