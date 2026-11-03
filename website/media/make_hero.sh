#!/bin/sh
set -e
cd "$(dirname "$0")"
curl -sfL -A 'Mozilla/5.0' -o hero-moss.mp4 "https://videos.pexels.com/video-files/16296843/16296843-uhd_3840_2160_24fps.mp4"
curl -sfL -A 'Mozilla/5.0' -o hero-galaxy.mp4 "https://videos.pexels.com/video-files/27254149/12103118_3840_2160_24fps.mp4"
curl -sfL -A 'Mozilla/5.0' -o hero-lava.mp4 "https://videos.pexels.com/video-files/30401545/13028933_1920_1080_60fps.mp4"
ffmpeg -v error -y -t 10.4 -i hero-moss.mp4 -ss 12.5 -t 8 -i hero-galaxy.mp4 -ss 26 -t 10 -i hero-lava.mp4 -an -filter_complex "\
[0:v]fps=24,scale=1280:720:flags=lanczos,eq=contrast=1.03:gamma=0.97,format=yuv420p,setpts=PTS-STARTPTS[moss];\
[1:v]fps=24,vflip,crop=2560:1440:1280:0,scale=1280:720:flags=lanczos,curves=all='0/0 0.12/0.05 0.45/0.65 1/1',eq=saturation=1.6,format=yuv420p,setpts=PTS-STARTPTS[galaxy];\
[2:v]fps=24,crop=1600:900:200:0,scale=1280:720:flags=lanczos,eq=gamma=1.1:saturation=1.1,format=yuv420p,setpts=PTS-STARTPTS[lava];\
[moss][galaxy][lava]concat=n=3:v=1[out]" -map "[out]" -c:v libx264 -preset slow -crf 27 -x264-params aq-mode=3:aq-strength=1.0 -pix_fmt yuv420p -movflags +faststart hero.mp4
ffmpeg -v error -y -i hero.mp4 -frames:v 1 -q:v 6 hero-poster.jpg
ffmpeg -v error -y -ss 5 -i hero-moss.mp4 -frames:v 1 -vf scale=2400:-2,eq=contrast=1.03:gamma=0.97 -q:v 3 card-frame.jpg
rm -f hero-moss.mp4 hero-galaxy.mp4 hero-lava.mp4
