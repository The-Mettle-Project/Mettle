@echo off
REM Build Mettle csv_query benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building csv_query.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\csv_query\csv_query.mettle -o examples\csv_query\csv_query.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\csv_query\csv_query_c.exe examples\csv_query\csv_query.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\csv_query\csv_query.exe
echo   C:       examples\csv_query\csv_query_c.exe
