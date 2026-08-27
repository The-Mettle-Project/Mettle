@echo off
REM Build Mettle bytecode_vm benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building bytecode_vm.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\bytecode_vm\bytecode_vm.mettle -o examples\bytecode_vm\bytecode_vm.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\bytecode_vm\bytecode_vm_c.exe examples\bytecode_vm\bytecode_vm.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\bytecode_vm\bytecode_vm.exe
echo   C:       examples\bytecode_vm\bytecode_vm_c.exe
