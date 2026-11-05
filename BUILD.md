# FFmpeg decoder build

The `decoder_ffmpeg/` module is `libraries/decoder_ffmpeg` from the androidx/media repository at
tag 1.11.0 (commit `2bc20785`), copied verbatim apart from `build.gradle`, which is rewritten for
this tree, `src/main/jni/ffmpeg_jni.cc`, which is changed as below, `build_ffmpeg.sh`, which
builds arm64-v8a alone and adds parsers, a bitstream filter and libswscale, `CMakeLists.txt`,
which builds the video JNI too and links libswscale, `FfmpegLibrary.java`, which names the
decoders for the video codecs and picks the WMV and WMA decoder by the codecs string, and
`FfmpegAudioDecoder.java` and `FfmpegAudioRenderer.java`, which pass the codecs string,
extradata, block align, bit rate and bits per sample of a WAVEFORMATEX track on. The module's
`README.md` is upstream's and still describes four ABIs and FFmpeg 6.0. Its JNI wrapper links
FFmpeg's `libavcodec`, `libavutil`, `libswresample` and `libswscale` statically into
`libffmpegJNI.so`, so that library is an LGPL 2.1 work as a whole: this file is the build recipe
that goes with the published source (spec 4.3), and the JNI wrapper is part of it.

## FFmpeg 9.0.2 (2026-11-04)

DR-638 and DR-634. MPEG-PS and TS files failed with "No decoder for this video (MPEG-2)" and "No
decoder for this audio (mpeg-L2)", DivX AVIs failed on their MP3 audio in MediaCodec, whose
decoder refuses AVI chunks that are not cut on frame boundaries, and 3GP H.263 and MJPEG had no
decoder at all. The library moves from 6.1.6 to 9.0.2 and takes thirty five more
decoders, all LGPL: `mpeg1video mpeg2video mp1 mp2 mp3 msmpeg4v1 mjpeg dvvideo theora flv
vp6 vp6f amrnb amrwb adpcm_ima_wav adpcm_ms pcm_u8 pcm_s32le pcm_f32le pcm_s16be pcm_s24be
pcm_bluray pcm_dvd hevc prores dnxhd cinepak indeo3 indeo4 indeo5 msvideo1 msrle huffyuv ffv1
lagarith`. Every name exists in 9.0.2 as written.

- The JNI compiled against 9.0.2 with no change: nothing it calls was removed since 6.1.
  configure no longer knows `--disable-postproc` (libpostproc, GPL, is gone from the 9.0
  tree), so the script drops it.
- The script builds arm64-v8a alone. armeabi-v7a, x86 and x86_64 were never packaged, since the
  app is arm64 only, and building them tripled the time.
- Parsers `hevc`, `mpegvideo`, `mpegaudio` and `mjpeg` join `h264`, and the
  `mpeg4_unpack_bframes` bitstream filter is built in for AVI files with packed B-frames;
  nothing calls the filter yet, the `mpeg4` decoder still unpacks them itself. The video JNI
  uses no parser: each sample goes in as one packet, and MPEG-1 and 2 need whole pictures (a
  picture sent in two halves decodes with concealed macroblocks), which Media3's H.262 reader
  gives for TS and PS, one sample per picture.
- `ffmpeg_jni.cc` runs the `mpegaudio` parser for `mp1`, `mp2` and `mp3` the way it runs the mlp
  parser for TrueHD, on a context of its own since the parser writes the layer it finds into
  it, so a sample may hold part of a frame or several and the rest is held for the next. The
  parser is made again on a seek. `AUDIO_MPEG_L1` and `AUDIO_MPEG_L2` now go to `mp1` and `mp2`
  rather than `mp3`; FFmpeg's three fixed point MPEG audio decoders take any layer.
- `ffmpeg_jni.cc` puts the sample rate, channels, block align, bit rate and now the bits per
  sample on the context of any codec given a block align, which the ASF and AVI extractors take
  from the WAVEFORMATEX (the track's second initialization entry) for WMA and for `adpcm_ms` and
  `adpcm_ima_wav`; `adpcm_ima_wav` will not open without the bits per sample.
- `ffmpeg_video_jni.cc` puts the BITMAPINFOHEADER bit count on the context when the track
  carries one (`FfmpegBitmapInfo` as the Format's custom data): `msrle` refuses to open without
  it, and `lagarith`, `msvideo1`, `cinepak` and old HuffYUV choose their layout by it. The frame
  size goes on the context for every codec whose track has one, H.264 included, as before.
- `ffmpeg_video_jni.cc` copies 8 and 10 bit 4:2:0 (`yuv420p`, `yuvj420p`, `yuv420p10le`) as
  before and sends anything else through libswscale (LGPL) into `yuv420p`, or `yuv420p10le` for
  a source deeper than 8 bits: 4:2:2, 4:1:1, 4:4:4, 4:1:0 (Indeo), 12 bit, RGB, BGR, RGB555,
  palette and grey. sws writes straight into the output buffer, bilinear for the chroma, one
  thread, with the same range on both sides, so YUV and grey samples keep their values and their
  range; RGB and palette frames come out as full range BT.601. Full range already reached the
  renderer: the JNI reports it per frame (`yuvj*` formats or `AVCOL_RANGE_JPEG`), else the
  track's colour range decides, `FfmpegVideoOutputBuffer.fullRange` carries it, `XrRenderer`
  passes it to `nativePlanarUpload`, and `yuvRange` in `xr_yuv.c` scales by it. The `yuvj`
  formats other than 4:2:0 now count as full range too.
- `FfmpegVideoRenderer.isVideoCodec` takes every new video mime; whether a track goes to FFmpeg
  is still the gate's call. H.265 stays out, so it keeps MediaCodec.


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

- FFmpeg tag `n9.0.2` on branch `release/9.0`, commit `946fcce07b6dcd0331c8cc609192aeff5e1924f8`
  (RELEASE file `9.0.2`, committed 2026-09-18), since 2026-11-04. From 2026-09-22 it was tag
  `n6.1.6` on branch `release/6.1`, commit `9266ee001cb04b9dd6ff0335022bd986e2bc8e8b`
  (RELEASE file `6.1.6`, released 2026-06-20); before that `release/6.0` at
  `3f92512fd1fd6f5e6d6eb45a156c352835314d69` (6.0.2, 2023 code) from Phase 1 to the first
  Phase 8 merge. The move was the security audit's finding 7: the 6.0 branch has had no point
  release since, and 6.1.6 carries three years of decoder fixes. The module's README recommends
  6.0 and its JNI uses the `ch_layout` API, so 5.1 or later is required; 6.1 built and linked
  with no change to `ffmpeg_jni.cc`.
- NDK `27.0.12077973`, host `darwin-x86_64`, `ANDROID_ABI` 29 (the app's minSdk), arm64-v8a
  only.
- Decoders: `ac3 eac3 dca truehd mlp flac alac vorbis opus pcm_s16le pcm_s24le h264 mpeg4
  msmpeg4v2 msmpeg4v3 wmv1 wmv2 wmv3 vc1 wmav1 wmav2 wmapro wmalossless` (h264 since 2026-10-17,
  the three MPEG-4 ones and the eight Windows Media ones since 2026-10-23) and, since
  2026-11-04, `mpeg1video mpeg2video mp1 mp2 mp3 msmpeg4v1 mjpeg dvvideo theora flv vp6 vp6f
  amrnb amrwb adpcm_ima_wav adpcm_ms pcm_u8 pcm_s32le pcm_f32le pcm_s16be pcm_s24be pcm_bluray
  pcm_dvd hevc prores dnxhd cinepak indeo3 indeo4 indeo5 msvideo1 msrle huffyuv ffv1 lagarith`.
- In the script's common options: parsers `h264`, `hevc`, `mpegvideo`, `mpegaudio` and `mjpeg`,
  the `mpeg4_unpack_bframes` bitstream filter, and `--enable-swscale`.
- No `--enable-gpl`, no `--enable-nonfree`, no external codec libraries. configure reports
  `License: LGPL version 2.1 or later`.

## Steps

```
cd decoder_ffmpeg/src/main/jni
git clone --branch release/9.0 https://git.ffmpeg.org/ffmpeg.git ffmpeg
git -C ffmpeg checkout n9.0.2
./build_ffmpeg.sh "$(pwd)/.." "$HOME/Library/Android/sdk/ndk/27.0.12077973" darwin-x86_64 29 \
    ac3 eac3 dca truehd mlp flac alac vorbis opus pcm_s16le pcm_s24le h264 mpeg4 msmpeg4v2 msmpeg4v3 \
    wmv1 wmv2 wmv3 vc1 wmav1 wmav2 wmapro wmalossless mpeg1video mpeg2video mp1 mp2 mp3 msmpeg4v1 \
    mjpeg dvvideo theora flv vp6 vp6f amrnb amrwb adpcm_ima_wav adpcm_ms pcm_u8 pcm_s32le pcm_f32le \
    pcm_s16be pcm_s24be pcm_bluray pcm_dvd hevc prores dnxhd cinepak indeo3 indeo4 indeo5 msvideo1 \
    msrle huffyuv ffv1 lagarith
```

The script is the upstream one with the parsers, the bitstream filter and libswscale added to
its common options, `--disable-postproc` taken out, and the armeabi-v7a, x86 and x86_64 passes
taken out, so it builds `android-libs/arm64-v8a` alone. Gradle then builds `libffmpegJNI.so`
through the module's `CMakeLists.txt` whenever `src/main/jni/ffmpeg` exists. The `ffmpeg/`
directory is gitignored.

## The arm64-v8a configure line the script ran

```
./configure --libdir=android-libs/arm64-v8a --arch=aarch64 --cpu=armv8-a
  --cross-prefix=<NDK>/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android29-
  --nm=<NDK>/.../llvm-nm --ar=<NDK>/.../llvm-ar --ranlib=<NDK>/.../llvm-ranlib
  --strip=<NDK>/.../llvm-strip
  --target-os=android --enable-static --disable-shared --disable-doc --disable-programs
  --disable-everything --disable-avdevice --disable-avformat --enable-swscale
  --disable-avfilter --disable-symver --enable-swresample
  --extra-ldexeflags=-pie --disable-v4l2-m2m --disable-vulkan --enable-parser=h264
  --enable-parser=hevc --enable-parser=mpegvideo --enable-parser=mpegaudio
  --enable-parser=mjpeg --enable-bsf=mpeg4_unpack_bframes
  --enable-decoder=<each of the fifty eight decoders above>
```

configure summary for arm64: libraries avcodec, avutil, swresample and swscale; sixty decoders,
the fifty eight named, h263, which mpeg4 pulls in, and vp3, which theora pulls in; parsers ac3,
h264, hevc, mjpeg, mlp, mpegaudio and mpegvideo (ac3 and mlp pulled in by their decoders; 9.0
no longer pulls in h263 and mpeg4video as 6.1 did); bitstream filter mpeg4_unpack_bframes; no
encoders, demuxers, muxers, protocols, filters or hwaccels; external libraries iconv and zlib
(both from bionic and the NDK, permissive); NEON, DOTPROD, I8MM, SVE, SVE2 and SME on behind
runtime cpu detection, threading pthreads. `License: LGPL version 2.1 or later`.

## Results (2026-11-04, Mac, 9.0.2, arm64 only)

- Whole script: 55 s wall clock (127.4 s user, 23.6 s sys).
- Static libraries for arm64-v8a: `libavcodec.a` 39,018,474 bytes, `libavutil.a` 4,563,008,
  `libswresample.a` 577,546, `libswscale.a` 8,868,838.
- configure: sixty decoders, seven parsers, one bitstream filter, external libraries iconv and
  zlib, `License: LGPL version 2.1 or later`. No GPL, no nonfree.
- `libffmpegJNI.so` for arm64, stripped, in the debug build: 6,315,888 bytes, against 3,564,192
  on 6.1.6. libswscale and the conversion are about 1.29 MB of that: the same objects linked by
  hand without them come to 5,018,232 bytes, with them 6,308,768.
- Checked on the Mac with Homebrew's FFmpeg 9.0.2 libraries driving the two JNI files through a
  stand-in JNIEnv (not on a headset): MJPEG 4:2:0, 4:2:2 and 4:4:4, ProRes 422 and 4444, DNxHR
  444, DV PAL, NTSC and DV50, HuffYUV YUV and RGB, FFV1 grey, grey 10 and 4:2:0 12 bit, Cinepak,
  MS Video 1, MS RLE 8 bit (with the bit count), MPEG-2 4:2:0 and 4:2:2 in PS and TS, MPEG-1, HEVC
  4:2:2 10 bit and H.263 in 3GP all come out with every frame; luma matches the decoder's own
  exactly for YUV and grey and within 1 for RGB and palette (BT.601 full range), within 6.5 for
  RGB555 against a reference that widens 5 bits by 255/31; chroma averages 0.25 to 3.8 steps
  from a box average of the source, the difference being sws's wider bilinear taps at edges. MP3, MP2 and MPEG-PS MP2 fed in 7, 417, 1000 and 100,000 byte pieces give the
  same PCM as whole frames, a seek in the middle included; ADPCM MS and IMA open from the
  WAVEFORMATEX fields and give the reference decoder's samples.
- sws on one Mac core, bilinear: 1080p `yuv422p` to `yuv420p` 1.8 ms, `yuv422p10le` to
  `yuv420p10le` 3.4 ms, 2160p 7.3 and 13.4 ms. Not measured on a headset.
- The source offer repository takes the 9.0.2 tarball in place of 6.1.6's, the four module files
  and this recipe; it has to be pushed before a build with this library ships.

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
