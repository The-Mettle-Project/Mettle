@echo off
REM Build Mettle linked_list_sum benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building linked_list_sum.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\linked_list_sum\linked_list_sum.mettle -o examples\linked_list_sum\linked_list_sum.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\linked_list_sum\linked_list_sum_c.exe examples\linked_list_sum\linked_list_sum.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\linked_list_sum\linked_list_sum.exe
echo   C:       examples\linked_list_sum\linked_list_sum_c.exe
