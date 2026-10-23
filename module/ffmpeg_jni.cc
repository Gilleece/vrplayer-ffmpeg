/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <android/log.h>
#include <jni.h>
#include <stdlib.h>

extern "C" {
#ifdef __cplusplus
#define __STDC_CONSTANT_MACROS
#ifdef _STDINT_H
#undef _STDINT_H
#endif
#include <stdint.h>
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#define LOG_TAG "ffmpeg_jni"
#define LOGE(...) \
  ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))
#define LOGD(...) \
  ((void)__android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__))

#define LIBRARY_FUNC(RETURN_TYPE, NAME, ...)                               \
  extern "C" {                                                             \
  JNIEXPORT RETURN_TYPE                                                    \
  Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_##NAME(JNIEnv* env,    \
                                                           jobject thiz,   \
                                                           ##__VA_ARGS__); \
  }                                                                        \
  JNIEXPORT RETURN_TYPE                                                    \
  Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_##NAME(                \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__)

#define AUDIO_DECODER_FUNC(RETURN_TYPE, NAME, ...)               \
  extern "C" {                                                   \
  JNIEXPORT RETURN_TYPE                                          \
  Java_androidx_media3_decoder_ffmpeg_FfmpegAudioDecoder_##NAME( \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__);                 \
  }                                                              \
  JNIEXPORT RETURN_TYPE                                          \
  Java_androidx_media3_decoder_ffmpeg_FfmpegAudioDecoder_##NAME( \
      JNIEnv* env, jobject thiz, ##__VA_ARGS__)

#define ERROR_STRING_BUFFER_LENGTH 256

// Output format corresponding to AudioFormat.ENCODING_PCM_16BIT.
static const AVSampleFormat OUTPUT_FORMAT_PCM_16BIT = AV_SAMPLE_FMT_S16;
// Output format corresponding to AudioFormat.ENCODING_PCM_FLOAT.
static const AVSampleFormat OUTPUT_FORMAT_PCM_FLOAT = AV_SAMPLE_FMT_FLT;

// LINT.IfChange
static const int AUDIO_DECODER_ERROR_INVALID_DATA = -1;
static const int AUDIO_DECODER_ERROR_OTHER = -2;
// LINT.ThenChange(../java/androidx/media3/decoder/ffmpeg/FfmpegAudioDecoder.java)

static jmethodID growOutputBufferMethod;

/**
 * What hangs off the codec context: the resampler and the input it was built
 * for, the output format fixed by the first frame, since that is what the Java
 * side reports to the sink, a packet and a frame reused for every call, and for
 * TrueHD the parser that splits a sample into access units.
 */
struct DecoderState {
  SwrContext* resampleContext;
  AVChannelLayout inLayout;
  int inFormat;
  int inRate;
  AVChannelLayout outLayout;
  int outRate;
  int haveOutput;
  AVPacket* packet;
  AVFrame* frame;
  AVCodecParserContext* parser;
};

/**
 * Returns the AVCodec with the specified name, or NULL if it is not available.
 */
const AVCodec* getCodecByName(JNIEnv* env, jstring codecName);

/**
 * Allocates and opens a new AVCodecContext for the specified codec, passing the
 * provided extraData as initialization data for the decoder if it is non-NULL.
 * The block align and bit rate are the container's, for the codecs that need
 * them, and 0 otherwise. Returns the created context.
 */
AVCodecContext* createContext(JNIEnv* env, const AVCodec* codec,
                              jbyteArray extraData, jboolean outputFloat,
                              jint rawSampleRate, jint rawChannelCount,
                              jint blockAlign, jlong bitRate);

struct GrowOutputBufferCallback {
  uint8_t* operator()(int requiredSize) const;

  JNIEnv* env;
  jobject thiz;
  jobject decoderOutputBuffer;
};

/**
 * Where the decoded samples are written. The buffer moves when it has to grow,
 * so the place for the next write is the base pointer plus what has been
 * written so far rather than wherever the last write left off.
 */
struct OutputBuffer {
  /** Returns room for the given number of bytes, or NULL if there is none. */
  uint8_t* reserve(int bytes);

  uint8_t* data;
  int size;
  int written;
  GrowOutputBufferCallback growBuffer;
};

/**
 * Decodes the packet, appending what comes out to the output buffer. Returns 0,
 * or a negative AUDIO_DECODER_ERROR constant value in the case of an error.
 */
int decodePacket(AVCodecContext* context, AVPacket* packet,
                 OutputBuffer* output);

/**
 * Decodes a sample made of several access units, one unit at a time. Returns 0,
 * or a negative AUDIO_DECODER_ERROR constant value in the case of an error.
 */
int decodeAccessUnits(AVCodecContext* context, AVCodecParserContext* parser,
                      AVPacket* packet, const uint8_t* input, int inputSize,
                      OutputBuffer* output);

/**
 * Transforms ffmpeg AVERROR into a negative AUDIO_DECODER_ERROR constant value.
 */
int transformError(int errorNumber);

/**
 * Outputs a log message describing the avcodec error number.
 */
void logError(const char* functionName, int errorNumber);

/**
 * Releases the specified context.
 */
void releaseContext(AVCodecContext* context);

jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    LOGE("JNI_OnLoad: GetEnv failed");
    return -1;
  }
  jclass clazz =
      env->FindClass("androidx/media3/decoder/ffmpeg/FfmpegAudioDecoder");
  if (!clazz) {
    LOGE("JNI_OnLoad: FindClass failed");
    return -1;
  }
  growOutputBufferMethod =
      env->GetMethodID(clazz, "growOutputBuffer",
                       "(Landroidx/media3/decoder/"
                       "SimpleDecoderOutputBuffer;I)Ljava/nio/ByteBuffer;");
  if (!growOutputBufferMethod) {
    LOGE("JNI_OnLoad: GetMethodID failed");
    return -1;
  }
  return JNI_VERSION_1_6;
}

LIBRARY_FUNC(jstring, ffmpegGetVersion) {
  return env->NewStringUTF(LIBAVCODEC_IDENT);
}

LIBRARY_FUNC(jint, ffmpegGetInputBufferPaddingSize) {
  return (jint)AV_INPUT_BUFFER_PADDING_SIZE;
}

LIBRARY_FUNC(jboolean, ffmpegHasDecoder, jstring codecName) {
  return getCodecByName(env, codecName) != NULL;
}

AUDIO_DECODER_FUNC(jlong, ffmpegInitialize, jstring codecName,
                   jbyteArray extraData, jboolean outputFloat,
                   jint rawSampleRate, jint rawChannelCount, jint blockAlign,
                   jlong bitRate) {
  const AVCodec* codec = getCodecByName(env, codecName);
  if (!codec) {
    LOGE("Codec not found.");
    return 0L;
  }
  return (jlong)createContext(env, codec, extraData, outputFloat, rawSampleRate,
                              rawChannelCount, blockAlign, bitRate);
}

AUDIO_DECODER_FUNC(jint, ffmpegDecode, jlong context, jobject inputData,
                   jint inputSize, jobject decoderOutputBuffer,
                   jobject outputData, jint outputSize) {
  if (!context) {
    LOGE("Context must be non-NULL.");
    return -1;
  }
  if (!inputData || !decoderOutputBuffer || !outputData) {
    LOGE("Input and output buffers must be non-NULL.");
    return -1;
  }
  if (inputSize < 0) {
    LOGE("Invalid input buffer size: %d.", inputSize);
    return -1;
  }
  if (outputSize < 0) {
    LOGE("Invalid output buffer length: %d", outputSize);
    return -1;
  }
  uint8_t* inputBuffer = (uint8_t*)env->GetDirectBufferAddress(inputData);
  uint8_t* outputBuffer = (uint8_t*)env->GetDirectBufferAddress(outputData);
  AVCodecContext* avContext = (AVCodecContext*)context;
  DecoderState* state = (DecoderState*)avContext->opaque;
  AVPacket* packet = state->packet;
  GrowOutputBufferCallback growBuffer{env, thiz, decoderOutputBuffer};
  OutputBuffer output = {outputBuffer, outputSize, 0, growBuffer};
  int ret;
  if (state->parser) {
    ret = decodeAccessUnits(avContext, state->parser, packet, inputBuffer,
                            inputSize, &output);
  } else {
    packet->data = inputBuffer;
    packet->size = inputSize;
    ret = decodePacket(avContext, packet, &output);
  }
  av_packet_unref(packet);
  return ret < 0 ? ret : output.written;
}

uint8_t* GrowOutputBufferCallback::operator()(int requiredSize) const {
  jobject newOutputData = env->CallObjectMethod(
      thiz, growOutputBufferMethod, decoderOutputBuffer, requiredSize);
  if (env->ExceptionCheck()) {
    LOGE("growOutputBuffer() failed");
    env->ExceptionDescribe();
    return nullptr;
  }
  return static_cast<uint8_t*>(env->GetDirectBufferAddress(newOutputData));
}

AUDIO_DECODER_FUNC(jint, ffmpegGetChannelCount, jlong context) {
  if (!context) {
    LOGE("Context must be non-NULL.");
    return -1;
  }
  AVCodecContext* avContext = (AVCodecContext*)context;
  DecoderState* state = (DecoderState*)avContext->opaque;
  return state->haveOutput ? state->outLayout.nb_channels
                           : avContext->ch_layout.nb_channels;
}

AUDIO_DECODER_FUNC(jint, ffmpegGetSampleRate, jlong context) {
  if (!context) {
    LOGE("Context must be non-NULL.");
    return -1;
  }
  AVCodecContext* avContext = (AVCodecContext*)context;
  DecoderState* state = (DecoderState*)avContext->opaque;
  return state->haveOutput ? state->outRate : avContext->sample_rate;
}

AUDIO_DECODER_FUNC(jlong, ffmpegReset, jlong jContext, jbyteArray extraData) {
  AVCodecContext* context = (AVCodecContext*)jContext;
  if (!context) {
    LOGE("Tried to reset without a context.");
    return 0L;
  }

  AVCodecID codecId = context->codec_id;
  if (codecId == AV_CODEC_ID_TRUEHD) {
    jboolean outputFloat =
        (jboolean)(context->request_sample_fmt == OUTPUT_FORMAT_PCM_FLOAT);
    // The output the Java side already reported carries over to the new
    // context
    DecoderState* old = (DecoderState*)context->opaque;
    AVChannelLayout outLayout = {};
    int outRate = old->outRate;
    int haveOutput = old->haveOutput;
    if (haveOutput && av_channel_layout_copy(&outLayout, &old->outLayout) < 0) {
      haveOutput = 0;
    }
    // Release and recreate the context if the codec is TrueHD.
    // TODO: Figure out why flushing doesn't work for this codec.
    releaseContext(context);
    const AVCodec* codec = avcodec_find_decoder(codecId);
    if (!codec) {
      LOGE("Unexpected error finding codec %d.", codecId);
      av_channel_layout_uninit(&outLayout);
      return 0L;
    }
    AVCodecContext* created =
        createContext(env, codec, extraData, outputFloat,
                      /* rawSampleRate= */ -1,
                      /* rawChannelCount= */ -1,
                      /* blockAlign= */ 0,
                      /* bitRate= */ 0);
    if (created && haveOutput) {
      DecoderState* state = (DecoderState*)created->opaque;
      if (av_channel_layout_copy(&state->outLayout, &outLayout) >= 0) {
        state->outRate = outRate;
        state->haveOutput = 1;
      }
    }
    av_channel_layout_uninit(&outLayout);
    return (jlong)created;
  }

  avcodec_flush_buffers(context);
  return (jlong)context;
}

AUDIO_DECODER_FUNC(void, ffmpegRelease, jlong context) {
  if (context) {
    releaseContext((AVCodecContext*)context);
  }
}

const AVCodec* getCodecByName(JNIEnv* env, jstring codecName) {
  if (!codecName) {
    return NULL;
  }
  const char* codecNameChars = env->GetStringUTFChars(codecName, NULL);
  const AVCodec* codec = avcodec_find_decoder_by_name(codecNameChars);
  env->ReleaseStringUTFChars(codecName, codecNameChars);
  return codec;
}

AVCodecContext* createContext(JNIEnv* env, const AVCodec* codec,
                              jbyteArray extraData, jboolean outputFloat,
                              jint rawSampleRate, jint rawChannelCount,
                              jint blockAlign, jlong bitRate) {
  AVCodecContext* context = avcodec_alloc_context3(codec);
  if (!context) {
    LOGE("Failed to allocate context.");
    return NULL;
  }
  context->request_sample_fmt =
      outputFloat ? OUTPUT_FORMAT_PCM_FLOAT : OUTPUT_FORMAT_PCM_16BIT;
  if (extraData) {
    jsize size = env->GetArrayLength(extraData);
    context->extradata_size = size;
    // Zeroed, padding included, which the bitstream reader may run into
    context->extradata =
        (uint8_t*)av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!context->extradata) {
      LOGE("Failed to allocate extradata.");
      releaseContext(context);
      return NULL;
    }
    env->GetByteArrayRegion(extraData, 0, size, (jbyte*)context->extradata);
  }
  // Windows Media Audio, like raw mu-law and A-law, has its rate and channels
  // only in the container. wmapro and wmalossless then take the channel mask
  // from the extradata.
  bool wma = context->codec_id == AV_CODEC_ID_WMAV1 ||
             context->codec_id == AV_CODEC_ID_WMAV2 ||
             context->codec_id == AV_CODEC_ID_WMAPRO ||
             context->codec_id == AV_CODEC_ID_WMALOSSLESS;
  if (context->codec_id == AV_CODEC_ID_PCM_MULAW ||
      context->codec_id == AV_CODEC_ID_PCM_ALAW ||
      (wma && rawSampleRate > 0 && rawChannelCount > 0)) {
    context->sample_rate = rawSampleRate;
    av_channel_layout_default(&context->ch_layout, rawChannelCount);
  }
  if (blockAlign > 0) {
    context->block_align = blockAlign;
  }
  if (bitRate > 0) {
    context->bit_rate = bitRate;
  }
  context->err_recognition = AV_EF_IGNORE_ERR;
  int result = avcodec_open2(context, codec, NULL);
  if (result < 0) {
    logError("avcodec_open2", result);
    releaseContext(context);
    return NULL;
  }
  DecoderState* state = (DecoderState*)av_mallocz(sizeof(DecoderState));
  if (!state) {
    LOGE("Failed to allocate decoder state.");
    releaseContext(context);
    return NULL;
  }
  // Set straight away, so a failure below releases whatever was made
  context->opaque = state;
  state->packet = av_packet_alloc();
  state->frame = av_frame_alloc();
  if (!state->packet || !state->frame) {
    LOGE("Failed to allocate packet or frame.");
    releaseContext(context);
    return NULL;
  }
  if (context->codec_id == AV_CODEC_ID_TRUEHD) {
    // Media3's MatroskaExtractor glues 16 TrueHD access units into one sample,
    // which is what the MediaCodec passthrough path wants, but this decoder
    // takes one unit per packet and drops the rest of what it is given. The
    // mlp parser cuts a sample back into the units it was made of, and reads
    // the major sync into the context on the way, which the decoder needs
    // since nothing here sets a channel layout for it.
    state->parser = av_parser_init(AV_CODEC_ID_TRUEHD);
    if (!state->parser) {
      LOGE("Failed to allocate TrueHD parser.");
      releaseContext(context);
      return NULL;
    }
  }
  return context;
}

uint8_t* OutputBuffer::reserve(int bytes) {
  if (written + bytes > size) {
    LOGD(
        "Output buffer size (%d) too small for output data (%d), "
        "reallocating buffer.",
        size, written + bytes);
    size = written + bytes;
    // The new buffer starts with a copy of everything written so far, so the
    // offset stays put and only the base moves.
    data = growBuffer(size);
    if (!data) {
      return nullptr;
    }
  }
  return data + written;
}

int decodeAccessUnits(AVCodecContext* context, AVCodecParserContext* parser,
                      AVPacket* packet, const uint8_t* input, int inputSize,
                      OutputBuffer* output) {
  int idle = 0;
  while (inputSize > 0) {
    uint8_t* unit = NULL;
    int unitSize = 0;
    int used = av_parser_parse2(parser, context, &unit, &unitSize, input,
                                inputSize, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (used < 0) {
      logError("av_parser_parse2", used);
      return transformError(used);
    }
    input += used;
    inputSize -= used;
    if (used > 0 || unitSize > 0) {
      idle = 0;
    } else if (++idle > 1) {
      // Nothing taken and nothing handed back twice over, so there is no whole
      // unit left in what remains.
      break;
    }
    if (unitSize <= 0) {
      // The parser is holding on to a unit it has not finished.
      continue;
    }
    packet->data = unit;
    packet->size = unitSize;
    int result = decodePacket(context, packet, output);
    if (result < 0) {
      return result;
    }
  }
  return 0;
}

// Converts count samples of input, or when in is NULL whatever the resampler
// still holds, and appends them to the output. What swr_convert says it wrote
// is what counts, not the room reserved for it.
static int convertSamples(DecoderState* state, AVSampleFormat outFormat,
                          const uint8_t** in, int count, OutputBuffer* output) {
  int outSamples = swr_get_out_samples(state->resampleContext, count);
  if (outSamples < 0) {
    logError("swr_get_out_samples", outSamples);
    return AUDIO_DECODER_ERROR_INVALID_DATA;
  }
  if (outSamples == 0) {
    return 0;
  }
  int frameSize =
      av_get_bytes_per_sample(outFormat) * state->outLayout.nb_channels;
  uint8_t* out = output->reserve(frameSize * outSamples);
  if (!out) {
    LOGE("Failed to reallocate output buffer.");
    return AUDIO_DECODER_ERROR_OTHER;
  }
  int converted =
      swr_convert(state->resampleContext, &out, outSamples, in, count);
  if (converted < 0) {
    logError("swr_convert", converted);
    return AUDIO_DECODER_ERROR_INVALID_DATA;
  }
  output->written += converted * frameSize;
  return 0;
}

// Builds the resampler for the frame's input when it is not the input the last
// one was built for, since a stream can change its layout, rate or sample
// format part way through. The output stays what the first frame set, which is
// what the sink was configured with, so a change is remixed into it.
static int prepareResampler(AVCodecContext* context, DecoderState* state,
                            const AVFrame* frame, OutputBuffer* output) {
  const AVChannelLayout* inLayout = frame->ch_layout.nb_channels > 0
                                        ? &frame->ch_layout
                                        : &context->ch_layout;
  int inFormat = frame->format;
  int inRate =
      frame->sample_rate > 0 ? frame->sample_rate : context->sample_rate;
  if (state->resampleContext && state->inFormat == inFormat &&
      state->inRate == inRate &&
      av_channel_layout_compare(&state->inLayout, inLayout) == 0) {
    return 0;
  }
  int result;
  if (!state->haveOutput) {
    result = av_channel_layout_copy(&state->outLayout, inLayout);
    if (result < 0) {
      logError("av_channel_layout_copy", result);
      return AUDIO_DECODER_ERROR_OTHER;
    }
    state->outRate = inRate;
    state->haveOutput = 1;
  }
  if (state->resampleContext) {
    LOGD("Input now %d channels at %d Hz, format %d, resampler rebuilt.",
         inLayout->nb_channels, inRate, inFormat);
    // What the old one still holds goes out before it is let go of
    result = convertSamples(state, context->request_sample_fmt, NULL, 0,
                            output);
    swr_free(&state->resampleContext);
    if (result < 0) {
      return result;
    }
  }
  SwrContext* resampleContext = NULL;
  result = swr_alloc_set_opts2(&resampleContext,             // ps
                               &state->outLayout,            // out_ch_layout
                               context->request_sample_fmt,  // out_sample_fmt
                               state->outRate,               // out_sample_rate
                               inLayout,                     // in_ch_layout
                               (AVSampleFormat)inFormat,     // in_sample_fmt
                               inRate,                       // in_sample_rate
                               0,                            // log_offset
                               NULL                          // log_ctx
  );
  if (result < 0) {
    logError("swr_alloc_set_opts2", result);
    swr_free(&resampleContext);
    return transformError(result);
  }
  result = swr_init(resampleContext);
  if (result < 0) {
    logError("swr_init", result);
    swr_free(&resampleContext);
    return transformError(result);
  }
  result = av_channel_layout_copy(&state->inLayout, inLayout);
  if (result < 0) {
    logError("av_channel_layout_copy", result);
    swr_free(&resampleContext);
    return AUDIO_DECODER_ERROR_OTHER;
  }
  state->resampleContext = resampleContext;
  state->inFormat = inFormat;
  state->inRate = inRate;
  return 0;
}

int decodePacket(AVCodecContext* context, AVPacket* packet,
                 OutputBuffer* output) {
  int result = 0;
  // Queue input data.
  result = avcodec_send_packet(context, packet);
  if (result) {
    logError("avcodec_send_packet", result);
    return transformError(result);
  }

  // Dequeue output data until it runs out.
  DecoderState* state = static_cast<DecoderState*>(context->opaque);
  AVFrame* frame = state->frame;
  while (true) {
    result = avcodec_receive_frame(context, frame);
    if (result) {
      if (result == AVERROR(EAGAIN)) {
        break;
      }
      logError("avcodec_receive_frame", result);
      return transformError(result);
    }

    // Resample output.
    result = prepareResampler(context, state, frame, output);
    if (result >= 0) {
      result = convertSamples(state, context->request_sample_fmt,
                              (const uint8_t**)frame->extended_data,
                              frame->nb_samples, output);
    }
    av_frame_unref(frame);
    if (result < 0) {
      return result;
    }
  }
  return 0;
}

int transformError(int errorNumber) {
  return errorNumber == AVERROR_INVALIDDATA ? AUDIO_DECODER_ERROR_INVALID_DATA
                                            : AUDIO_DECODER_ERROR_OTHER;
}

void logError(const char* functionName, int errorNumber) {
  char* buffer = (char*)malloc(ERROR_STRING_BUFFER_LENGTH * sizeof(char));
  av_strerror(errorNumber, buffer, ERROR_STRING_BUFFER_LENGTH);
  LOGE("Error in %s: %s", functionName, buffer);
  free(buffer);
}

void releaseContext(AVCodecContext* context) {
  if (!context) {
    return;
  }
  DecoderState* state = (DecoderState*)context->opaque;
  if (state) {
    if (state->resampleContext) {
      swr_free(&state->resampleContext);
    }
    if (state->parser) {
      av_parser_close(state->parser);
    }
    av_packet_free(&state->packet);
    av_frame_free(&state->frame);
    av_channel_layout_uninit(&state->inLayout);
    av_channel_layout_uninit(&state->outLayout);
    av_free(state);
    context->opaque = NULL;
  }
  avcodec_free_context(&context);
}
