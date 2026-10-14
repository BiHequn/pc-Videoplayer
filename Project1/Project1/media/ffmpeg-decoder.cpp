#include"ffmpeg-decoder.h"
#include<Windows.h>
#include<mmsystem.h>
#include"media-packet-queue.h"
#include"video-frame-queue.h"
#include "video-frame-copy.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include<vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>

#include<libswresample/swresample.h>
}

extern "C"
{
 #include<libavcodec/bsf.h>
}

static AVFormatContext* g_formatContext = nullptr;

static int g_videoStreamIndex = -1;

static AVCodecContext* g_audioCodecContext = nullptr;
static int g_audioStreamIndex = -1;

static SwrContext* g_audioResampleContext = nullptr;
static std::vector<unsigned char>g_audioPcmBuffer;
static int g_audioSampleRate = 0;
static int g_audioChannels = 0;
static bool g_audioPcmLogged = false;

static unsigned int g_frameIntervalMs = 33;
//pts
static int g_videoPtsLogCount = 0;
static std::atomic<double> g_audioStartPtsSeconds(-1.0);
static int g_audioPtsLogCount = 0;

static HWAVEOUT g_audioOutput = nullptr;
static HANDLE g_audioCompletionEvent = nullptr;
static std::atomic<std::uint64_t> g_audioCompletions{0};
static void CALLBACK audioCompletion(HWAVEOUT,UINT message,DWORD_PTR instance,DWORD_PTR,DWORD_PTR) {
    if(message==WOM_DONE) {
        ++g_audioCompletions;
        // WaveOut callbacks must not call device functions or take playback locks.
        if(instance)SetEvent(reinterpret_cast<HANDLE>(instance));
    }
}

static const int kAudioBufferCount = 4;
static std::vector<unsigned char>
  g_audioBuffers[kAudioBufferCount];
static WAVEHDR
g_audioHeaders[kAudioBufferCount] = {};
static int g_nextAudioBuffer = 0;
static DWORD g_audioVolumeBeforeMute = 0xFFFFFFFF;
static DWORD g_audioDeviceOriginalVolume = 0xFFFFFFFF;
static bool g_audioMutedForRate = false;
static std::mutex g_audioOutputMutex;
static float g_audioVolume = 1.0f;

//增加线程处理队列
static MediaPacketQueue g_audioPacketQueue;
static MediaPacketQueue g_videoPacketQueue;
static VideoFrameQueue g_videoFrameQueue;

static std::thread g_demuxThread;
static std::thread g_audioDecodeThread;
static std::thread g_videoDecodeThread;

static std::atomic<bool> g_playbackRunning(false);
static std::atomic<bool> g_playbackPaused(false);
static std::atomic<bool> g_outputStarted(false);
static std::atomic<bool> g_videoDecodeFinished(false);
static std::atomic<bool> g_audioDecodeFinished(true);
static std::atomic<bool> g_abortRequested(false);
static std::mutex g_cancellationProbeMutex;
static bool (*g_cancellationProbe)(void*) = nullptr;
static void* g_cancellationUser = nullptr;
static std::atomic<ULONGLONG> g_previewDeadlineMs(0);
// Written only after joining decoders, then published by thread creation.
static double g_seekTargetSeconds = -1.0;
// Only the video decoder writes this; the worker reads it after the release/
// acquire publication through g_videoDecodeFinished, and resets after joining.
static QueuedVideoFrame g_previewTailFrame;
static std::atomic<PlaybackEndReason>
    g_playbackEndReason(PlaybackEndReason::None);
static QueuedVideoFrame g_displayFrame;
static std::atomic<double> g_playbackRate(1.0);
static double g_videoClockSeconds = 0.0;
static ULONGLONG g_videoClockLastTickMs = 0;
static bool g_videoClockValid = false;
static double g_durationSeconds = 0.0;
static VideoDecoderBackend g_effectiveBackend = VideoDecoderBackend::Auto;
static bool g_nativeYuvOutput = false;
static std::atomic<bool> g_buffering(false);

static const size_t kAudioPacketQueueSize = 32;
static const size_t kVideoPacketQueueSize = 16;
static const size_t kVideoFrameQueueSize = 6;

static void audioDecodeThreadMain();
static void videoDecodeThreadMain();
static bool startPlaybackThreads(bool paused = false, bool outputStarted = false);
static void applyFfmpegVolume();

static std::unique_ptr<IVideoDecoder> g_videoDecoder;
static AVBSFContext* g_videoBsf = nullptr;


static void printFfmpegError(const char* operation, int errorCode)
{
  char errorText[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(errorCode, errorText, sizeof(errorText));

  std::cerr << operation<< " failed: "<< errorText<< std::endl;
}

static bool externalCancellationRequested()
{
    std::lock_guard<std::mutex> lock(g_cancellationProbeMutex);
    return g_cancellationProbe && g_cancellationProbe(g_cancellationUser);
}

static bool ffmpegCancellationRequested()
{
    const ULONGLONG deadline = g_previewDeadlineMs.load();
    return g_abortRequested.load() || externalCancellationRequested() ||
        (deadline != 0 && GetTickCount64() >= deadline);
}

static int ffmpegInterruptCallback(void*)
{
    return ffmpegCancellationRequested() ? 1 : 0;
}

void setFfmpegCancellationProbe(bool (*probe)(void*), void* user)
{
    std::lock_guard<std::mutex> lock(g_cancellationProbeMutex);
    g_cancellationProbe = probe;
    g_cancellationUser = user;
}

static bool queueAudioPacket(const AVPacket* packet)
{
    // A rate change must also release a demux push already waiting for space.
    AVPacket* copy = av_packet_clone(packet);
    if (!copy)
        return false;
    std::unique_lock<std::mutex> lock(g_audioPacketQueue.mutex);
    while (!g_audioPacketQueue.stopped && g_playbackRunning.load() &&
        !ffmpegCancellationRequested() && g_playbackRate.load() == 1.0 &&
        g_outputStarted.load() && g_audioPacketQueue.packets.size() >= kAudioPacketQueueSize)
    {
        g_audioPacketQueue.condition.wait_for(lock, std::chrono::milliseconds(5));
    }
    const bool running = !g_audioPacketQueue.stopped && g_playbackRunning.load() &&
        !ffmpegCancellationRequested();
    if (running && g_playbackRate.load() == 1.0)
    {
        g_audioPacketQueue.packets.push_back(copy);
        copy = nullptr;
    }
    lock.unlock();
    av_packet_free(&copy);
    g_audioPacketQueue.condition.notify_all();
    return running;
}
//编码格式转换
static VideoCodec toVideoCodec(AVCodecID codecId)
{
    switch (codecId)
    {
        case AV_CODEC_ID_H264:
        return VideoCodec::H264;

        case AV_CODEC_ID_HEVC:
            return VideoCodec::Hevc;

        default:
            return VideoCodec::Unknown;
    }
}

static bool initializeVideoBsf(
    AVStream* stream,
    VideoDecoderConfig* config)
{
    if (!stream || !config)
        return false;

    const char* filterName = nullptr;

    if (stream->codecpar->codec_id == AV_CODEC_ID_H264)
        filterName = "h264_mp4toannexb";
    else if (stream->codecpar->codec_id == AV_CODEC_ID_HEVC)
        filterName = "hevc_mp4toannexb";
    else
        return true;

    const AVBitStreamFilter* filter =
        av_bsf_get_by_name(filterName);

    if (!filter)
        return false;

    if (av_bsf_alloc(filter, &g_videoBsf) < 0)
        return false;

    if (avcodec_parameters_copy(
        g_videoBsf->par_in,
        stream->codecpar) < 0)
    {
        av_bsf_free(&g_videoBsf);
        return false;
    }

    g_videoBsf->time_base_in = stream->time_base;

    if (av_bsf_init(g_videoBsf) < 0)
    {
        av_bsf_free(&g_videoBsf);
        return false;
    }

    if (g_videoBsf->par_out->extradata &&
        g_videoBsf->par_out->extradata_size > 0)
    {
        config->extraData.assign(
            g_videoBsf->par_out->extradata,
            g_videoBsf->par_out->extradata +
            g_videoBsf->par_out->extradata_size);
    }

    return true;
}

static void demuxThreadMain()
{
    AVPacket* packet = av_packet_alloc();
    if (!packet)
    {
        std::cerr
            << "av_packet_alloc for demux failed."
            << std::endl;

        g_playbackEndReason.store(
            PlaybackEndReason::DemuxError);

        finishMediaPacketQueue(&g_audioPacketQueue);
        finishMediaPacketQueue(&g_videoPacketQueue);
        return;
    }

    // 只有解封装线程可以调用 av_read_frame。
    while (g_playbackRunning.load() && !ffmpegCancellationRequested())
    {
        if (g_playbackPaused.load())
        {
            Sleep(1);
            continue;
        }

        const int result = av_read_frame(
            g_formatContext,
            packet);

        if (result < 0)
        {
            if (result == AVERROR_EOF)
            {
                g_playbackEndReason.store(
                    PlaybackEndReason::Completed);
            }
            else if (!ffmpegCancellationRequested())
            {
                printFfmpegError(
                    "av_read_frame",
                    result);

                g_playbackEndReason.store(
                    PlaybackEndReason::DemuxError);
            }

            break;
        }

        bool pushed = true;

        if (packet->stream_index == g_audioStreamIndex)
        {
            if (g_playbackRate.load() == 1.0)
                pushed = queueAudioPacket(packet);
        }
        else if (packet->stream_index == g_videoStreamIndex)
        {
            pushed = pushMediaPacket(
                &g_videoPacketQueue,
                packet,
                kVideoPacketQueueSize);
        }

        // 队列内部已经 clone，原始 packet 可以解除引用并复用。
        av_packet_unref(packet);

        if (!pushed)
        {
            break;
        }
    }

    // packet 本体必须等循环完全结束后再释放。
    av_packet_free(&packet);

    // 通知两个解码线程后面不会再有新的 packet。
    finishMediaPacketQueue(&g_audioPacketQueue);
    finishMediaPacketQueue(&g_videoPacketQueue);
}

bool openFfmpegVideo(
    const char* source,
    std::unique_ptr<IVideoDecoder> videoDecoder,
    VideoDecoderBackend backend)
{
  if (!source || source[0] == '\0' || !videoDecoder)
  {
    std::cerr << "Video source or decoder is missing." << std::endl;
    return false;
  }

  closeFfmpegVideo();
  g_abortRequested.store(false);
  if (ffmpegCancellationRequested())
      return false;
  g_videoDecoder = std::move(videoDecoder);
  g_playbackEndReason.store(
      PlaybackEndReason::None);

  g_formatContext = avformat_alloc_context();
  if (!g_formatContext)
  {
      std::cerr
          << "avformat_alloc_context failed."
          << std::endl;
      closeFfmpegVideo();
      return false;
  }

  g_formatContext->interrupt_callback.callback =
      ffmpegInterruptCallback;
  g_formatContext->interrupt_callback.opaque = nullptr;
  const bool networkSource=std::strstr(source, "://")!=nullptr;
  if(networkSource){g_formatContext->probesize=256*1024;g_formatContext->max_analyze_duration=500000;}

  // FFmpeg 自动识别本地文件或网络 URL 的封装格式。
  int result = avformat_open_input(
    &g_formatContext,
    source,
    nullptr,
    nullptr);

  if (result < 0 || ffmpegCancellationRequested())
  {
    printFfmpegError("avformat_open_input", result);
    closeFfmpegVideo();
    return false;
  }

  // 读取 MP4 中的视频流、音频流等基本信息。
  result = avformat_find_stream_info(
    g_formatContext,
    nullptr);

  if (result < 0 || ffmpegCancellationRequested())
  {
    printFfmpegError("avformat_find_stream_info", result);
    closeFfmpegVideo();
    return false;
  }

  // 找到最合适的视频流以及对应解码器。
  result = av_find_best_stream(
      g_formatContext,
      AVMEDIA_TYPE_VIDEO,
      -1,
      -1,
      nullptr,
    0);

  if (result < 0)
  {
    printFfmpegError("av_find_best_stream", result);
    closeFfmpegVideo();
    return false;
  }

  g_videoStreamIndex = result;
  

  AVStream* videoStream =
    g_formatContext->streams[g_videoStreamIndex];

  if (g_formatContext->duration != AV_NOPTS_VALUE &&
      g_formatContext->duration > 0)
  {
      g_durationSeconds =
          static_cast<double>(g_formatContext->duration) / AV_TIME_BASE;
  }
  else if (videoStream->duration != AV_NOPTS_VALUE &&
           videoStream->duration > 0 && videoStream->time_base.num > 0 &&
           videoStream->time_base.den > 0)
  {
      g_durationSeconds = videoStream->duration * av_q2d(videoStream->time_base);
  }

  AVCodecParameters* codecParameters =
      videoStream->codecpar;

  VideoDecoderConfig decoderConfig;
  decoderConfig.nativeYuv = g_nativeYuvOutput;
  decoderConfig.codec =
      toVideoCodec(codecParameters->codec_id);
  decoderConfig.ffmpegCodecId =
      static_cast<int>(codecParameters->codec_id);
  decoderConfig.width = codecParameters->width;
  decoderConfig.height = codecParameters->height;
  decoderConfig.timeBase.numerator =
      videoStream->time_base.num;
  decoderConfig.timeBase.denominator =
      videoStream->time_base.den;

  if (codecParameters->extradata &&codecParameters->extradata_size > 0)
  {
      decoderConfig.extraData.assign(codecParameters->extradata,codecParameters->extradata + codecParameters->extradata_size);
  }

  VideoDecoderConfig originalDecoderConfig =decoderConfig;

  VideoDecoderBackend activeBackend = backend;
  if (backend == VideoDecoderBackend::Auto &&
      g_formatContext->iformat &&
      g_formatContext->iformat->name &&
      std::strcmp(g_formatContext->iformat->name, "flv") == 0)
  {
      g_videoDecoder =
          createVideoDecoder(VideoDecoderBackend::Ffmpeg);
      if (!g_videoDecoder)
      {
          std::cerr
              << "FFmpeg decoder is unavailable for FLV."
              << std::endl;
          closeFfmpegVideo();
          return false;
      }

      activeBackend = VideoDecoderBackend::Ffmpeg;
      std::cerr
          << "Auto decoder uses FFmpeg for FLV compatibility."
          << std::endl;
  }

  if (activeBackend != VideoDecoderBackend::Ffmpeg)
  {
      if (!initializeVideoBsf(
          videoStream,
          &decoderConfig))
      {
          std::cerr
              << "Initialize video bitstream filter failed."
              << std::endl;

          closeFfmpegVideo();
          return false;
      }
  }

  if (!g_videoDecoder->initialize(decoderConfig))
  {
      if (activeBackend != VideoDecoderBackend::Auto)
      {
          std::cerr
              << "Requested decoder initialization failed."
              << std::endl;

          closeFfmpegVideo();
          return false;
      }

      std::cerr
          << "MF initialization failed, "
          "fallback to FFmpeg."
          << std::endl;

      g_videoDecoder.reset();

      av_bsf_free(&g_videoBsf);
      decoderConfig = originalDecoderConfig;

      g_videoDecoder = createVideoDecoder(VideoDecoderBackend::Ffmpeg);

      if (!g_videoDecoder ||
          !g_videoDecoder->initialize(decoderConfig))
      {
          std::cerr
              << "FFmpeg fallback initialization failed."
              << std::endl;

          closeFfmpegVideo();
          return false;
      }

      std::cerr
          << "Using FFmpeg software decoder fallback."
          << std::endl;
      activeBackend = VideoDecoderBackend::Ffmpeg;
  }

  // Auto's factory prefers MF when it is compiled in; otherwise it is software.
  if (activeBackend == VideoDecoderBackend::Auto)
  {
#if NP_ENABLE_MF_VIDEO_DECODER
      activeBackend = VideoDecoderBackend::MediaFoundation;
#else
      activeBackend = VideoDecoderBackend::Ffmpeg;
#endif
  }
  g_effectiveBackend = activeBackend;

  std::cerr
      << "Video decoder interface initialized: "
      << decoderConfig.width
      << "x"
      << decoderConfig.height
      << std::endl;

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

      g_audioCompletions=0;
      g_audioCompletionEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
      MMRESULT waveResult = g_audioCompletionEvent ? waveOutOpen(
          &g_audioOutput,
          WAVE_MAPPER,
          &audioFormat,
          reinterpret_cast<DWORD_PTR>(&audioCompletion),
          reinterpret_cast<DWORD_PTR>(g_audioCompletionEvent),
          CALLBACK_FUNCTION) : MMSYSERR_NOMEM;

      if (waveResult != MMSYSERR_NOERROR)
      {
          std::cerr << "waveOutOpen failed: "
              << waveResult
              << std::endl;

          // 音频设备打开失败时，仍然允许继续播放画面。
          g_audioOutput = nullptr;
          if(g_audioCompletionEvent){CloseHandle(g_audioCompletionEvent);g_audioCompletionEvent=nullptr;}
      }
      else
      {
          waveOutGetVolume(g_audioOutput, &g_audioDeviceOriginalVolume);
          applyFfmpegVolume();
      }
  }
  else
  {
      // 没有音频流不是错误，继续进行无声视频播放。
      g_audioStreamIndex = -1;
      std::cerr << "No audio stream found." << std::endl;
  }
  std::cerr
      << "Opened video stream: "
      << avcodec_get_name(
          codecParameters->codec_id)
      << ", "
      << codecParameters->width
      << "x"
      << codecParameters->height
      << std::endl;

  if (ffmpegCancellationRequested() || !startPlaybackThreads())
  {
      closeFfmpegVideo();
      return false;
  }

  return true;
}


//音频辅助函数
static void decodeAudioPacket(const AVPacket* packet, AVFrame* frame)
{
    int audioResult = avcodec_send_packet(
        g_audioCodecContext,
        packet);

    if (audioResult < 0)
    {
        printFfmpegError(
            "avcodec_send_packet for audio",
            audioResult);
        return;
    }

    while (g_playbackRunning.load())
    {
      audioResult = avcodec_receive_frame(
          g_audioCodecContext,
          frame);

      if (audioResult == AVERROR(EAGAIN) ||
          audioResult == AVERROR_EOF)
      {
          return;
      }

      if (audioResult < 0)
      {
          printFfmpegError(
              "avcodec_receive_frame for audio",
              audioResult);
          return;
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

    const int64_t resampleDelay = swr_get_delay(g_audioResampleContext, g_audioSampleRate);
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
            AVStream* audioStream =
                g_formatContext->streams[g_audioStreamIndex];

            audioPtsSeconds =
                frame->best_effort_timestamp *
                av_q2d(audioStream->time_base);

            audioPtsSeconds -= static_cast<double>(resampleDelay) / g_audioSampleRate;
        }

        int skippedSamples = 0;
        if (g_seekTargetSeconds >= 0.0)
        {
            // Unknown timestamps cannot safely contribute pre-target PCM.
            if (audioPtsSeconds < 0.0)
            {
                av_frame_unref(frame);
                continue;
            }
            const double skip = std::ceil((g_seekTargetSeconds - audioPtsSeconds) *
                g_audioSampleRate);
            if (skip >= convertedSamples)
            {
                av_frame_unref(frame);
                continue;
            }
            skippedSamples = static_cast<int>((std::max)(0.0, skip));
            audioPtsSeconds += static_cast<double>(skippedSamples) / g_audioSampleRate;
        }
        const size_t skippedBytes = static_cast<size_t>(skippedSamples) * g_audioChannels * 2;
        const size_t retainedBytes = static_cast<size_t>(convertedSamples - skippedSamples) *
            g_audioChannels * 2;
        if (skippedBytes != 0)
            std::memmove(g_audioPcmBuffer.data(), g_audioPcmBuffer.data() + skippedBytes,
                retainedBytes);

        if (g_audioPtsLogCount < 10)
        {
            std::cerr
                << "Audio PTS: "
                << audioPtsSeconds
                << " seconds"
                << std::endl;

            ++g_audioPtsLogCount;
        }

        g_audioPcmBuffer.resize(retainedBytes);

        if (g_audioOutput)
        {
            std::unique_lock<std::mutex> outputLock(g_audioOutputMutex);
            WAVEHDR& audioHeader =
                g_audioHeaders[g_nextAudioBuffer];

            // Release the control lock while waiting; muted audio must never
            // wait on stale WaveOut headers or submit newly decoded PCM.
            while (g_playbackRunning.load() && !ffmpegCancellationRequested() &&
                g_playbackRate.load() == 1.0 &&
                (!g_outputStarted.load() || g_playbackPaused.load() ||
                 ((audioHeader.dwFlags & WHDR_PREPARED) != 0 &&
                  (audioHeader.dwFlags & WHDR_DONE) == 0)))
            {
                const bool awaitingCompletion=(audioHeader.dwFlags&WHDR_PREPARED)!=0 && (audioHeader.dwFlags&WHDR_DONE)==0;
                outputLock.unlock();
                if(awaitingCompletion&&g_audioCompletionEvent)WaitForSingleObject(g_audioCompletionEvent,10);
                else Sleep(1);
                outputLock.lock();
            }
            if (!g_playbackRunning.load() || ffmpegCancellationRequested() ||
                g_playbackRate.load() != 1.0 || g_audioMutedForRate)
            {
                av_frame_unref(frame);
                return;
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

            MMRESULT prepareResult =
                waveOutPrepareHeader(
                    g_audioOutput,
                    &audioHeader,
                    sizeof(audioHeader));

            if (prepareResult == MMSYSERR_NOERROR)
            {
                MMRESULT writeResult =
                    waveOutWrite(
                        g_audioOutput,
                        &audioHeader,
                        sizeof(audioHeader));

                if (writeResult == MMSYSERR_NOERROR)
                {
                    if (g_audioStartPtsSeconds.load() < 0.0 && audioPtsSeconds >= 0.0)
                        g_audioStartPtsSeconds.store(audioPtsSeconds);
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
    }
}

static void audioDecodeThreadMain()
{
    AVFrame* frame = av_frame_alloc();
    if (!frame)
    {
        std::cerr << "av_frame_alloc for audio failed" << std::endl;
        g_playbackRunning.store(false);
        g_audioDecodeFinished.store(true);
        stopMediaPacketQueue(&g_audioPacketQueue);
        stopMediaPacketQueue(&g_videoPacketQueue);
        stopVideoFrameQueue(&g_videoFrameQueue);
        return;
    }
    while (g_playbackRunning.load() && !ffmpegCancellationRequested())
    {
        if (!g_outputStarted.load() && g_playbackRate.load() == 1.0)
        {
            Sleep(1);
            continue;
        }

        if (g_playbackPaused.load())
        {
            Sleep(1);
            continue;
        }

        AVPacket* packet = popMediaPacket(&g_audioPacketQueue);
        //队列完成，停止或者已经没有剩余packet
        if (!packet)
        {
            if (g_playbackRunning.load())
                decodeAudioPacket(nullptr, frame);
            break;
        }
        if (g_playbackRate.load() == 1.0)
            decodeAudioPacket(packet, frame);
        //队列返回的是独立clone的packet,消费完成后由音频线程负责释放
        av_packet_free(&packet);
    }
    av_frame_free(&frame);

    // 文件结束时让已经交给 WaveOut 的尾部音频播完。
    while (g_playbackRunning.load() && !ffmpegCancellationRequested() &&
        g_audioOutput && g_playbackRate.load() == 1.0)
    {
        bool allDone = true;

        for (int i = 0; i < kAudioBufferCount; ++i)
        {
            if ((g_audioHeaders[i].dwFlags &
                WHDR_PREPARED) != 0 &&
                (g_audioHeaders[i].dwFlags &
                WHDR_DONE) == 0)
            {
                allDone = false;
                break;
            }
        }

        if (allDone)
            break;

        Sleep(1);
    }

    g_audioDecodeFinished.store(true);
}

//interface版本
static bool pushInterfaceVideoFrame(const DecodedVideoFrame& frame)
{
    if (!frame.data ||
        frame.width <= 0 ||
        frame.height <= 0 ||
        frame.stride == 0)
    {
        return false;
    }

    if (g_seekTargetSeconds >= 0.0 &&
        (!std::isfinite(frame.ptsSeconds) || frame.ptsSeconds < g_seekTargetSeconds))
    {
        if (g_previewDeadlineMs.load() != 0 && std::isfinite(frame.ptsSeconds) &&
            frame.ptsSeconds >= 0.0 &&
            (g_previewTailFrame.pixels.empty() || frame.ptsSeconds > g_previewTailFrame.ptsSeconds))
        {
            if (!copyVideoFrame(frame, &g_previewTailFrame)) return false;
        }
        return true;
    }

    if (g_videoPtsLogCount < 10)
    {
        std::cerr
            << "Video PTS: "
            << frame.ptsSeconds
            << " seconds"
            << std::endl;

        ++g_videoPtsLogCount;
    }

    return pushVideoFrame(&g_videoFrameQueue, frame, kVideoFrameQueueSize);
}

static bool receiveDecodedVideoFrames()
{
    if (!g_videoDecoder)
        return false;
    while (g_playbackRunning.load())
    {
        DecodedVideoFrame frame = {};
        VideoDecoderResult result =
            g_videoDecoder->receiveFrame(&frame);
        if (result ==
            VideoDecoderResult::FrameReady)
        {
            if (!pushInterfaceVideoFrame(frame))
                return false;
            continue;
        }
        if (result ==
            VideoDecoderResult::TryAgain ||
            result ==
            VideoDecoderResult::EndOfStream ||
            result ==
            VideoDecoderResult::Ok)
        {
            return true;
        }
        std::cerr
            << "Video decoder receiveFrame failed."
            << std::endl;
        return false;
    }
    // 外部要求停止播放，不视为解码错误。
    return true;
}

static bool sendVideoPacketToDecoder(const AVPacket* packet)
{
    if (!g_videoDecoder)
        return false;

    EncodedVideoPacket encodedPacket = {};

    if (packet)
    {
        if (!packet->data || packet->size <= 0)
            return false;

        encodedPacket.data = packet->data;
        encodedPacket.size =
            static_cast<std::size_t>(
                packet->size);

        encodedPacket.pts = packet->pts;
        encodedPacket.dts = packet->dts;
        encodedPacket.duration =
            packet->duration;

        encodedPacket.hasPts =
            packet->pts != AV_NOPTS_VALUE;

        encodedPacket.hasDts =
            packet->dts != AV_NOPTS_VALUE;

        encodedPacket.keyFrame =
            (packet->flags &
                AV_PKT_FLAG_KEY) != 0;
    }

    VideoDecoderResult result =
        VideoDecoderResult::TryAgain;

    // MFT may remain busy across output renegotiation or an HLS
    // segment boundary. Drain pending frames and retry the same packet.
    constexpr int kMaximumSendAttempts = 16;

    for (int attempt = 0;
         attempt < kMaximumSendAttempts;
         ++attempt)
    {
        result =
            packet
            ? g_videoDecoder->sendPacket(
                encodedPacket)
            : g_videoDecoder->drain();

        if (result != VideoDecoderResult::TryAgain)
            break;

        if (!receiveDecodedVideoFrames())
            return false;

        if (!g_playbackRunning.load())
            return true;
    }

    if (result == VideoDecoderResult::Error ||
        result == VideoDecoderResult::TryAgain)
    {
        std::cerr
            << "Video decoder sendPacket failed."
            << std::endl;

        return false;
    }

    return receiveDecodedVideoFrames();
}

static bool decodeVideoPacket(const AVPacket* packet)
{
    if (!g_videoBsf)
        return sendVideoPacketToDecoder(packet);

    int result = 0;

    if (packet)
    {
        AVPacket* input = av_packet_clone(packet);

        if (!input)
            return false;

        result = av_bsf_send_packet(
            g_videoBsf,
            input);

        av_packet_free(&input);
    }
    else
    {
        result = av_bsf_send_packet(
            g_videoBsf,
            nullptr);
    }

    if (result < 0 && result != AVERROR_EOF)
    {
        printFfmpegError(
            "av_bsf_send_packet",
            result);

        return false;
    }

    AVPacket* filteredPacket =
        av_packet_alloc();

    if (!filteredPacket)
        return false;

    while (true)
    {
        result = av_bsf_receive_packet(
            g_videoBsf,
            filteredPacket);

        if (result == AVERROR(EAGAIN) ||
            result == AVERROR_EOF)
        {
            break;
        }

        if (result < 0)
        {
            printFfmpegError(
                "av_bsf_receive_packet",
                result);

            av_packet_free(&filteredPacket);
            return false;
        }

        bool decoded =
            sendVideoPacketToDecoder(
                filteredPacket);

        av_packet_unref(filteredPacket);

        if (!decoded)
        {
            av_packet_free(&filteredPacket);
            return false;
        }
    }

    av_packet_free(&filteredPacket);

    if (!packet)
        return sendVideoPacketToDecoder(nullptr);

    return true;
}

static void stopPlaybackAfterVideoError()
{
    g_playbackEndReason.store(
        PlaybackEndReason::DecodeError);

    g_abortRequested.store(true);
    g_playbackRunning.store(false);

    stopMediaPacketQueue(
        &g_audioPacketQueue);

    stopMediaPacketQueue(
        &g_videoPacketQueue);

    stopVideoFrameQueue(
        &g_videoFrameQueue);
}

static void videoDecodeThreadMain()
{
    while (g_playbackRunning.load())
    {
        if (g_playbackPaused.load())
        {
            Sleep(1);
            continue;
        }

        AVPacket* packet =
            popMediaPacket(
                &g_videoPacketQueue);

        if (!packet)
        {
            // 队列正常结束时，取出解码器中的尾帧。
            if (g_playbackRunning.load() &&
                !decodeVideoPacket(nullptr))
            {
                stopPlaybackAfterVideoError();
            }

            break;
        }

        bool decoded =
            decodeVideoPacket(packet);

        av_packet_free(&packet);

        if (!decoded)
        {
            stopPlaybackAfterVideoError();
            break;
        }
    }

    finishVideoFrameQueue(
        &g_videoFrameQueue);

    g_videoDecodeFinished.store(true);
}


static bool startPlaybackThreads(bool paused, bool outputStarted)
{
    if (ffmpegCancellationRequested())
        return false;
    clearMediaPacketQueue(&g_audioPacketQueue);
    clearMediaPacketQueue(&g_videoPacketQueue);
    clearVideoFrameQueue(&g_videoFrameQueue);

    g_playbackPaused.store(paused);
    g_outputStarted.store(outputStarted);
    g_videoDecodeFinished.store(false);
    g_audioDecodeFinished.store(
        g_audioStreamIndex < 0);
    g_playbackRunning.store(true);

    try
    {
        g_videoDecodeThread =
            std::thread(videoDecodeThreadMain);

        if (g_audioStreamIndex >= 0)
        {
            g_audioDecodeThread =
                std::thread(audioDecodeThreadMain);
        }

        g_demuxThread =
            std::thread(demuxThreadMain);
    }
    catch (...)
    {
        std::cerr
            << "Failed to start playback threads."
            << std::endl;

        g_playbackRunning.store(false);
        stopMediaPacketQueue(&g_audioPacketQueue);
        stopMediaPacketQueue(&g_videoPacketQueue);
        stopVideoFrameQueue(&g_videoFrameQueue);

        if (g_demuxThread.joinable())
            g_demuxThread.join();
        if (g_audioDecodeThread.joinable())
            g_audioDecodeThread.join();
        if (g_videoDecodeThread.joinable())
            g_videoDecodeThread.join();

        return false;
    }

    return true;
}

bool decodeNextFfmpegFrame(DecodedVideoFrame* output)
{
    if (!output)
        return false;

    *output = {};

    QueuedVideoFrame frame;
    if (!tryPopVideoFrame(
        &g_videoFrameQueue,
        &frame))
    {
        return false;
    }

    g_displayFrame = std::move(frame);

    decodedFrameView(g_displayFrame,output);
    return true;
}

bool isFfmpegPlaybackFinished()
{
    return g_videoDecodeFinished.load() &&
        g_audioDecodeFinished.load();
}

bool isFfmpegVideoFinished()
{
    return g_videoDecodeFinished.load();
}

PlaybackEndReason getFfmpegPlaybackEndReason()
{
    return g_playbackEndReason.load();
}

unsigned int getFfmpegFrameIntervalMs()
{
    return g_frameIntervalMs;
}

void closeFfmpegVideo()
{
    g_abortRequested.store(true);
    g_playbackRunning.store(false);
    g_playbackPaused.store(false);
    g_outputStarted.store(false);

    stopMediaPacketQueue(&g_audioPacketQueue);
    stopMediaPacketQueue(&g_videoPacketQueue);
    stopVideoFrameQueue(&g_videoFrameQueue);

    if (g_demuxThread.joinable())
        g_demuxThread.join();
    if (g_audioDecodeThread.joinable())
        g_audioDecodeThread.join();
    if (g_videoDecodeThread.joinable())
        g_videoDecodeThread.join();

    av_bsf_free(&g_videoBsf);

    clearMediaPacketQueue(&g_audioPacketQueue);
    clearMediaPacketQueue(&g_videoPacketQueue);
    clearVideoFrameQueue(&g_videoFrameQueue);
    g_displayFrame = {};
    g_buffering.store(false);
    g_videoDecodeFinished.store(false);
    g_audioDecodeFinished.store(true);

    if (g_audioOutput)
    {
        // 停止所有尚未播放完成的音频。
        waveOutReset(g_audioOutput);
        waveOutSetVolume(g_audioOutput, g_audioDeviceOriginalVolume);

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
    if(g_audioCompletionEvent){CloseHandle(g_audioCompletionEvent);g_audioCompletionEvent=nullptr;}

    g_nextAudioBuffer = 0;

  if (g_audioCodecContext)
      avcodec_free_context(&g_audioCodecContext);

  // unique_ptr 析构具体解码器，具体实现负责释放其内部资源。
  g_videoDecoder.reset();

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
  g_audioStartPtsSeconds.store(-1.0);
  g_seekTargetSeconds = -1.0;
  g_previewTailFrame = {};

  g_playbackRate.store(1.0);
  g_videoClockSeconds = 0.0;
  g_videoClockLastTickMs = 0;
  g_videoClockValid = false;
  g_durationSeconds = 0.0;
  g_effectiveBackend = VideoDecoderBackend::Auto;
  g_audioMutedForRate = false;
  g_audioVolume = 1.0f;
  g_audioVolumeBeforeMute = 0xFFFFFFFF;
  g_audioDeviceOriginalVolume = 0xFFFFFFFF;
  g_playbackEndReason.store(PlaybackEndReason::None);

}

void setFfmpegAudioPaused(bool paused)
{
    // Capture the running interval before freezing; discard the paused interval
    // when resuming, even if the worker never ticked while paused.
    advanceFfmpegPlaybackClock();
    std::lock_guard<std::mutex> outputLock(g_audioOutputMutex);
    g_playbackPaused.store(paused);
    g_videoClockLastTickMs = GetTickCount64();

    if (!g_audioOutput)
        return;
    if (paused || g_buffering.load())
    {
        waveOutPause(g_audioOutput);
    }
    else
    {
        waveOutRestart(g_audioOutput);
    }
}

void startFfmpegPlayback()
{
    g_videoClockLastTickMs = GetTickCount64();
    g_outputStarted.store(true);
}

//建立waveout实际时钟，用来观测
double getFfmpegAudioClockSeconds()
{
    double startPts =
        g_audioStartPtsSeconds.load();

    if(!g_audioOutput || g_playbackRate.load() != 1.0 ||
       g_audioSampleRate <= 0 ||
       startPts < 0.0 ||
       g_audioDecodeFinished.load())
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
        return startPts +
            static_cast<double>(position.u.sample) /
            g_audioSampleRate;
    }
    //部分音频设备可能自己返回已经播放的字节数
    if (position.wType == TIME_BYTES)
    {
        int bytesPerSeconds = g_audioSampleRate * g_audioChannels * 2;
        return startPts +
            static_cast<double>(position.u.cb) /
            bytesPerSeconds;
    }
    return -1.0;
    
}

static double getVideoClockSeconds()
{
    return g_videoClockValid ? g_videoClockSeconds : -1.0;
}

static void resetVideoClock(double seconds)
{
    g_videoClockSeconds = seconds < 0.0 ? 0.0 : seconds;
    g_videoClockLastTickMs = GetTickCount64();
    g_videoClockValid = true;
}

void anchorFfmpegVideoClock(double ptsSeconds)
{
    if (!g_formatContext || !std::isfinite(ptsSeconds) || ptsSeconds < 0.0)
        return;

    resetVideoClock(ptsSeconds);
}

static void applyFfmpegVolume()
{
    const DWORD channel = static_cast<DWORD>(g_audioVolume * 65535.0f + 0.5f);
    g_audioVolumeBeforeMute = channel | (channel << 16);
    if (g_audioOutput)
        waveOutSetVolume(g_audioOutput,
            g_audioMutedForRate ? 0 : g_audioVolumeBeforeMute);
}

void setFfmpegVolume(float normalized)
{
    if (!std::isfinite(normalized))
        return;

    std::lock_guard<std::mutex> outputLock(g_audioOutputMutex);
    g_audioVolume = std::clamp(normalized, 0.0f, 1.0f);
    applyFfmpegVolume();
}

static void muteFfmpegAudio(bool muted)
{
    std::lock_guard<std::mutex> outputLock(g_audioOutputMutex);
    g_audioMutedForRate = muted;
    applyFfmpegVolume();
}

double getFfmpegDurationSeconds()
{
    return g_durationSeconds;
}

VideoDecoderBackend getFfmpegEffectiveBackend()
{
    return g_effectiveBackend;
}

void requestFfmpegAbort()
{
    // No FFmpeg, WaveOut, queue or clock access is allowed on the UI thread.
    g_abortRequested.store(true);
}

double getFfmpegPlaybackClockSeconds()
{
    if (g_playbackRate.load() == 1.0)
    {
        double audioClock = getFfmpegAudioClockSeconds();
        if (audioClock >= 0.0)
            return audioClock;

        return getVideoClockSeconds();
    }

    return getVideoClockSeconds();
}

void advanceFfmpegPlaybackClock()
{
    if (!g_videoClockValid)
        return;

    const ULONGLONG now = GetTickCount64();
    const double elapsed =
        static_cast<double>(now - g_videoClockLastTickMs) / 1000.0;

    g_videoClockLastTickMs = now;
    if (g_playbackPaused.load() || g_buffering.load() || !g_outputStarted.load() ||
        !g_playbackRunning.load())
        return;

    // Keep a wall clock anchored to usable audio, then carry on at 1x when
    // audio is missing, unavailable or drained before the last video frame.
    const double audioClock = g_playbackRate.load() == 1.0
        ? getFfmpegAudioClockSeconds() : -1.0;
    if (audioClock >= 0.0)
        g_videoClockSeconds = audioClock;
    else
        g_videoClockSeconds += elapsed * g_playbackRate.load();
}

bool seekFfmpegToPosition(double targetSeconds)
{
    if (!g_formatContext || !g_playbackRunning.load() ||
        ffmpegCancellationRequested() || !std::isfinite(targetSeconds))
        return false;

    if (targetSeconds < 0.0)
        targetSeconds = 0.0;
    if (g_durationSeconds > 0.0)
        targetSeconds = (std::min)(targetSeconds, g_durationSeconds);
    if (targetSeconds >= static_cast<double>((std::numeric_limits<int64_t>::max)()) /
        AV_TIME_BASE)
        return false;

    const bool wasPaused = g_playbackPaused.load();
    const bool wasStarted = g_outputStarted.load();
    // Interrupt a blocked network read before joining the demux thread.
    g_abortRequested.store(true);
    g_playbackRunning.store(false);

    stopMediaPacketQueue(&g_audioPacketQueue);
    stopMediaPacketQueue(&g_videoPacketQueue);
    stopVideoFrameQueue(&g_videoFrameQueue);

    if (g_demuxThread.joinable())
        g_demuxThread.join();
    if (g_audioDecodeThread.joinable())
        g_audioDecodeThread.join();
    if (g_videoDecodeThread.joinable())
        g_videoDecodeThread.join();

    g_previewTailFrame = {};
    if (g_audioOutput)
    {
        waveOutReset(g_audioOutput);

        for (int i = 0; i < kAudioBufferCount; ++i)
        {
            if ((g_audioHeaders[i].dwFlags & WHDR_PREPARED) != 0)
            {
                waveOutUnprepareHeader(
                    g_audioOutput,
                    &g_audioHeaders[i],
                    sizeof(g_audioHeaders[i]));
            }

            g_audioHeaders[i] = {};
            g_audioBuffers[i].clear();
        }
    }

    g_nextAudioBuffer = 0;
    g_audioStartPtsSeconds.store(-1.0);

    if (g_audioCodecContext)
        avcodec_flush_buffers(g_audioCodecContext);
    if (g_audioResampleContext)
    {
        swr_close(g_audioResampleContext);
        if (swr_init(g_audioResampleContext) < 0)
            return false;
    }
    if (g_videoDecoder)
        g_videoDecoder->flush();
    if (g_videoBsf)
        av_bsf_flush(g_videoBsf);

    clearMediaPacketQueue(&g_audioPacketQueue);
    clearMediaPacketQueue(&g_videoPacketQueue);
    clearVideoFrameQueue(&g_videoFrameQueue);

    g_abortRequested.store(false);
    if (ffmpegCancellationRequested())
        return false;
    // A previously interrupted AVIO read must not poison the new seek.
    if (g_formatContext->pb)
    {
        g_formatContext->pb->error = 0;
        g_formatContext->pb->eof_reached = 0;
    }
    const int64_t targetTimestamp =
        static_cast<int64_t>(targetSeconds * AV_TIME_BASE);
    const int seekResult = av_seek_frame(
        g_formatContext,
        -1,
        targetTimestamp,
        AVSEEK_FLAG_BACKWARD);

    if (seekResult < 0 || ffmpegCancellationRequested())
    {
        printFfmpegError("av_seek_frame", seekResult);
        // The worker can report this failure and subsequently close or reopen.
        // Do not advertise a target clock which the demuxer never reached.
        g_playbackEndReason.store(PlaybackEndReason::DemuxError);
        g_videoDecodeFinished.store(true);
        g_audioDecodeFinished.store(true);
        g_outputStarted.store(false);
        g_displayFrame = {};
        return false;
    }

    g_videoDecodeFinished.store(false);
    g_audioDecodeFinished.store(g_audioStreamIndex < 0);
    g_playbackEndReason.store(PlaybackEndReason::None);
    g_displayFrame = {};
    g_outputStarted.store(false);
    g_seekTargetSeconds = targetSeconds;

    resetVideoClock(targetSeconds);

    if (ffmpegCancellationRequested() || !startPlaybackThreads(wasPaused, wasStarted))
        return false;

    setFfmpegAudioPaused(wasPaused);
    return true;
}

bool previewFfmpegAtPosition(double seconds, DecodedVideoFrame* frame)
{
    if (!frame || !std::isfinite(seconds) || ffmpegCancellationRequested())
        return false;
    *frame = {};
    const bool wasStarted = g_outputStarted.exchange(false);
    setFfmpegAudioPaused(true);
    const ULONGLONG deadline = GetTickCount64() + 5000;
    g_previewDeadlineMs.store(deadline);
    bool ready = seekFfmpegToPosition(seconds);
    if (ready)
    {
        // Pause the actual device throughout preview, but let demux/video
        // process keyframe preroll. Audio packets are retained for resume.
        g_playbackPaused.store(false);
        ready = false;
        while (GetTickCount64() < deadline && !ffmpegCancellationRequested())
        {
            if (decodeNextFfmpegFrame(frame))
            {
                ready = true;
                break;
            }
            if (isFfmpegVideoFinished())
            {
                // A timestamp denotes a frame's start. At the end of video,
                // the final frame still covers the interval up to EOF even
                // though no frame starts at/after the requested tail position.
                if (getFfmpegPlaybackEndReason() == PlaybackEndReason::Completed &&
                    !g_previewTailFrame.pixels.empty())
                {
                    g_displayFrame = std::move(g_previewTailFrame);
                    decodedFrameView(g_displayFrame,frame);
                    ready = true;
                }
                break;
            }
            Sleep(1);
        }
    }
    setFfmpegAudioPaused(true);
    g_outputStarted.store(wasStarted);
    ready = ready && !ffmpegCancellationRequested();
    g_previewDeadlineMs.store(0);
    if (!ready)
        *frame = {};
    return ready;
}

void setFfmpegPlaybackRate(double rate)
{
    if (!std::isfinite(rate) || rate <= 0.0)
        rate = 1.0;

    const double oldRate = g_playbackRate.load();
    if (rate == oldRate)
        return;

    advanceFfmpegPlaybackClock();

    if (oldRate == 1.0)
    {
        double anchor = getFfmpegPlaybackClockSeconds();
        if (anchor < 0.0)
            anchor = g_displayFrame.ptsSeconds >= 0.0
                ? g_displayFrame.ptsSeconds
                : 0.0;

        resetVideoClock(anchor);
        muteFfmpegAudio(true);
    }

    bool restartOutput = false;
    if (rate == 1.0)
    {
        // Gate the old thread before changing rate/unmuting, and keep the new
        // thread gated until the seek has reset WaveOut and joined the old one.
        std::lock_guard<std::mutex> outputLock(g_audioOutputMutex);
        restartOutput = g_outputStarted.exchange(false);
    }
    g_playbackRate.store(rate);
    g_audioPacketQueue.condition.notify_all();

    if (rate == 1.0)
    {
        // The restarted audio thread may run before seek returns. Restore
        // volume first so its initial target PCM cannot be discarded as muted.
        muteFfmpegAudio(false);
        if (seekFfmpegToPosition(getVideoClockSeconds()))
            g_outputStarted.store(restartOutput);
    }
}

double getFfmpegPlaybackRate()
{
    return g_playbackRate.load();
}

bool seekFfmpegBySeconds(double deltaSeconds)
{
    double position = getFfmpegPlaybackClockSeconds();
    if (position < 0.0)
        position = 0.0;

    return seekFfmpegToPosition(position + deltaSeconds);
}

void setFfmpegNativeYuvOutput(bool enabled){g_nativeYuvOutput=enabled;}
std::uint64_t getFfmpegAudioCompletionCount(){return g_audioCompletions.load();}
void setFfmpegBuffering(bool buffering){
    g_buffering.store(buffering);g_videoClockLastTickMs=GetTickCount64();
    std::lock_guard<std::mutex> lock(g_audioOutputMutex);
    if(g_audioOutput){if(buffering)waveOutPause(g_audioOutput);else if(!g_playbackPaused.load())waveOutRestart(g_audioOutput);}
}
