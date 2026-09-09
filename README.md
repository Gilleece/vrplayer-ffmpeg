# vrplayer-ffmpeg

The complete source of `libffmpegJNI.so`, the audio decoder library that the Realtime 3D
Player VR app loads at runtime, published to satisfy the GNU Lesser General Public License
version 2.1 (section 6) under which FFmpeg is licensed. The app itself is closed source; this
library is a separate work, built with no GPL or non-free parts, and may be replaced by a
compatible build of the same library.

## What is here

- `ffmpeg-6.0.2-3f92512.tar.gz`: FFmpeg, branch `release/6.0` at commit
  `3f92512fd1fd6f5e6d6eb45a156c352835314d69` (RELEASE file 6.0.2), unmodified, as a
  `git archive` of that commit from https://git.ffmpeg.org/ffmpeg.git. `fetch-ffmpeg.sh`
  unpacks it into `ffmpeg/` (or clones the same commit when the tarball is absent).
  FFmpeg is LGPL-2.1-or-later (`LICENSE`, which is FFmpeg's `COPYING.LGPLv2.1`).
- `module/`: the JNI wrapper and build script, from `libraries/decoder_ffmpeg` of
  https://github.com/androidx/media at tag 1.11.0 (commit `2bc20785`), Apache-2.0
  (`module/LICENSE`). `build_ffmpeg.sh` and `CMakeLists.txt` are verbatim. `ffmpeg_jni.cc`
  carries one change: TrueHD samples are split into access units with an
  `AVCodecParserContext` before decoding, described in `BUILD.md`.
- `BUILD.md`: the exact build recipe, toolchain versions, decoder list and the configure line
  that produced the shipped library.

## Building

```
./fetch-ffmpeg.sh
cd module
./build_ffmpeg.sh "$(pwd)" "$ANDROID_NDK" darwin-x86_64 29 \
    ac3 eac3 dca truehd mlp flac alac vorbis opus pcm_s16le pcm_s24le
```

with `module/ffmpeg` a link or copy of `../ffmpeg`, then CMake through the module's
`CMakeLists.txt` with the Android NDK toolchain for `arm64-v8a`, as described in `BUILD.md`.
The result is `libffmpegJNI.so`, which links `libavcodec`, `libavutil` and `libswresample`
statically and is the library the app ships.
