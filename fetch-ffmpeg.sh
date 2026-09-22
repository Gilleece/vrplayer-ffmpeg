#!/bin/sh
# Unpacks, or fetches, the exact FFmpeg source libffmpegJNI.so is built from.
set -eu
cd "$(dirname "$0")"
if [ -f ffmpeg-6.1.6-9266ee0.tar.gz ]; then
    tar xzf ffmpeg-6.1.6-9266ee0.tar.gz
else
    git clone --branch release/6.1 https://git.ffmpeg.org/ffmpeg.git ffmpeg
    git -C ffmpeg checkout 9266ee001cb04b9dd6ff0335022bd986e2bc8e8b
fi
