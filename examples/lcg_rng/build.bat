@echo off
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

bin\mettle.exe --build --emit-obj --linker internal --release examples\lcg_rng\lcg_rng.mettle -o examples\lcg_rng\lcg_rng.exe
if %ERRORLEVEL% NEQ 0 exit /b 1

gcc -O3 -o examples\lcg_rng\lcg_rng_c.exe examples\lcg_rng\lcg_rng.c -lkernel32
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Built examples\lcg_rng\lcg_rng.exe and lcg_rng_c.exe
