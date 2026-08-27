#include "ffmpeg-decoder.h"

#include <iostream>
#include<vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include<libswscale/swscale.h>
}

static AVFormatContext* g_formatContext = nullptr;
static AVCodecContext* g_codecContext = nullptr;
static int g_videoStreamIndex = -1;
static SwsContext* g_scaleContext = nullptr;
static std::vector<unsigned char>g_bgraBuffer;

static void printFfmpegError(const char* operation, int errorCode)
{
  char errorText[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(errorCode, errorText, sizeof(errorText));

  std::cerr
    << operation
    << " failed: "
    << errorText
    << std::endl;
}

bool openFfmpegVideo(const char* source)
{
  if (!source || source[0] == '\0')
  {
    std::cerr << "Video source is missing." << std::endl;
    return false;
  }

  closeFfmpegVideo();

  // FFmpeg 自动识别本地文件或网络 URL 的封装格式。
  int result = avformat_open_input(
    &g_formatContext,
    source,
    nullptr,
    nullptr);

  if (result < 0)
  {
    printFfmpegError("avformat_open_input", result);
    closeFfmpegVideo();
    return false;
  }

  // 读取 MP4 中的视频流、音频流等基本信息。
  result = avformat_find_stream_info(
    g_formatContext,
    nullptr);

  if (result < 0)
  {
    printFfmpegError("avformat_find_stream_info", result);
    closeFfmpegVideo();
    return false;
  }

  const AVCodec* decoder = nullptr;

  // 找到最合适的视频流以及对应解码器。
  result = av_find_best_stream(
    g_formatContext,
    AVMEDIA_TYPE_VIDEO,
    -1,
    -1,
    &decoder,
    0);

  if (result < 0)
  {
    printFfmpegError("av_find_best_stream", result);
    closeFfmpegVideo();
    return false;
  }

  g_videoStreamIndex = result;
  g_codecContext = avcodec_alloc_context3(decoder);

  if (!g_codecContext)
  {
    std::cerr << "avcodec_alloc_context3 failed." << std::endl;
    closeFfmpegVideo();
    return false;
  }

  AVStream* videoStream =
    g_formatContext->streams[g_videoStreamIndex];

  result = avcodec_parameters_to_context(
    g_codecContext,
    videoStream->codecpar);

  if (result < 0)
  {
    printFfmpegError("avcodec_parameters_to_context", result);
    closeFfmpegVideo();
    return false;
  }

  result = avcodec_open2(
    g_codecContext,
    decoder,
    nullptr);

  if (result < 0)
  {
    printFfmpegError("avcodec_open2", result);
    closeFfmpegVideo();
    return false;
  }

  std::cerr
    << "Opened video: "
    << decoder->name
    << ", "
    << g_codecContext->width
    << "x"
    << g_codecContext->height
    << std::endl;

  return true;
}

static bool convertFrameToBgra(
  const AVFrame* frame,
  DecodedVideoFrame* output)
{
  if (!frame || !output ||
    frame->width <= 0 || frame->height <= 0)
  {
    return false;
  }

  // 创建或复用 YUV 到 BGRA 的颜色转换器。
  g_scaleContext = sws_getCachedContext(
    g_scaleContext,
    frame->width,
    frame->height,
    static_cast<AVPixelFormat>(frame->format),
    frame->width,
    frame->height,
    AV_PIX_FMT_BGRA,
    SWS_BILINEAR,
    nullptr,
    nullptr,
    nullptr);

  if (!g_scaleContext)
  {
    std::cerr << "sws_getCachedContext failed." << std::endl;
    return false;
  }

  const int stride = frame->width * 4;
  const size_t bufferSize =
    static_cast<size_t>(stride) * frame->height;

  g_bgraBuffer.resize(bufferSize);

  unsigned char* destinationData[4] = {
      g_bgraBuffer.data(),
      nullptr,
      nullptr,
      nullptr
  };

  int destinationStride[4] = {
      stride,
      0,
      0,
      0
  };

  // 将解码器输出的 YUV 帧转换成连续 BGRA 像素。
  int convertedHeight = sws_scale(
    g_scaleContext,
    frame->data,
    frame->linesize,
    0,
    frame->height,
    destinationData,
    destinationStride);

  if (convertedHeight != frame->height)
  {
    std::cerr << "sws_scale failed." << std::endl;
    return false;
  }

  output->data = g_bgraBuffer.data();
  output->width = frame->width;
  output->height = frame->height;
  output->stride = stride;

  return true;
}

bool decodeFirstFfmpegFrame(DecodedVideoFrame* output)
{
  if (!output || !g_formatContext ||
    !g_codecContext || g_videoStreamIndex < 0)
  {
    return false;
  }

  *output = {};

  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();

  if (!packet || !frame)
  {
    av_packet_free(&packet);
    av_frame_free(&frame);

    std::cerr << "Failed to allocate packet or frame." << std::endl;
    return false;
  }

  bool decoded = false;

  // 不断读取压缩 Packet，直到解码器输出第一帧。
  while (av_read_frame(g_formatContext, packet) >= 0)
  {
    if (packet->stream_index != g_videoStreamIndex)
    {
      av_packet_unref(packet);
      continue;
    }

    int result = avcodec_send_packet(
      g_codecContext,
      packet);

    av_packet_unref(packet);

    if (result < 0)
    {
      printFfmpegError("avcodec_send_packet", result);
      break;
    }

    result = avcodec_receive_frame(
      g_codecContext,
      frame);

    // 解码器可能需要多个 Packet 才能输出一帧。
    if (result == AVERROR(EAGAIN))
      continue;

    if (result < 0)
    {
      printFfmpegError("avcodec_receive_frame", result);
      break;
    }

    decoded = convertFrameToBgra(frame, output);
    break;
  }

  av_packet_free(&packet);
  av_frame_free(&frame);

  if (decoded)
  {
    std::cerr
      << "Decoded first frame: "
      << output->width
      << "x"
      << output->height
      << ", BGRA"
      << std::endl;
  }

  return decoded;
}

void closeFfmpegVideo()
{
  if (g_scaleContext)
  {
    sws_freeContext(g_scaleContext);
    g_scaleContext = nullptr;
  }
  g_bgraBuffer.clear();

  // 解码器依赖媒体输入，因此先释放解码器。
  if (g_codecContext)
    avcodec_free_context(&g_codecContext);

  if (g_formatContext)
    avformat_close_input(&g_formatContext);

  g_videoStreamIndex = -1;
}