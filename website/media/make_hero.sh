#!/bin/sh
set -e
cd "$(dirname "$0")"
curl -sfL -A 'Mozilla/5.0' -o hero-source.mp4 "https://videos.pexels.com/video-files/36374734/15427153_3840_2160_30fps.mp4"
ffmpeg -v error -y -ss 3 -t 7 -i hero-source.mp4 -an -filter_complex "fps=30,scale=2560:1440:flags=lanczos,setpts='if(lt(N/30,0.3),sqrt(1.2*N/30),if(lt(N/30,6.7),0.6+(N/30-0.3),7.6-sqrt(1.2*max(0,7-N/30))))/TB',fps=30,eq=contrast=1.05:saturation=1.06:gamma=0.97,format=yuv420p,split[f][g];[g]reverse[r];[f][r]concat=n=2:v=1" -c:v libx264 -preset slow -crf 25 -x264-params aq-mode=3:aq-strength=1.0 -pix_fmt yuv420p -movflags +faststart hero.mp4
ffmpeg -v error -y -i hero.mp4 -frames:v 1 -vf scale=1600:-2 -q:v 5 hero-poster.jpg
rm -f hero-source.mp4
