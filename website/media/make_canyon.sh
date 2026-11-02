#!/bin/sh
set -e
cd "$(dirname "$0")"
curl -sfL -A 'Mozilla/5.0' -o canyon-source.mp4 "https://videos.pexels.com/video-files/34305686/14533316_2560_1440_60fps.mp4"
ffmpeg -v error -y -ss 2.5 -t 4 -i canyon-source.mp4 -an -filter_complex "fps=60,scale=1920:1080:flags=lanczos,setpts='if(lt(N/60,0.2),sqrt(3.2*N/60),if(lt(N/60,3.8),0.8+(N/60-0.2)/0.5,8.8-sqrt(3.2*max(0,4-N/60))))/TB',fps=30,eq=gamma=0.68:saturation=1.12:contrast=1.06,format=yuv420p,split[f][g];[g]reverse[r];[f][r]concat=n=2:v=1" -c:v libx264 -preset slow -crf 25 -x264-params aq-mode=3:aq-strength=1.0 -pix_fmt yuv420p -movflags +faststart canyon.mp4
ffmpeg -v error -y -i canyon.mp4 -frames:v 1 -vf scale=1600:-2 -q:v 5 canyon-poster.jpg
rm -f canyon-source.mp4
