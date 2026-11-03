#!/bin/sh
set -e
cd "$(dirname "$0")"
curl -sfL -A 'Mozilla/5.0' -o hero-moss.mp4 "https://videos.pexels.com/video-files/16296843/16296843-uhd_3840_2160_24fps.mp4"
curl -sfL -A 'Mozilla/5.0' -o hero-snow.mp4 "https://videos.pexels.com/video-files/28638515/12438508_1920_1080_24fps.mp4"
curl -sfL -A 'Mozilla/5.0' -o hero-canyon.mp4 "https://videos.pexels.com/video-files/28543610/12415311_1920_1080_30fps.mp4"
ffmpeg -v error -y -t 10.4 -i hero-moss.mp4 -ss 1 -t 8 -i hero-snow.mp4 -ss 5 -t 10 -i hero-canyon.mp4 -an -filter_complex "\
[0:v]fps=24,scale=1280:720:flags=lanczos,eq=contrast=1.03:gamma=0.97,format=yuv420p,setpts=PTS-STARTPTS[moss];\
[1:v]fps=24,crop=1536:864:192:40,scale=1280:720:flags=lanczos,curves=all='0/0 0.55/0.10 0.82/0.62 1/1',colorbalance=bs=0.10:bm=0.04,format=yuv420p,setpts=PTS-STARTPTS[snow];\
[2:v]fps=24,scale=1280:720:flags=lanczos,curves=all='0/0 0.5/0.16 0.8/0.62 1/0.9',eq=saturation=1.2,format=yuv420p,setpts=PTS-STARTPTS[canyon];\
[moss][snow][canyon]concat=n=3:v=1[out]" -map "[out]" -c:v libx264 -preset slow -crf 27 -x264-params aq-mode=3:aq-strength=1.0 -pix_fmt yuv420p -movflags +faststart hero.mp4
ffmpeg -v error -y -i hero.mp4 -frames:v 1 -q:v 6 hero-poster.jpg
ffmpeg -v error -y -ss 5 -i hero-moss.mp4 -frames:v 1 -vf scale=2400:-2,eq=contrast=1.03:gamma=0.97 -q:v 3 card-frame.jpg
rm -f hero-moss.mp4 hero-snow.mp4 hero-canyon.mp4
