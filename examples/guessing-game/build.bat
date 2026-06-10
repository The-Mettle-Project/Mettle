@echo off
REM Build Mettle Number Guessing Game
set APP=%~dp0
set ROOT=%APP%..\..
cd /d "%ROOT%"

if not exist bin\mettle.exe (
    echo Building Mettle compiler...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

echo Building guessing_game.mettle...
bin\mettle.exe --build examples\guessing-game\guessing_game.mettle -o examples\guessing-game\guessing_game.exe
if %ERRORLEVEL% NEQ 0 (
    echo Mettle build failed.
    exit /b 1
)

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build successful! Run: examples\guessing-game\guessing_game.exe
) else (
    echo Link failed.
    exit /b 1
)
