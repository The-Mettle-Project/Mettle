@echo off
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"
if not exist bin\mettle.exe ( call build.bat & if %ERRORLEVEL% NEQ 0 exit /b 1 )
bin\mettle.exe --build --emit-obj --linker internal --release examples\popcount\popcount.mettle -o examples\popcount\popcount.exe
if %ERRORLEVEL% NEQ 0 exit /b 1
gcc -O3 -o examples\popcount\popcount_c.exe examples\popcount\popcount.c -lkernel32
if %ERRORLEVEL% NEQ 0 exit /b 1
