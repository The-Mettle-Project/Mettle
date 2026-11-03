#!/bin/sh
set -e
cd "$(dirname "$0")"
curl -sfL -A 'Mozilla/5.0' -o hero-source.mp4 "https://videos.pexels.com/video-files/16296843/16296843-uhd_3840_2160_24fps.mp4"
ffmpeg -v error -y -t 10.4 -i hero-source.mp4 -an -filter_complex "fps=24,scale=1280:720:flags=lanczos,setpts='if(lt(N/24,0.4),sqrt(1.6*N/24),if(lt(N/24,10),0.8+(N/24-0.4),11.2-sqrt(1.6*max(0,10.4-N/24))))/TB',fps=24,eq=contrast=1.03:gamma=0.97,format=yuv420p,split[f][g];[g]reverse[r];[f][r]concat=n=2:v=1" -c:v libx264 -preset slow -crf 27 -x264-params aq-mode=3:aq-strength=1.0 -pix_fmt yuv420p -movflags +faststart hero.mp4
ffmpeg -v error -y -i hero-source.mp4 -frames:v 1 -vf scale=1600:-2,eq=contrast=1.03:gamma=0.97 -q:v 6 hero-poster.jpg
ffmpeg -v error -y -ss 5 -i hero-source.mp4 -frames:v 1 -vf scale=2400:-2,eq=contrast=1.03:gamma=0.97 -q:v 3 card-frame.jpg
rm -f hero-source.mp4
