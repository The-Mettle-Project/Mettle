@echo off
REM Build Mettle physics_grid benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building physics_grid.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\physics_grid\physics_grid.mettle -o examples\physics_grid\physics_grid.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\physics_grid\physics_grid_c.exe examples\physics_grid\physics_grid.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\physics_grid\physics_grid.exe
echo   C:       examples\physics_grid\physics_grid_c.exe
