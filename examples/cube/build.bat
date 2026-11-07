@echo off
setlocal EnableExtensions
set APP=%~dp0
set ROOT=%APP%..\..

cd /d "%ROOT%"
if errorlevel 1 (
  echo ERROR: Could not cd to repository root: %ROOT%
  exit /b 1
)

if not exist bin\mettle.exe (
  call build.bat --skip-tests
  if errorlevel 1 exit /b 1
)

bin\mettle.exe -O --build --release --linker internal --subsystem windows examples\cube\cube.mettle -o examples\cube\cube.exe
if errorlevel 1 exit /b 1

echo Built examples\cube\cube.exe
exit /b 0
