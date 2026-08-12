@echo off
REM Build examples/tracy_demo/tracy_demo.mettle with Tracy instrumentation.
REM
REM Tracy path resolution (first match wins):
REM   1. First argument:  build.bat "C:\path\to\tracy"
REM   2. Environment:     set TRACY_DIR=C:\path\to\tracy
REM   3. Saved config:    examples\tracy_demo\tracy_dir.local.bat
REM   4. Interactive prompt (saved to tracy_dir.local.bat for next run)
REM
REM Mettle also persists the resolved path to .mettle\tracy_dir in the repo root.
REM
REM 1. Start Tracy Profiler
REM 2. Run examples\tracy_demo\tracy_demo.exe
REM 3. Connect to the process

setlocal EnableExtensions
set APP=%~dp0
set ROOT=%APP%..\..
set CONFIG=%APP%tracy_dir.local.bat
set OUT=examples\tracy_demo\tracy_demo.exe

cd /d "%ROOT%"
if errorlevel 1 call :fail Could not cd to repository root: %ROOT%

if not "%~1"=="" set "TRACY_DIR=%~1"
if not "%~1"=="" call :save_tracy_dir

if "%TRACY_DIR%"=="" if exist "%CONFIG%" call "%CONFIG%"

if "%TRACY_DIR%"=="" call :prompt_tracy_dir

call :validate_tracy_dir "%TRACY_DIR%"
if errorlevel 1 exit /b 1

if not exist bin\mettle.exe (
  echo Building Mettle compiler...
  call build.bat
  if errorlevel 1 call :fail Mettle compiler build failed.
)

if not exist bin\mettle.exe call :fail Mettle compiler not found: bin\mettle.exe

echo Building %OUT% with mettle --build --tracy...
echo TRACY_DIR=%TRACY_DIR%

bin\mettle.exe --build --tracy --tracy-dir "%TRACY_DIR%" examples\tracy_demo\tracy_demo.mettle -o %OUT%
if errorlevel 1 call :fail Mettle build failed.

if not exist "%OUT%" call :fail Build finished but executable was not created: %OUT%

echo.
echo Built %OUT%
echo   Tracy source: %TRACY_DIR%
echo   Saved path:   %CONFIG%
echo   1. Start Tracy Profiler
echo   2. Run: %OUT%
echo   3. Connect when prompted
echo.
exit /b 0

:prompt_tracy_dir
echo.
echo Tracy source directory is not configured yet.
echo Enter the Tracy repo root - the folder that contains public\tracy\TracyC.h
echo Example: G:\Projects\tracy-mettle\tracy
echo.
set "TRACY_DIR="
set /p "TRACY_DIR=TRACY_DIR> "
if "%TRACY_DIR%"=="" call :fail No Tracy directory entered.

if "%TRACY_DIR:~-1%"=="\" set "TRACY_DIR=%TRACY_DIR:~0,-1%"

call :validate_tracy_dir "%TRACY_DIR%"
if errorlevel 1 goto :prompt_tracy_dir

call :save_tracy_dir
exit /b 0

:validate_tracy_dir
set "CHECK_DIR=%~1"
if "%CHECK_DIR%"=="" (
  echo ERROR: Tracy directory path is empty.
  exit /b 1
)
if not exist "%CHECK_DIR%" (
  echo ERROR: Directory does not exist:
  echo         "%CHECK_DIR%"
  exit /b 1
)
if not exist "%CHECK_DIR%\public\tracy\TracyC.h" (
  echo ERROR: Not a Tracy repo root - missing public\tracy\TracyC.h:
  echo         "%CHECK_DIR%"
  exit /b 1
)
if not exist "%CHECK_DIR%\public\TracyClient.cpp" (
  echo ERROR: Not a Tracy repo root - missing public\TracyClient.cpp:
  echo         "%CHECK_DIR%"
  exit /b 1
)
exit /b 0

:save_tracy_dir
> "%CONFIG%" echo @echo off
>> "%CONFIG%" echo REM Local Tracy path for examples\tracy_demo\build.bat - auto-generated, do not commit
>> "%CONFIG%" echo set "TRACY_DIR=%TRACY_DIR%"
echo Saved Tracy path to %CONFIG%
exit /b 0

:fail
echo.
echo ERROR: %*
echo.
exit /b 1
