#!/bin/sh
# Unpacks, or fetches, the exact FFmpeg source libffmpegJNI.so is built from.
set -eu
cd "$(dirname "$0")"
if [ -f ffmpeg-9.0.2-946fcce.tar.gz ]; then
    tar xzf ffmpeg-9.0.2-946fcce.tar.gz
else
    git clone --branch release/9.0 https://git.ffmpeg.org/ffmpeg.git ffmpeg
    git -C ffmpeg checkout 946fcce07b6dcd0331c8cc609192aeff5e1924f8
fi
