@echo off
REM Build Mettle voxel_trace benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building voxel_trace.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\voxel_trace\voxel_trace.mettle -o examples\voxel_trace\voxel_trace.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\voxel_trace\voxel_trace_c.exe examples\voxel_trace\voxel_trace.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\voxel_trace\voxel_trace.exe
echo   C:       examples\voxel_trace\voxel_trace_c.exe
