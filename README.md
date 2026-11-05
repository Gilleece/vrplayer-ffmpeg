# vrplayer-ffmpeg

The complete source of `libffmpegJNI.so`, the audio and video decoder library that the Realtime
3D Player VR app loads at runtime, published to satisfy the GNU Lesser General Public License
version 2.1 (section 6) under which FFmpeg is licensed. The app itself is closed source; this
library is a separate work, built with no GPL or non-free parts, and may be replaced by a
compatible build of the same library.

## What is here

- `ffmpeg-9.0.2-946fcce.tar.gz`: FFmpeg, tag `n9.0.2` on branch `release/9.0` at commit
  `946fcce07b6dcd0331c8cc609192aeff5e1924f8` (RELEASE file 9.0.2), unmodified, as a
  `git archive` of that commit from https://git.ffmpeg.org/ffmpeg.git. `fetch-ffmpeg.sh`
  unpacks it into `ffmpeg/` (or clones the same commit when the tarball is absent).
  FFmpeg is LGPL-2.1-or-later (`LICENSE`, which is FFmpeg's `COPYING.LGPLv2.1`).
- `module/`: the JNI wrapper and build script, from `libraries/decoder_ffmpeg` of
  https://github.com/androidx/media at tag 1.11.0 (commit `2bc20785`), Apache-2.0
  (`module/LICENSE`). `build_ffmpeg.sh` builds arm64-v8a alone and adds parsers, a bitstream
  filter and libswscale, and `CMakeLists.txt` builds the video JNI too and links libswscale.
  `ffmpeg_jni.cc` carries three changes: TrueHD and MPEG audio samples are split into frames
  with an `AVCodecParserContext` before decoding, and Windows Media Audio and ADPCM get their
  sample rate, channels, block align, bit rate and bits per sample from the container.
  `ffmpeg_video_jni.cc`, the video decoder for 10 bit H.264, MPEG-1, 2 and 4, WMV, VC-1, MJPEG,
  DV, ProRes, DNxHD and the older AVI codecs, which turns frames that are not 4:2:0 into 4:2:0
  with libswscale, is the app owner's. All described in `BUILD.md`.
- `BUILD.md`: the exact build recipe, toolchain versions, decoder list and the configure line
  that produced the shipped library.

## Building

```
./fetch-ffmpeg.sh
cd module
./build_ffmpeg.sh "$(pwd)" "$ANDROID_NDK" darwin-x86_64 29 \
    ac3 eac3 dca truehd mlp flac alac vorbis opus pcm_s16le pcm_s24le h264 mpeg4 msmpeg4v2 msmpeg4v3 \
    wmv1 wmv2 wmv3 vc1 wmav1 wmav2 wmapro wmalossless mpeg1video mpeg2video mp1 mp2 mp3 msmpeg4v1 \
    mjpeg dvvideo theora flv vp6 vp6f amrnb amrwb adpcm_ima_wav adpcm_ms pcm_u8 pcm_s32le pcm_f32le \
    pcm_s16be pcm_s24be pcm_bluray pcm_dvd hevc prores dnxhd cinepak indeo3 indeo4 indeo5 msvideo1 \
    msrle huffyuv ffv1 lagarith
```

with `module/ffmpeg` a link or copy of `../ffmpeg`, then CMake through the module's
`CMakeLists.txt` with the Android NDK toolchain for `arm64-v8a`, as described in `BUILD.md`.
The result is `libffmpegJNI.so`, which links `libavcodec`, `libavutil`, `libswresample` and
`libswscale` statically and is the library the app ships.
