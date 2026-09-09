#!/bin/sh
# Unpacks, or fetches, the exact FFmpeg source libffmpegJNI.so is built from.
set -eu
cd "$(dirname "$0")"
if [ -f ffmpeg-6.0.2-3f92512.tar.gz ]; then
    tar xzf ffmpeg-6.0.2-3f92512.tar.gz
else
    git clone --branch release/6.0 https://git.ffmpeg.org/ffmpeg.git ffmpeg
    git -C ffmpeg checkout 3f92512fd1fd6f5e6d6eb45a156c352835314d69
fi
