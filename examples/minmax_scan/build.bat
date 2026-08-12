@echo off
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"
if not exist bin\mettle.exe ( call build.bat & if %ERRORLEVEL% NEQ 0 exit /b 1 )
bin\mettle.exe --build --emit-obj --linker internal --release examples\minmax_scan\minmax_scan.mettle -o examples\minmax_scan\minmax_scan.exe
if %ERRORLEVEL% NEQ 0 exit /b 1
gcc -O3 -o examples\minmax_scan\minmax_scan_c.exe examples\minmax_scan\minmax_scan.c -lkernel32
if %ERRORLEVEL% NEQ 0 exit /b 1
