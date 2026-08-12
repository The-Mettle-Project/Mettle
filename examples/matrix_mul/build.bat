@echo off
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

bin\mettle.exe --build --emit-obj --linker internal --release examples\matrix_mul\matrix_mul.mettle -o examples\matrix_mul\matrix_mul.exe
if %ERRORLEVEL% NEQ 0 exit /b 1

gcc -O3 -o examples\matrix_mul\matrix_mul_c.exe examples\matrix_mul\matrix_mul.c -lkernel32
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Build successful: examples\matrix_mul\matrix_mul.exe
