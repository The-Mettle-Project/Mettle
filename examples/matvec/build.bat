@echo off
REM Build Mettle matvec benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building matvec.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\matvec\matvec.mettle -o examples\matvec\matvec.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\matvec\matvec_c.exe examples\matvec\matvec.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\matvec\matvec.exe
echo   C:       examples\matvec\matvec_c.exe
