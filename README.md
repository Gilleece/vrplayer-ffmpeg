# vrplayer-ffmpeg

The complete source of `libffmpegJNI.so`, the audio and video decoder library that the Realtime
3D Player VR app loads at runtime, published to satisfy the GNU Lesser General Public License
version 2.1 (section 6) under which FFmpeg is licensed. The app itself is closed source; this
library is a separate work, built with no GPL or non-free parts, and may be replaced by a
compatible build of the same library.

## What is here

- `ffmpeg-6.1.6-9266ee0.tar.gz`: FFmpeg, tag `n6.1.6` on branch `release/6.1` at commit
  `9266ee001cb04b9dd6ff0335022bd986e2bc8e8b` (RELEASE file 6.1.6), unmodified, as a
  `git archive` of that commit from https://git.ffmpeg.org/ffmpeg.git. `fetch-ffmpeg.sh`
  unpacks it into `ffmpeg/` (or clones the same commit when the tarball is absent).
  FFmpeg is LGPL-2.1-or-later (`LICENSE`, which is FFmpeg's `COPYING.LGPLv2.1`).
- `module/`: the JNI wrapper and build script, from `libraries/decoder_ffmpeg` of
  https://github.com/androidx/media at tag 1.11.0 (commit `2bc20785`), Apache-2.0
  (`module/LICENSE`). `build_ffmpeg.sh` enables the h264 parser and `CMakeLists.txt` builds
  the video JNI too. `ffmpeg_jni.cc` carries two changes: TrueHD samples are split into access
  units with an `AVCodecParserContext` before decoding, and Windows Media Audio gets its sample
  rate, channels, block align and bit rate from the container. `ffmpeg_video_jni.cc`, the
  video decoder for 10 bit H.264, MPEG-4 Part 2, WMV and VC-1, is the app owner's. All
  described in `BUILD.md`.
- `BUILD.md`: the exact build recipe, toolchain versions, decoder list and the configure line
  that produced the shipped library.

## Building

```
./fetch-ffmpeg.sh
cd module
./build_ffmpeg.sh "$(pwd)" "$ANDROID_NDK" darwin-x86_64 29 \
    ac3 eac3 dca truehd mlp flac alac vorbis opus pcm_s16le pcm_s24le h264 mpeg4 msmpeg4v2 msmpeg4v3
```

with `module/ffmpeg` a link or copy of `../ffmpeg`, then CMake through the module's
`CMakeLists.txt` with the Android NDK toolchain for `arm64-v8a`, as described in `BUILD.md`.
The result is `libffmpegJNI.so`, which links `libavcodec`, `libavutil` and `libswresample`
statically and is the library the app ships.
