@echo off
REM Build Mettle bst_insert benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building bst_insert.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\bst_insert\bst_insert.mettle -o examples\bst_insert\bst_insert.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\bst_insert\bst_insert_c.exe examples\bst_insert\bst_insert.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\bst_insert\bst_insert.exe
echo   C:       examples\bst_insert\bst_insert_c.exe
