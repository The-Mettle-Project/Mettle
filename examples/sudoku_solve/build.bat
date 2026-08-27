@echo off
REM Build Mettle sudoku_solve benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building sudoku_solve.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\sudoku_solve\sudoku_solve.mettle -o examples\sudoku_solve\sudoku_solve.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\sudoku_solve\sudoku_solve_c.exe examples\sudoku_solve\sudoku_solve.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\sudoku_solve\sudoku_solve.exe
echo   C:       examples\sudoku_solve\sudoku_solve_c.exe
