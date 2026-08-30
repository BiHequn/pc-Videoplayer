#include "ffmpeg-decoder.h"
#include<Windows.h>
#include<mmsystem.h>

#include <iostream>
#include<vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include<libswscale/swscale.h>
#include<libswresample/swresample.h>
}

static AVFormatContext* g_formatContext = nullptr;
static AVCodecContext* g_codecContext = nullptr;
static int g_videoStreamIndex = -1;

static AVCodecContext* g_audioCodecContext = nullptr;
static int g_audioStreamIndex = -1;

static SwrContext* g_audioResampleContext = nullptr;
static std::vector<unsigned char>g_audioPcmBuffer;
static int g_audioSampleRate = 0;
static int g_audioChannels = 0;
static bool g_audioPcmLogged = false;

static SwsContext* g_scaleContext = nullptr;
static std::vector<unsigned char>g_bgraBuffer;
static unsigned int g_frameIntervalMs = 33;
//pts
static int g_videoPtsLogCount = 0;
static double g_audioStartPtsSeconds = -1.0;
static int g_audioPtsLogCount = 0;

static HWAVEOUT g_audioOutput = nullptr;

static const int kAudioBufferCount = 4;
static std::vector<unsigned char>
  g_audioBuffers[kAudioBufferCount];
static WAVEHDR
g_audioHeaders[kAudioBufferCount] = {};
static int g_nextAudioBuffer = 0;

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
  //根据视频流帧率计算每帧间隔
  AVRational frameRate = av_guess_frame_rate(
      g_formatContext,
      videoStream,
      nullptr);
  if (frameRate.num >0 &&  frameRate.den > 0)
  {
      double fps =
          static_cast<double>(frameRate.num) /
          static_cast<double>(frameRate.den);
      if (fps >= 1.0 && fps <= 240.0)
      {
          g_frameIntervalMs =
              static_cast<unsigned int>(1000.0 / fps + 0.5);
          if (g_frameIntervalMs == 0)
              g_frameIntervalMs = 1;
      }

  }

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
  //音频解码
  const AVCodec* audioDecoder = nullptr;

  // 查找音频流。没有音频时仍然允许只播放画面。
  int audioResult = av_find_best_stream(
      g_formatContext,
      AVMEDIA_TYPE_AUDIO,
      -1,
      -1,
      &audioDecoder,
      0);

  if (audioResult >= 0)
  {
      g_audioStreamIndex = audioResult;
      g_audioCodecContext = avcodec_alloc_context3(audioDecoder);

      if (!g_audioCodecContext)
      {
          std::cerr
              << "avcodec_alloc_context3 for audio failed."
              << std::endl;

          closeFfmpegVideo();
          return false;
      }

      AVStream* audioStream =
          g_formatContext->streams[g_audioStreamIndex];

      audioResult = avcodec_parameters_to_context(
          g_audioCodecContext,
          audioStream->codecpar);

      if (audioResult < 0)
      {
          printFfmpegError(
              "avcodec_parameters_to_context for audio",
              audioResult);

          closeFfmpegVideo();
          return false;
      }

      audioResult = avcodec_open2(
          g_audioCodecContext,
          audioDecoder,
          nullptr);

      if (audioResult < 0)
      {
          printFfmpegError(
              "avcodec_open2 for audio",
              audioResult);

          closeFfmpegVideo();
          return false;
      }

      std::cerr
          << "Opened audio: "
          << audioDecoder->name
          << ", "
          << g_audioCodecContext->sample_rate
          << " Hz, "
          << g_audioCodecContext->ch_layout.nb_channels
          << " channels"
          << std::endl;    
    // 把 AAC 解码器输出的 float planar 转成 S16 交错 PCM。
      int swrResult = swr_alloc_set_opts2(
          &g_audioResampleContext,
          &g_audioCodecContext->ch_layout,
          AV_SAMPLE_FMT_S16,
          g_audioCodecContext->sample_rate,
          &g_audioCodecContext->ch_layout,
          g_audioCodecContext->sample_fmt,
          g_audioCodecContext->sample_rate,
          0,
          nullptr);

      if (swrResult < 0 || !g_audioResampleContext)
      {
          printFfmpegError("swr_alloc_set_opts2", swrResult);
          closeFfmpegVideo();
          return false;
      }

      swrResult = swr_init(g_audioResampleContext);
      if (swrResult < 0)
      {
          printFfmpegError("swr_init", swrResult);
          closeFfmpegVideo();
          return false;
      }

      g_audioSampleRate = g_audioCodecContext->sample_rate;
      g_audioChannels = g_audioCodecContext->ch_layout.nb_channels;

      //使用ffmpeg转换后的S16 PCM格式打开系统音频设备
      WAVEFORMATEX audioFormat = {};
      audioFormat.wFormatTag = WAVE_FORMAT_PCM;
      audioFormat.nChannels =
          static_cast<WORD>(g_audioChannels);
      audioFormat.nSamplesPerSec = g_audioSampleRate;
      audioFormat.wBitsPerSample = 16;
      audioFormat.nBlockAlign =
          audioFormat.nChannels *
          audioFormat.wBitsPerSample / 8;
      audioFormat.nAvgBytesPerSec =
          audioFormat.nSamplesPerSec *
          audioFormat.nBlockAlign;

      MMRESULT waveResult = waveOutOpen(
          &g_audioOutput,
          WAVE_MAPPER,
          &audioFormat,
          0,
          0,
          CALLBACK_NULL);

      if (waveResult != MMSYSERR_NOERROR)
      {
          std::cerr << "waveOutOpen failed: "
              << waveResult
              << std::endl;

          // 音频设备打开失败时，仍然允许继续播放画面。
          g_audioOutput = nullptr;
      }

  }
  else
  {
      // 没有音频流不是错误，继续进行无声视频播放。
      g_audioStreamIndex = -1;
      std::cerr << "No audio stream found." << std::endl;
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



static bool convertFrameToBgra(const AVFrame* frame,DecodedVideoFrame* output)
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
bool decodeNextFfmpegFrame(DecodedVideoFrame* output)
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

        std::cerr
            << "Failed to allocate packet or frame."
            << std::endl;
        return false;
    }

    bool decoded = false;

    // 不断读取压缩包，直到视频解码器输出一帧。
    while (av_read_frame(g_formatContext, packet) >= 0)
    {
        // 遇到音频包时，解码并转换成 S16 交错 PCM。
        if (g_audioCodecContext &&
            g_audioResampleContext &&
            packet->stream_index == g_audioStreamIndex)
        {
            int audioResult = avcodec_send_packet(
                g_audioCodecContext,
                packet);

            av_packet_unref(packet);

            if (audioResult < 0)
            {
                printFfmpegError(
                    "avcodec_send_packet for audio",
                    audioResult);
                continue;
            }

            audioResult = avcodec_receive_frame(
                g_audioCodecContext,
                frame);

            if (audioResult == AVERROR(EAGAIN))
                continue;

            if (audioResult < 0)
            {
                printFfmpegError(
                    "avcodec_receive_frame for audio",
                    audioResult);
                continue;
            }

            // S16 每个声道的一个采样占 2 字节。
            const size_t pcmSize =
                static_cast<size_t>(frame->nb_samples) *
                g_audioChannels *
                2;

            g_audioPcmBuffer.resize(pcmSize);

            unsigned char* outputData[1] = {
                g_audioPcmBuffer.data()
            };

            int convertedSamples = swr_convert(
                g_audioResampleContext,
                outputData,
                frame->nb_samples,
                const_cast<const unsigned char**>(
                    frame->extended_data),
                frame->nb_samples);

            if (convertedSamples > 0)
            {
                double audioPtsSeconds = -1.0;
                if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                {
                    AVStream* audioStream = g_formatContext->streams[g_audioStreamIndex];
                    audioPtsSeconds = frame->best_effort_timestamp* av_q2d(audioStream->time_base);
                    if (g_audioStartPtsSeconds < 0.0)
                    {
                        g_audioStartPtsSeconds = audioPtsSeconds;
                    }
                }
                if (g_audioPtsLogCount < 10)
                {
                    std::cerr << "Audio PTS: " << audioPtsSeconds << " seconds" << std::endl;
                    ++g_audioPtsLogCount;
                }
                g_audioPcmBuffer.resize(
                    static_cast<size_t>(convertedSamples) *
                    g_audioChannels *
                    2);

                if (g_audioOutput)
                {
                    WAVEHDR& audioHeader =
                        g_audioHeaders[g_nextAudioBuffer];

                    // 缓冲区仍在播放时，跳过当前音频帧，避免阻塞窗口线程。
                    if ((audioHeader.dwFlags & WHDR_PREPARED) != 0 &&
                        (audioHeader.dwFlags & WHDR_DONE) == 0)
                    {
                        while ((audioHeader.dwFlags & WHDR_DONE) == 0)
                        {
                            Sleep(1);
                        }
                        }

                    // 释放已经播放完成的旧 Header。
                    if ((audioHeader.dwFlags & WHDR_PREPARED) != 0)
                    {
                        waveOutUnprepareHeader(
                            g_audioOutput,
                            &audioHeader,
                            sizeof(audioHeader));

                        audioHeader = {};
                    }

                    std::vector<unsigned char>& audioBuffer =
                        g_audioBuffers[g_nextAudioBuffer];

                    // 异步播放期间必须保证这块 PCM 内存一直存在。
                    audioBuffer = g_audioPcmBuffer;

                    audioHeader.lpData =
                        reinterpret_cast<LPSTR>(
                            audioBuffer.data());

                    audioHeader.dwBufferLength =
                        static_cast<DWORD>(
                            audioBuffer.size());

                    MMRESULT prepareResult = waveOutPrepareHeader(
                        g_audioOutput,
                        &audioHeader,
                        sizeof(audioHeader));

                    if (prepareResult == MMSYSERR_NOERROR)
                    {
                        MMRESULT writeResult = waveOutWrite(
                            g_audioOutput,
                            &audioHeader,
                            sizeof(audioHeader));

                        if (writeResult == MMSYSERR_NOERROR)
                        {
                            g_nextAudioBuffer =
                                (g_nextAudioBuffer + 1) %
                                kAudioBufferCount;
                        }
                        else
                        {
                            waveOutUnprepareHeader(
                                g_audioOutput,
                                &audioHeader,
                                sizeof(audioHeader));

                            audioHeader = {};
                        }
                    }
                }

                if (!g_audioPcmLogged)
                {
                    std::cerr
                        << "Audio PCM decoded: "
                        << g_audioSampleRate
                        << " Hz, "
                        << g_audioChannels
                        << " channels, "
                        << g_audioPcmBuffer.size()
                        << " bytes"
                        << std::endl;

                    g_audioPcmLogged = true;
                }
            }

            av_frame_unref(frame);
            continue;
        }

        // 其他无关数据流直接跳过。
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
            printFfmpegError(
                "avcodec_send_packet",
                result);
            break;
        }

        result = avcodec_receive_frame(
            g_codecContext,
            frame);

        if (result == AVERROR(EAGAIN))
            continue;

        if (result < 0)
        {
            printFfmpegError(
                "avcodec_receive_frame",
                result);
            break;
        }

        decoded = convertFrameToBgra(frame, output);

        if (decoded)
        {
            if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
            {
                AVStream* videoStream =
                    g_formatContext->streams[g_videoStreamIndex];

                output->ptsSeconds =
                    frame->best_effort_timestamp *
                    av_q2d(videoStream->time_base);
            }
            else
            {
                // -1 表示当前视频帧没有有效时间戳。
                output->ptsSeconds = -1.0;
            }

            // 暂时只打印前 10 帧，避免控制台持续刷屏。
            if (g_videoPtsLogCount < 10)
            {
                std::cerr
                    << "Video PTS: "
                    << output->ptsSeconds
                    << " seconds"
                    << std::endl;

                ++g_videoPtsLogCount;
            }
        }

        break;
    }

    av_packet_free(&packet);
    av_frame_free(&frame);

    return decoded;
}

unsigned int getFfmpegFrameIntervalMs()
{
    return g_frameIntervalMs;
}

void closeFfmpegVideo()
{
    if (g_audioOutput)
    {
        // 停止所有尚未播放完成的音频。
        waveOutReset(g_audioOutput);

        for (int i = 0; i < kAudioBufferCount; ++i)
        {
            if ((g_audioHeaders[i].dwFlags &
                WHDR_PREPARED) != 0)
            {
                waveOutUnprepareHeader(
                    g_audioOutput,
                    &g_audioHeaders[i],
                    sizeof(g_audioHeaders[i]));
            }

            g_audioHeaders[i] = {};
            g_audioBuffers[i].clear();
        }

        waveOutClose(g_audioOutput);
        g_audioOutput = nullptr;
    }

    g_nextAudioBuffer = 0;

  if (g_scaleContext)
  {
    sws_freeContext(g_scaleContext);
    g_scaleContext = nullptr;
  }
  g_bgraBuffer.clear();

  if (g_audioCodecContext)
      avcodec_free_context(&g_audioCodecContext);

  // 解码器依赖媒体输入，因此先释放解码器。
  if (g_codecContext)
    avcodec_free_context(&g_codecContext);

  if (g_formatContext)
    avformat_close_input(&g_formatContext);

  //音频转换器释放和状态复位
  if (g_audioResampleContext)
  {
      swr_free(&g_audioResampleContext);
  }
  g_audioPcmBuffer.clear();
  g_audioSampleRate = 0;
  g_audioChannels = 0;
  g_audioPcmLogged = false;

  g_videoStreamIndex = -1;
  g_frameIntervalMs = 33;
  g_audioStreamIndex = -1;
  g_videoPtsLogCount = 0;

  g_audioPtsLogCount = 0;
  g_audioStartPtsSeconds = -1.0;

}

void setFfmpegAudioPaused(bool paused)
{
    if (!g_audioOutput)
        return;
    if (paused)
    {
        waveOutPause(g_audioOutput);
    }
    else
    {
        waveOutRestart(g_audioOutput);
    }
}

//建立waveout实际时钟，用来观测
double getFfmpegAudioClockSeconds()
{
    if(!g_audioOutput || g_audioSampleRate <= 0 || g_audioStartPtsSeconds < 0.0)
    {
        return -1.0;
    }
    MMTIME position = {};
    position.wType = TIME_SAMPLES;
    MMRESULT result = waveOutGetPosition(
       g_audioOutput, &position, sizeof(position));
    if (result != MMSYSERR_NOERROR)
    {
        return -1.0;
    }
    if (position.wType == TIME_SAMPLES)
    {
        return g_audioStartPtsSeconds + static_cast<double>(position.u.sample) / g_audioSampleRate;
    }
    //部分音频设备可能自己返回已经播放的字节数
    if (position.wType == TIME_BYTES)
    {
        int bytesPerSeconds = g_audioSampleRate * g_audioChannels * 2;
        return g_audioStartPtsSeconds + static_cast<double>(position.u.cb) / bytesPerSeconds;
    }
    return -1.0;
    
}