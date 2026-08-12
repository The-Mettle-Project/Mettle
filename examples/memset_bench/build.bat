@echo off
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

bin\mettle.exe --build --emit-obj --linker internal --release examples\memset_bench\memset_bench.mettle -o examples\memset_bench\memset_bench.exe
if %ERRORLEVEL% NEQ 0 exit /b 1

gcc -O3 -o examples\memset_bench\memset_bench_c.exe examples\memset_bench\memset_bench.c -lkernel32
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Built examples\memset_bench\memset_bench.exe and memset_bench_c.exe
