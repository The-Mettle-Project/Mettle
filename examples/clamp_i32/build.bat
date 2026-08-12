@echo off
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"
if not exist bin\mettle.exe ( call build.bat & if %ERRORLEVEL% NEQ 0 exit /b 1 )
bin\mettle.exe --build --emit-obj --linker internal --release examples\clamp_i32\clamp_i32.mettle -o examples\clamp_i32\clamp_i32.exe
if %ERRORLEVEL% NEQ 0 exit /b 1
gcc -O3 -o examples\clamp_i32\clamp_i32_c.exe examples\clamp_i32\clamp_i32.c -lkernel32
if %ERRORLEVEL% NEQ 0 exit /b 1
