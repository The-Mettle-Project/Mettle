@echo off
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

bin\mettle.exe --build --emit-obj --linker internal --release examples\sort_insertion\sort_insertion.mettle -o examples\sort_insertion\sort_insertion.exe
if %ERRORLEVEL% NEQ 0 exit /b 1

gcc -O3 -o examples\sort_insertion\sort_insertion_c.exe examples\sort_insertion\sort_insertion.c -lkernel32
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Build successful: examples\sort_insertion\sort_insertion.exe
