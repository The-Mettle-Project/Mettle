@echo off
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

bin\mettle.exe --build --emit-obj --linker internal --release examples\binary_search\binary_search.mettle -o examples\binary_search\binary_search.exe
if %ERRORLEVEL% NEQ 0 exit /b 1

gcc -O3 -o examples\binary_search\binary_search_c.exe examples\binary_search\binary_search.c -lkernel32 -fno-trapping-math -fno-rounding-math
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Built examples\binary_search\binary_search.exe and binary_search_c.exe
