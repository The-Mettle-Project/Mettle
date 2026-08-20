@echo off
REM Build examples/raylib/sphere.mettle against raylib with the internal PE linker.
REM
REM raylib path resolution (first match wins):
REM   1. First argument:   build.bat "C:\path\to\raylib\bin"
REM   2. Environment:      set RAYLIB_DIR=C:\path\to\raylib\bin
REM   3. libraylib.dll or raylib.dll already on PATH
REM
REM The linker imports by probing the DLL's export table, so only the DLL is
REM needed: no import library, no C toolchain, no C runtime.

setlocal EnableExtensions
set APP=%~dp0
set ROOT=%APP%..\..
set OUT=examples\raylib\sphere.exe

cd /d "%ROOT%"
if errorlevel 1 (
  echo ERROR: Could not cd to repository root: %ROOT%
  exit /b 1
)

if not "%~1"=="" set "RAYLIB_DIR=%~1"
if not "%RAYLIB_DIR%"=="" set "PATH=%RAYLIB_DIR%;%PATH%"

set RAYLIB_NAME=
for %%D in (libraylib.dll) do if not "%%~$PATH:D"=="" set RAYLIB_NAME=libraylib
if "%RAYLIB_NAME%"=="" for %%D in (raylib.dll) do if not "%%~$PATH:D"=="" set RAYLIB_NAME=raylib

if "%RAYLIB_NAME%"=="" (
  echo ERROR: raylib.dll / libraylib.dll not found on PATH.
  echo Pass the directory holding it: examples\raylib\build.bat "C:\path\to\raylib\bin"
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

echo Building %OUT% against %RAYLIB_NAME%.dll ...
bin\mettle.exe --build --release examples\raylib\sphere.mettle -o %OUT% --link-arg -l%RAYLIB_NAME%
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
echo Run it:            %OUT%
echo Benchmark 10s:     %OUT% --bench=10
echo.
exit /b 0
