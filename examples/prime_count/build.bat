@echo off
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

bin\mettle.exe --build --emit-obj --linker internal --release examples\prime_count\prime_count.mettle -o examples\prime_count\prime_count.exe
if %ERRORLEVEL% NEQ 0 exit /b 1

gcc -O3 -o examples\prime_count\prime_count_c.exe examples\prime_count\prime_count.c -lkernel32
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Build successful: examples\prime_count\prime_count.exe
