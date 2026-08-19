@echo off
REM Build Mettle interp_ast benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building interp_ast.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\interp_ast\interp_ast.mettle -o examples\interp_ast\interp_ast.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\interp_ast\interp_ast_c.exe examples\interp_ast\interp_ast.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\interp_ast\interp_ast.exe
echo   C:       examples\interp_ast\interp_ast_c.exe
