@echo off
REM Build examples/ui_demo/ui_demo.mettle with the internal PE linker (user32 + gdi32).
setlocal EnableExtensions
set APP=%~dp0
set ROOT=%APP%..\..
set OUT=examples\ui_demo\ui_demo.exe
set DIR_HELPERS=examples\ui_demo\dir_helpers.o

cd /d "%ROOT%"
if errorlevel 1 (
  echo ERROR: Could not cd to repository root: %ROOT%
  exit /b 1
)

if not exist bin\mettle.exe (
  echo Building Mettle compiler...
  call build.bat
  if errorlevel 1 (
    echo ERROR: Mettle compiler build failed.
    exit /b 1
  )
)

if not exist bin\mettle.exe (
  echo ERROR: Mettle compiler not found: bin\mettle.exe
  exit /b 1
)

echo Compiling stdlib\dir_helpers.c...
where gcc >nul 2>&1
if errorlevel 1 (
  echo ERROR: gcc is required to compile stdlib\dir_helpers.c
  exit /b 1
)
gcc -c stdlib\dir_helpers.c -o %DIR_HELPERS%
if errorlevel 1 (
  echo ERROR: dir_helpers.c compile failed.
  exit /b 1
)

echo Building %OUT% with mettle --build --linker internal...
bin\mettle.exe --build --linker internal -s examples\ui_demo\ui_demo.mettle -o %OUT% --link-arg %DIR_HELPERS%
if errorlevel 1 (
  echo ERROR: Mettle build failed.
  exit /b 1
)

if not exist "%OUT%" (
  echo ERROR: Build finished but executable was not created: %OUT%
  exit /b 1
)

echo.
echo Built %OUT%
echo Run from the repository root: %OUT%
echo.
exit /b 0
