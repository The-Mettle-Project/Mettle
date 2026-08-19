@echo off
REM Build Mettle json_parse benchmark
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building json_parse.mettle...
bin\mettle.exe --build --emit-obj --linker internal --release examples\json_parse\json_parse.mettle -o examples\json_parse\json_parse.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

echo.
echo Building C counterpart...
gcc -O3 -o examples\json_parse\json_parse_c.exe examples\json_parse\json_parse.c -lkernel32
if %ERRORLEVEL% NEQ 0 (
    echo C build failed.
    exit /b 1
)

echo.
echo Build successful!
echo   Mettle: examples\json_parse\json_parse.exe
echo   C:       examples\json_parse\json_parse_c.exe
