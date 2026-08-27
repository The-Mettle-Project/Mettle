@echo off
REM Build Mettle btree_index benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building btree_index.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\btree_index\btree_index.mettle -o examples\btree_index\btree_index.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\btree_index\btree_index_c.exe examples\btree_index\btree_index.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\btree_index\btree_index.exe
echo   C:       examples\btree_index\btree_index_c.exe
