@echo off
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

bin\mettle.exe --build --emit-obj --linker internal --release examples\memcmp_bench\memcmp_bench.mettle -o examples\memcmp_bench\memcmp_bench.exe
if %ERRORLEVEL% NEQ 0 exit /b 1

gcc -O3 -o examples\memcmp_bench\memcmp_bench_c.exe examples\memcmp_bench\memcmp_bench.c -lkernel32
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Built examples\memcmp_bench\memcmp_bench.exe and memcmp_bench_c.exe
