@echo off
REM Build examples/video_player with the internal PE linker (user32, gdi32, winmm).
setlocal EnableExtensions
set APP=%~dp0
set ROOT=%APP%..\..

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

echo Building examples\video_player\player.exe ...
bin\mettle.exe --build --release --linker internal --subsystem windows examples\video_player\player.mettle -o examples\video_player\player.exe
if errorlevel 1 (
  echo ERROR: player build failed.
  exit /b 1
)

echo Building examples\video_player\vptool.exe ...
bin\mettle.exe --build --release --linker internal examples\video_player\vptool.mettle -o examples\video_player\vptool.exe
if errorlevel 1 (
  echo ERROR: vptool build failed.
  exit /b 1
)

echo.
echo Built examples\video_player\player.exe and examples\video_player\vptool.exe
echo.
echo Make a playable file from any source video:
echo   ffmpeg -i input.mp4 -c:v mjpeg -q:v 5 -pix_fmt yuvj420p -c:a pcm_s16le -ar 44100 -ac 2 clip.avi
echo.
echo Then run:
echo   examples\video_player\player.exe clip.avi
echo.
exit /b 0
