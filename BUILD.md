# FFmpeg decoder build

The `decoder_ffmpeg/` module is `libraries/decoder_ffmpeg` from the androidx/media repository at
tag 1.11.0 (commit `2bc20785`), copied verbatim apart from `build.gradle`, which is rewritten for
this tree, `src/main/jni/ffmpeg_jni.cc`, which is changed as below, `build_ffmpeg.sh`, which
enables the h264 parser, `CMakeLists.txt`, which builds the video JNI too, `FfmpegLibrary.java`,
which names the decoders for the video codecs and picks the WMV and WMA decoder by the codecs
string, and `FfmpegAudioDecoder.java` and `FfmpegAudioRenderer.java`, which pass WMA's codecs
string, extradata, block align and bit rate on. Its JNI wrapper
links FFmpeg's `libavcodec`, `libavutil` and `libswresample` statically into `libffmpegJNI.so`,
so that library is an LGPL 2.1 work as a whole: this file is the build recipe that goes with the
published source (spec 4.3), and the JNI wrapper is part of it.

## The H.264 video decoder (2026-10-17)

10 bit H.264 (High 10) has no hardware decoder on any headset, so since 1.1.0 it decodes in
software on Gen 2 headsets. `src/main/jni/ffmpeg_video_jni.cc` (the owner's) opens FFmpeg's
`h264` decoder with frame threads, cores minus two, from the decoder's own thread at default
priority so the frame threads do not take the player thread's audio priority, takes the Annex B
samples and parameter
sets Media3's extractors hand over, and copies each `yuv420p10le` (or 8 bit `yuv420p`) frame
into a Media3 output buffer as three planes. `FfmpegVideoDecoder`, `FfmpegVideoRenderer`,
`FfmpegVideoOutputBuffer` and `FfmpegVideoPlanes` are the Java side, also the owner's. The
decoder is LGPL like the rest of the build and lives in the same dynamically loaded `.so`. No
bitstream filter is needed: the decoder reads Annex B and avcC extradata itself.

## MPEG-4 Part 2 (2026-10-23)

AVI files carry MPEG-4 Part 2 (DivX, Xvid) or Microsoft's MPEG-4 v2 and v3, which no headset
decodes well with MediaCodec, so they go through the same video JNI with FFmpeg's `mpeg4`,
`msmpeg4v2` and `msmpeg4v3` decoders, all LGPL. They output 8 bit `yuv420p`, which the JNI
already copied. Two changes to `ffmpeg_video_jni.cc`: the track's frame size goes on the context
before it opens, since Microsoft's variants have none in the stream, and for every codec but
H.264 each sample's time goes in as its decode time too, while a frame comes back with its own
time, the time of the sample it came out with and the decoder's reorder delay (`has_b_frames`),
from which the Java side works out when it is shown. AVI has decode order times only.

## Windows Media (2026-10-23)

WMV and WMA files come from the app's own ASF extractor, and no headset has a MediaCodec decoder
for any of them, so they go to FFmpeg's `wmv1`, `wmv2`, `wmv3`, `vc1`, `wmav1`, `wmav2`,
`wmapro` and `wmalossless` decoders, all LGPL. No parser is needed: each ASF media object is a
whole frame, and VC-1's Advanced Profile carries its sequence headers in band, which the decoder
reads. The video goes through the video JNI unchanged: WMV3's 4 byte sequence header or VC-1's
headers are the extradata, the track's frame size goes on the context, and ASF's object times
are in decode order like AVI's, so the same reorder sum gives when a frame is shown. `vc1` and
`wmv3` have no threads in FFmpeg and decode on one core. For the audio, `ffmpeg_jni.cc` takes
the block align and bit rate from the track's WAVEFORMATEX and puts them, with the sample rate and
channel count, on the context before it opens, which all four decoders need; `wmapro` and
`wmalossless` take the channel mask from the extradata. The extradata buffer is now zeroed,
padding included.

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
- Decoders: `ac3 eac3 dca truehd mlp flac alac vorbis opus pcm_s16le pcm_s24le h264 mpeg4
  msmpeg4v2 msmpeg4v3 wmv1 wmv2 wmv3 vc1 wmav1 wmav2 wmapro wmalossless` (h264 since 2026-10-17,
  the three MPEG-4 ones and the eight Windows Media ones since 2026-10-23), and the `h264` parser
  through `--enable-parser=h264` in the script's common options.
- No `--enable-gpl`, no `--enable-nonfree`, no external codec libraries. configure reports
  `License: LGPL version 2.1 or later`.

## Steps

```
cd decoder_ffmpeg/src/main/jni
git clone --branch release/6.1 https://git.ffmpeg.org/ffmpeg.git ffmpeg
git -C ffmpeg checkout n6.1.6
./build_ffmpeg.sh "$(pwd)/.." "$HOME/Library/Android/sdk/ndk/27.0.12077973" darwin-x86_64 29 \
    ac3 eac3 dca truehd mlp flac alac vorbis opus pcm_s16le pcm_s24le h264 mpeg4 msmpeg4v2 msmpeg4v3 \
    wmv1 wmv2 wmv3 vc1 wmav1 wmav2 wmapro wmalossless
```

The script is the upstream one with one line added to its common options,
`--enable-parser=h264`, and builds armeabi-v7a, arm64-v8a, x86 and x86_64 in turn; only
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
  --extra-ldexeflags=-pie --disable-v4l2-m2m --disable-vulkan --enable-parser=h264
  --enable-decoder=ac3 --enable-decoder=eac3 --enable-decoder=dca --enable-decoder=truehd
  --enable-decoder=mlp --enable-decoder=flac --enable-decoder=alac --enable-decoder=vorbis
  --enable-decoder=opus --enable-decoder=pcm_s16le --enable-decoder=pcm_s24le
  --enable-decoder=h264 --enable-decoder=mpeg4 --enable-decoder=msmpeg4v2
  --enable-decoder=msmpeg4v3 --enable-decoder=wmv1 --enable-decoder=wmv2 --enable-decoder=wmv3
  --enable-decoder=vc1 --enable-decoder=wmav1 --enable-decoder=wmav2 --enable-decoder=wmapro
  --enable-decoder=wmalossless
```

configure summary for arm64: libraries avcodec, avutil, swresample; enabled decoders the
twenty three above and h263, which mpeg4 pulls in; parsers ac3, h263, h264, mlp and mpeg4video;
no encoders, demuxers, muxers, protocols, filters, bitstream filters or hwaccels; external libraries iconv and zlib (both from bionic and the NDK,
permissive); NEON, DOTPROD and I8MM on, runtime cpu detection on, threading pthreads.

## Results (2026-10-23, Mac, 6.1.6 with h264, MPEG-4 and Windows Media)

- Whole script, four ABIs: 3 min 30 s wall clock (361.9 s user, 79.8 s sys).
- Static libraries for arm64-v8a: `libavcodec.a` 27,361,876 bytes, `libavutil.a` 4,151,866,
  `libswresample.a` 574,590.
- configure for arm64: twenty four decoders (the twenty three named and h263), parsers ac3,
  h263, h264, mlp and mpeg4video, external libraries iconv and zlib, `License: LGPL version 2.1
  or later`. No GPL, no nonfree.
- `libffmpegJNI.so` for arm64, stripped, in the debug build: 3,564,192 bytes, against 3,103,400
  without Windows Media.

## Results (2026-10-23, Mac, 6.1.6 with h264 and MPEG-4)

- Whole script, four ABIs: 3 min 8 s wall clock (302.3 s user, 72.5 s sys).
- Static libraries for arm64-v8a: `libavcodec.a` 23,707,184 bytes, `libavutil.a` 4,151,682,
  `libswresample.a` 574,398.
- configure for arm64: sixteen decoders (the fifteen named and h263), parsers ac3, h263, h264,
  mlp and mpeg4video, external libraries iconv and zlib, `License: LGPL version 2.1 or later`.
  No GPL, no nonfree.
- `libffmpegJNI.so` for arm64, stripped, in the debug build: 3,103,400 bytes, against 2,715,000
  with h264 alone.

## Results (2026-10-17, Mac, 6.1.6 with h264)

- Whole script, four ABIs: 9 min 26 s wall clock (322.3 s user, 101.9 s sys), with other builds
  running on the Mac at the same time.
- Static libraries for arm64-v8a: `libavcodec.a` 19,188,442 bytes, `libavutil.a` 4,150,490,
  `libswresample.a` 574,158.
- configure for arm64: the twelve decoders, parsers ac3, h264 and mlp, no bitstream filters,
  external libraries iconv and zlib, `License: LGPL version 2.1 or later`. No GPL, no nonfree.
- `libffmpegJNI.so` for arm64, stripped, in the debug build: 2,715,000 bytes, against 1,402,384
  before the h264 decoder and the video JNI.
- The source offer repository holds the 6.1.6 tarball, the JNI wrapper, the build script and this
  recipe, so the new script line, `ffmpeg_video_jni.cc` and the CMake change go there too.

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
