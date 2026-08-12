@echo off
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"
if not exist bin\mettle.exe ( call build.bat & if %ERRORLEVEL% NEQ 0 exit /b 1 )
bin\mettle.exe --build --emit-obj --linker internal --release examples\prefix_sum\prefix_sum.mettle -o examples\prefix_sum\prefix_sum.exe
if %ERRORLEVEL% NEQ 0 exit /b 1
gcc -O3 -o examples\prefix_sum\prefix_sum_c.exe examples\prefix_sum\prefix_sum.c -lkernel32
if %ERRORLEVEL% NEQ 0 exit /b 1
