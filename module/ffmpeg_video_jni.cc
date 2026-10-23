// Copyright (c) 2026 Sean Gilleece. All rights reserved.

// The video half of the FFmpeg module: libavcodec's software decoders for the
// files no MediaCodec decoder on the headset plays well, 10 bit H.264 (High
// 10) and MPEG-4 Part 2 with Microsoft's v2 and v3 (DivX, Xvid). Both come out
// as 8 or 10 bit 4:2:0. FfmpegVideoDecoder drives it from its own thread. A
// frame that comes out is held here until Java has an output buffer for it,
// then its planes are copied into that buffer and the frame is let go.
#include <android/log.h>
#include <jni.h>
#include <stdint.h>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
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
  av_free(state);
}

// 8 or 10, or 0 for a layout this path does not draw
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
  if (frame->format == AV_PIX_FMT_YUVJ420P || frame->color_range == AVCOL_RANGE_JPEG) {
    return 1;
  }
  return frame->color_range == AVCOL_RANGE_MPEG ? 0 : -1;
}

static void copyPlane(uint8_t* dst, int dstStride, const uint8_t* src, int srcStride,
                      int rowBytes, int rows) {
  for (int y = 0; y < rows; y++) {
    memcpy(dst + (size_t)y * dstStride, src + (size_t)y * srcStride, rowBytes);
  }
}

VIDEO_FUNC(jlong, ffmpegVideoInit, jstring codecName, jbyteArray extraData,
           jint threads, jint width, jint height) {
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
  // Microsoft's MPEG-4 has no frame size in the stream, only in the container
  if (width > 0 && height > 0) {
    context->width = width;
    context->height = height;
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
  if (bitDepth == 0) {
    const char* formatName = av_get_pix_fmt_name((AVPixelFormat)frame->format);
    LOGE("Pixel format %s is not drawn", formatName ? formatName : "unknown");
    av_frame_unref(frame);
    return STATUS_ERROR;
  }
  state->held = true;
  state->bitDepth = bitDepth;
  jint values[INFO_LENGTH];
  values[INFO_WIDTH] = frame->width;
  values[INFO_HEIGHT] = frame->height;
  values[INFO_BIT_DEPTH] = bitDepth;
  values[INFO_FULL_RANGE] = fullRangeOf(frame);
  values[INFO_COLORSPACE] = colorspaceOf(frame->colorspace);
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
