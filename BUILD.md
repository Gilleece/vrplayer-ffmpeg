# FFmpeg audio decoder build

The `decoder_ffmpeg/` module is `libraries/decoder_ffmpeg` from the androidx/media repository at
tag 1.11.0 (commit `2bc20785`), copied verbatim apart from `build.gradle`, which is rewritten for
this tree, and `src/main/jni/ffmpeg_jni.cc`, which is changed as below. Its JNI wrapper links
FFmpeg's `libavcodec`, `libavutil` and `libswresample` statically into `libffmpegJNI.so`, so that
library is an LGPL 2.1 work as a whole: this file is the build recipe that goes with the published
source (spec 4.3), and the JNI wrapper is part of it.

## The TrueHD change to ffmpeg_jni.cc

Media3's `MatroskaExtractor` glues 16 TrueHD access units into one sample, which is what the
MediaCodec passthrough path expects, while FFmpeg's `truehd` decoder reads one access unit per
packet and throws away the rest with a "Multiple frames in a packet" warning. TrueHD therefore
failed to decode at all before this change (Phase 5, 2026-09-08). The JNI now keeps an
`AVCodecParserContext` from `av_parser_init(AV_CODEC_ID_TRUEHD)` alongside the codec context, and
for that codec only it runs each sample through `av_parser_parse2` and decodes the units it hands
back one at a time, appending each unit's output to the same buffer. The mlp parser is already in
the build, pulled in by the truehd and mlp decoders. Everything else goes through the unchanged
single packet path.

The parser also fills the channel layout, sample format and sample rate in from the major sync
before the decoder sees the unit. That matters here: the module opens the context with the codec
name alone, and on a Mac build of this same FFmpeg source `audio-test-truehd.mkv` fails on every
access unit without it ("Assignment of matrix channel 0 to invalid output channel", then "channel
count mismatch"), one unit per packet or not.

## Inputs

- FFmpeg tag `n6.1.6` on branch `release/6.1`, commit `9266ee001cb04b9dd6ff0335022bd986e2bc8e8b`
  (RELEASE file `6.1.6`, released 2026-06-20), since 2026-09-22; before that `release/6.0` at
  `3f92512fd1fd6f5e6d6eb45a156c352835314d69` (6.0.2, 2023 code) from Phase 1 to the first
  Phase 8 merge. The move was the security audit's finding 7: the 6.0 branch has had no point
  release since, and 6.1.6 carries three years of decoder fixes. The module's README recommends
  6.0 and its JNI uses the `ch_layout` API, so 5.1 or later is required; 6.1 built and linked
  with no change to `ffmpeg_jni.cc`.
- NDK `27.0.12077973`, host `darwin-x86_64`, `ANDROID_ABI` 29 (the app's minSdk).
- Decoders: `ac3 eac3 dca truehd mlp flac alac vorbis opus pcm_s16le pcm_s24le`.
- No `--enable-gpl`, no `--enable-nonfree`, no external codec libraries. configure reports
  `License: LGPL version 2.1 or later`.

## Steps

```
cd decoder_ffmpeg/src/main/jni
git clone --branch release/6.1 https://git.ffmpeg.org/ffmpeg.git ffmpeg
git -C ffmpeg checkout n6.1.6
./build_ffmpeg.sh "$(pwd)/.." "$HOME/Library/Android/sdk/ndk/27.0.12077973" darwin-x86_64 29 \
    ac3 eac3 dca truehd mlp flac alac vorbis opus pcm_s16le pcm_s24le
```

The script is the upstream one and builds armeabi-v7a, arm64-v8a, x86 and x86_64 in turn; only
`android-libs/arm64-v8a` is used, since the app is arm64 only. Gradle then builds
`libffmpegJNI.so` through the module's `CMakeLists.txt` whenever `src/main/jni/ffmpeg` exists.
The `ffmpeg/` directory is gitignored.

## The arm64-v8a configure line the script ran

```
./configure --libdir=android-libs/arm64-v8a --arch=aarch64 --cpu=armv8-a
  --cross-prefix=<NDK>/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android29-
  --nm=<NDK>/.../llvm-nm --ar=<NDK>/.../llvm-ar --ranlib=<NDK>/.../llvm-ranlib
  --strip=<NDK>/.../llvm-strip
  --target-os=android --enable-static --disable-shared --disable-doc --disable-programs
  --disable-everything --disable-avdevice --disable-avformat --disable-swscale
  --disable-postproc --disable-avfilter --disable-symver --enable-swresample
  --extra-ldexeflags=-pie --disable-v4l2-m2m --disable-vulkan
  --enable-decoder=ac3 --enable-decoder=eac3 --enable-decoder=dca --enable-decoder=truehd
  --enable-decoder=mlp --enable-decoder=flac --enable-decoder=alac --enable-decoder=vorbis
  --enable-decoder=opus --enable-decoder=pcm_s16le --enable-decoder=pcm_s24le
```

configure summary for arm64: libraries avcodec, avutil, swresample; enabled decoders exactly the
eleven above; parsers ac3 and mlp (pulled in by the decoders); no encoders, demuxers, muxers,
protocols, filters or hwaccels; external libraries iconv and zlib (both from bionic and the NDK,
permissive); NEON on, runtime cpu detection on.

## Results (2026-09-22, Mac, 6.1.6)

- Whole script, four ABIs: 1 min 40 s wall clock (140.8 s user, 186 percent CPU).
- Static libraries for arm64-v8a: `libavcodec.a` 8,056,296 bytes, `libavutil.a` 4,151,562,
  `libswresample.a` 574,278.
- configure for arm64: the same eleven decoders, parsers ac3 and mlp, external libraries iconv
  and zlib, `License: LGPL version 2.1 or later`.

## Results (2026-09-04, Mac, 6.0.2)

- Whole script, four ABIs: 1 min 28 s wall clock (129.8 s user, 194 percent CPU).
- Static libraries for arm64-v8a: `libavcodec.a` 8,228,212 bytes, `libavutil.a` 3,903,858,
  `libswresample.a` 572,870.
- `libffmpegJNI.so` in the APK and the APK delta: see `NOTES.md`, Phase 1 spike 5.
