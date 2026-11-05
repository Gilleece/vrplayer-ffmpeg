// Copyright (c) 2026 Sean Gilleece
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// The video half of the FFmpeg module: libavcodec's software decoders for the
// files no MediaCodec decoder on the headset plays well, 10 bit H.264 (High
// 10), MPEG-4 Part 2 with Microsoft's v2 and v3 (DivX, Xvid), WMV 7, 8 and 9
// with VC-1, MPEG-1 and 2, MJPEG, DV, ProRes, DNxHD and the old AVI codecs.
// 8 and 10 bit 4:2:0 frames are copied as they are, anything else goes
// through libswscale into 4:2:0 first. FfmpegVideoDecoder drives it from its
// own thread. A frame that comes out is held here until Java has an output
// buffer for it, then its planes are copied into that buffer and the frame is
// let go.
#include <android/log.h>
#include <jni.h>
#include <stdint.h>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#define LOG_TAG "ffmpeg_video"
#define LOGE(...) \
  ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

#define VIDEO_FUNC(RETURN_TYPE, NAME, ...)                       \
  extern "C" {                                                   \
  JNIEXPORT RETURN_TYPE                                          \
  Java_androidx_media3_decoder_ffmpeg_FfmpegVideoDecoder_##NAME( \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__);                 \
  }                                                              \
  JNIEXPORT RETURN_TYPE                                          \
  Java_androidx_media3_decoder_ffmpeg_FfmpegVideoDecoder_##NAME( \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__)

// What send and receive answer, the same numbers FfmpegVideoDecoder uses
static const int STATUS_OK = 0;
static const int STATUS_AGAIN = 1;
static const int STATUS_END = 2;
static const int STATUS_BAD_DATA = -1;
static const int STATUS_ERROR = -2;

// The slots of the int array receive fills in
static const int INFO_WIDTH = 0;
static const int INFO_HEIGHT = 1;
static const int INFO_BIT_DEPTH = 2;
static const int INFO_FULL_RANGE = 3;
static const int INFO_COLORSPACE = 4;
static const int INFO_DELAY = 5;
static const int INFO_LENGTH = 6;

// Media3's VideoDecoderOutputBuffer.COLORSPACE_* values
static const int COLORSPACE_UNKNOWN = 0;
static const int COLORSPACE_BT601 = 1;
static const int COLORSPACE_BT709 = 2;
static const int COLORSPACE_BT2020 = 3;

struct VideoState {
  AVCodecContext* codec;
  AVPacket* packet;
  AVFrame* frame;
  // A frame has been received and not yet copied or dropped
  bool held;
  int bitDepth;
  // Sample times also go in as decode times, for the codecs whose container
  // may only have those (AVI). H.264 keeps presentation times alone.
  bool sendsDts;
  // The held frame goes through sws on its way out
  bool convert;
  // Made for one input layout, size and range, remade when one changes
  SwsContext* sws;
  int swsFormat;
  int swsWidth;
  int swsHeight;
  bool swsFull;
};

static void logError(const char* what, int error) {
  char text[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(error, text, sizeof(text));
  LOGE("%s: %s", what, text);
}

static void releaseState(VideoState* state) {
  if (!state) {
    return;
  }
  av_frame_free(&state->frame);
  av_packet_free(&state->packet);
  avcodec_free_context(&state->codec);
  sws_freeContext(state->sws);
  av_free(state);
}

// 8 or 10 for the layouts copied as they are, else 0
static int bitDepthOf(int format) {
  switch (format) {
    case AV_PIX_FMT_YUV420P10LE:
      return 10;
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
      return 8;
    default:
      return 0;
  }
}

static int colorspaceOf(AVColorSpace space) {
  switch (space) {
    case AVCOL_SPC_BT709:
      return COLORSPACE_BT709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
      return COLORSPACE_BT601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
      return COLORSPACE_BT2020;
    default:
      return COLORSPACE_UNKNOWN;
  }
}

// 1 full, 0 limited, -1 when the stream does not say
static int fullRangeOf(const AVFrame* frame) {
  switch (frame->format) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ411P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ440P:
    case AV_PIX_FMT_YUVJ444P:
      return 1;
    default:
      break;
  }
  if (frame->color_range == AVCOL_RANGE_JPEG) {
    return 1;
  }
  return frame->color_range == AVCOL_RANGE_MPEG ? 0 : -1;
}

// What a frame sws turns into 4:2:0 is drawn as. sws gets the same range on
// both sides, so YUV and grey samples come out as they went in and keep the
// range the stream gives. RGB and palette frames come out as full range
// BT.601, sws's own matrix. Grey with no range said is taken as full, as sws
// does.
struct Converted {
  int bitDepth;
  int full;
  int colorspace;
  // The range sws is told for both sides
  bool swsFull;
};

static bool convertedOf(const AVFrame* frame, Converted* out) {
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
  if (!desc || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)
      || !sws_isSupportedInput((AVPixelFormat)frame->format)) {
    return false;
  }
  out->bitDepth = desc->comp[0].depth > 8 ? 10 : 8;
  if (desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) {
    out->full = 1;
    out->colorspace = COLORSPACE_BT601;
    out->swsFull = true;
  } else if (desc->nb_components < 3) {
    // sws always reads grey as full range
    out->full = fullRangeOf(frame) == 0 ? 0 : 1;
    out->colorspace = COLORSPACE_BT601;
    out->swsFull = true;
  } else {
    out->full = fullRangeOf(frame);
    out->colorspace = colorspaceOf(frame->colorspace);
    out->swsFull = out->full == 1;
  }
  return true;
}

// The cached context for this frame, made again if the frame differs
static bool prepareSws(VideoState* state, const AVFrame* frame, int bitDepth, bool full) {
  if (state->sws && state->swsFormat == frame->format && state->swsWidth == frame->width
      && state->swsHeight == frame->height && state->swsFull == full) {
    return true;
  }
  sws_freeContext(state->sws);
  state->sws = sws_alloc_context();
  if (!state->sws) {
    return false;
  }
  SwsContext* sws = state->sws;
  sws->src_w = sws->dst_w = frame->width;
  sws->src_h = sws->dst_h = frame->height;
  sws->src_format = frame->format;
  sws->dst_format = bitDepth > 8 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
  // The same range both sides, so YUV samples are not stretched or squeezed
  sws->src_range = sws->dst_range = full;
  sws->flags = SWS_BILINEAR;
  sws->threads = 1;
  int result = sws_init_context(sws, NULL, NULL);
  if (result < 0) {
    logError("sws_init_context", result);
    sws_freeContext(state->sws);
    state->sws = NULL;
    return false;
  }
  state->swsFormat = frame->format;
  state->swsWidth = frame->width;
  state->swsHeight = frame->height;
  state->swsFull = full;
  return true;
}

static void copyPlane(uint8_t* dst, int dstStride, const uint8_t* src, int srcStride,
                      int rowBytes, int rows) {
  for (int y = 0; y < rows; y++) {
    memcpy(dst + (size_t)y * dstStride, src + (size_t)y * srcStride, rowBytes);
  }
}

VIDEO_FUNC(jlong, ffmpegVideoInit, jstring codecName, jbyteArray extraData,
           jint threads, jint width, jint height, jint bitsPerPixel) {
  const char* name = env->GetStringUTFChars(codecName, NULL);
  const AVCodec* codec = avcodec_find_decoder_by_name(name);
  env->ReleaseStringUTFChars(codecName, name);
  if (!codec) {
    LOGE("No decoder for the codec name");
    return 0;
  }
  VideoState* state = (VideoState*)av_mallocz(sizeof(VideoState));
  if (!state) {
    return 0;
  }
  state->codec = avcodec_alloc_context3(codec);
  state->packet = av_packet_alloc();
  state->frame = av_frame_alloc();
  if (!state->codec || !state->packet || !state->frame) {
    LOGE("Failed to allocate the video context");
    releaseState(state);
    return 0;
  }
  AVCodecContext* context = state->codec;
  state->sendsDts = codec->id != AV_CODEC_ID_H264;
  if (extraData) {
    jsize size = env->GetArrayLength(extraData);
    // Zeroed, padding included, which the bitstream reader expects
    context->extradata = (uint8_t*)av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!context->extradata) {
      releaseState(state);
      return 0;
    }
    context->extradata_size = size;
    env->GetByteArrayRegion(extraData, 0, size, (jbyte*)context->extradata);
  }
  // Microsoft's MPEG-4, WMV 7 and 8, WMV3 and the old AVI codecs (Video 1,
  // RLE, HuffYUV, Lagarith) have no frame size in the stream, only in the
  // container
  if (width > 0 && height > 0) {
    context->width = width;
    context->height = height;
  }
  // The BITMAPINFOHEADER's bit count, which msrle and a few other AVI codecs
  // open by
  if (bitsPerPixel > 0) {
    context->bits_per_coded_sample = bitsPerPixel;
  }
  // Timestamps go in and come out in microseconds, untouched
  AVRational micros = {1, 1000000};
  context->pkt_timebase = micros;
  context->thread_count = threads > 0 ? threads : 1;
  context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  int result = avcodec_open2(context, codec, NULL);
  if (result < 0) {
    logError("avcodec_open2", result);
    releaseState(state);
    return 0;
  }
  return (jlong)state;
}

// A null buffer drains the decoder: what it still holds comes out, then the end
VIDEO_FUNC(jint, ffmpegVideoSend, jlong handle, jobject data, jint size, jlong timeUs) {
  VideoState* state = (VideoState*)handle;
  int result;
  if (!data) {
    result = avcodec_send_packet(state->codec, NULL);
  } else {
    uint8_t* bytes = (uint8_t*)env->GetDirectBufferAddress(data);
    jlong capacity = env->GetDirectBufferCapacity(data);
    if (!bytes || size < 0 || size > capacity) {
      return STATUS_ERROR;
    }
    // Media3 sizes the buffer with the padding the reader is allowed to run
    // into, but a reused buffer still holds the last sample there
    jlong pad = capacity - size;
    if (pad > AV_INPUT_BUFFER_PADDING_SIZE) {
      pad = AV_INPUT_BUFFER_PADDING_SIZE;
    }
    memset(bytes + size, 0, (size_t)pad);
    AVPacket* packet = state->packet;
    packet->data = bytes;
    packet->size = size;
    packet->pts = timeUs;
    packet->dts = state->sendsDts ? timeUs : AV_NOPTS_VALUE;
    result = avcodec_send_packet(state->codec, packet);
    packet->data = NULL;
    packet->size = 0;
  }
  if (result == 0) {
    return STATUS_OK;
  }
  if (result == AVERROR(EAGAIN)) {
    return STATUS_AGAIN;
  }
  if (result == AVERROR_EOF) {
    return STATUS_END;
  }
  logError("avcodec_send_packet", result);
  return result == AVERROR_INVALIDDATA ? STATUS_BAD_DATA : STATUS_ERROR;
}

// The next frame, held until it is copied or dropped. info gets its shape,
// colour and the decoder's reorder delay, ptsOut its own time and the time of
// the sample it came out with.
VIDEO_FUNC(jint, ffmpegVideoReceive, jlong handle, jintArray info, jlongArray ptsOut) {
  VideoState* state = (VideoState*)handle;
  if (state->held) {
    av_frame_unref(state->frame);
    state->held = false;
  }
  int result = avcodec_receive_frame(state->codec, state->frame);
  if (result == AVERROR(EAGAIN)) {
    return STATUS_AGAIN;
  }
  if (result == AVERROR_EOF) {
    return STATUS_END;
  }
  if (result < 0) {
    logError("avcodec_receive_frame", result);
    return result == AVERROR_INVALIDDATA ? STATUS_BAD_DATA : STATUS_ERROR;
  }
  AVFrame* frame = state->frame;
  int bitDepth = bitDepthOf(frame->format);
  int full = fullRangeOf(frame);
  int colorspace = colorspaceOf(frame->colorspace);
  bool convert = bitDepth == 0;
  if (convert) {
    Converted converted;
    if (!convertedOf(frame, &converted)
        || !prepareSws(state, frame, converted.bitDepth, converted.swsFull)) {
      const char* formatName = av_get_pix_fmt_name((AVPixelFormat)frame->format);
      LOGE("Pixel format %s is not drawn", formatName ? formatName : "unknown");
      av_frame_unref(frame);
      return STATUS_ERROR;
    }
    bitDepth = converted.bitDepth;
    full = converted.full;
    colorspace = converted.colorspace;
  }
  state->held = true;
  state->bitDepth = bitDepth;
  state->convert = convert;
  jint values[INFO_LENGTH];
  values[INFO_WIDTH] = frame->width;
  values[INFO_HEIGHT] = frame->height;
  values[INFO_BIT_DEPTH] = bitDepth;
  values[INFO_FULL_RANGE] = full;
  values[INFO_COLORSPACE] = colorspace;
  values[INFO_DELAY] = state->codec->has_b_frames;
  env->SetIntArrayRegion(info, 0, INFO_LENGTH, values);
  jlong times[2];
  if (state->sendsDts) {
    times[0] = frame->pts;
  } else {
    times[0] = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp
                                                              : frame->pts;
  }
  times[1] = frame->pkt_dts;
  env->SetLongArrayRegion(ptsOut, 0, 2, times);
  return STATUS_OK;
}

// The held frame into a buffer laid out as VideoDecoderOutputBuffer's
// initForYuvFrame lays it out: Y, then U, then V, each row at its stride,
// samples 16 bit little endian at 10 bit and one byte at 8
VIDEO_FUNC(jint, ffmpegVideoCopy, jlong handle, jobject data, jint yStride, jint uvStride) {
  VideoState* state = (VideoState*)handle;
  if (!state->held) {
    return STATUS_ERROR;
  }
  AVFrame* frame = state->frame;
  int bytes = state->bitDepth > 8 ? 2 : 1;
  int width = frame->width;
  int height = frame->height;
  int chromaWidth = (width + 1) / 2;
  int chromaHeight = (height + 1) / 2;
  uint8_t* out = (uint8_t*)env->GetDirectBufferAddress(data);
  jlong capacity = env->GetDirectBufferCapacity(data);
  jlong ySize = (jlong)yStride * height;
  jlong uvSize = (jlong)uvStride * chromaHeight;
  int status = STATUS_OK;
  if (!out || yStride < width * bytes || uvStride < chromaWidth * bytes
      || capacity < ySize + uvSize * 2) {
    LOGE("Output buffer too small for %dx%d", width, height);
    status = STATUS_ERROR;
  } else if (state->convert) {
    uint8_t* planes[4] = {out, out + ySize, out + ySize + uvSize, NULL};
    int strides[4] = {yStride, uvStride, uvStride, 0};
    int result = sws_scale(state->sws, frame->data, frame->linesize, 0, height, planes,
                           strides);
    if (result < 0) {
      logError("sws_scale", result);
      status = STATUS_ERROR;
    }
  } else {
    copyPlane(out, yStride, frame->data[0], frame->linesize[0], width * bytes, height);
    copyPlane(out + ySize, uvStride, frame->data[1], frame->linesize[1], chromaWidth * bytes,
              chromaHeight);
    copyPlane(out + ySize + uvSize, uvStride, frame->data[2], frame->linesize[2],
              chromaWidth * bytes, chromaHeight);
  }
  av_frame_unref(frame);
  state->held = false;
  return status;
}

VIDEO_FUNC(void, ffmpegVideoDrop, jlong handle) {
  VideoState* state = (VideoState*)handle;
  if (state->held) {
    av_frame_unref(state->frame);
    state->held = false;
  }
}

VIDEO_FUNC(void, ffmpegVideoFlush, jlong handle) {
  VideoState* state = (VideoState*)handle;
  if (state->held) {
    av_frame_unref(state->frame);
    state->held = false;
  }
  avcodec_flush_buffers(state->codec);
}

VIDEO_FUNC(void, ffmpegVideoRelease, jlong handle) {
  releaseState((VideoState*)handle);
}
