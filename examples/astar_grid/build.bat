@echo off
REM Build Mettle astar_grid benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building astar_grid.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\astar_grid\astar_grid.mettle -o examples\astar_grid\astar_grid.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\astar_grid\astar_grid_c.exe examples\astar_grid\astar_grid.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\astar_grid\astar_grid.exe
echo   C:       examples\astar_grid\astar_grid_c.exe
