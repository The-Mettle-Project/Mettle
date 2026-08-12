@echo off
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

bin\mettle.exe --build --emit-obj --linker internal --release examples\memcpy_bench\memcpy_bench.mettle -o examples\memcpy_bench\memcpy_bench.exe
if %ERRORLEVEL% NEQ 0 exit /b 1

gcc -O3 -o examples\memcpy_bench\memcpy_bench_c.exe examples\memcpy_bench\memcpy_bench.c -lkernel32
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Built examples\memcpy_bench\memcpy_bench.exe and memcpy_bench_c.exe
