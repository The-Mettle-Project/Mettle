@echo off
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

bin\mettle.exe --build --emit-obj --linker internal --release examples\dot_product\dot_product.mettle -o examples\dot_product\dot_product.exe
if %ERRORLEVEL% NEQ 0 exit /b 1

gcc -O3 -fopt-info -o examples\dot_product\dot_product_c.exe examples\dot_product\dot_product.c -lkernel32
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Built examples\dot_product\dot_product.exe and dot_product_c.exe
