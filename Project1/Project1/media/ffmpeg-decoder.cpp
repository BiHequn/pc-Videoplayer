#include"ffmpeg-decoder.h"
#include<Windows.h>
#include<mmsystem.h>
#include"media-packet-queue.h"
#include"video-frame-queue.h"

#include <atomic>
#include <cstring>
#include <iostream>
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

static const int kAudioBufferCount = 4;
static std::vector<unsigned char>
  g_audioBuffers[kAudioBufferCount];
static WAVEHDR
g_audioHeaders[kAudioBufferCount] = {};
static int g_nextAudioBuffer = 0;

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
static std::atomic<PlaybackEndReason>
    g_playbackEndReason(PlaybackEndReason::None);
static QueuedVideoFrame g_displayFrame;

static const size_t kAudioPacketQueueSize = 32;
static const size_t kVideoPacketQueueSize = 16;
static const size_t kVideoFrameQueueSize = 6;

static void audioDecodeThreadMain();
static void videoDecodeThreadMain();
static bool startPlaybackThreads();

static std::unique_ptr<IVideoDecoder> g_videoDecoder;
static AVBSFContext* g_videoBsf = nullptr;


static void printFfmpegError(const char* operation, int errorCode)
{
  char errorText[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(errorCode, errorText, sizeof(errorText));

  std::cerr << operation<< " failed: "<< errorText<< std::endl;
}

static int ffmpegInterruptCallback(void*)
{
    return g_abortRequested.load() ? 1 : 0;
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
    while (g_playbackRunning.load())
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
            else if (!g_abortRequested.load())
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
            // 首帧显示前音频线程还没启动输出，暂时不限制队列长度。
            size_t maxPacketCount =
                g_outputStarted.load() ?
                kAudioPacketQueueSize : 0;

            pushed = pushMediaPacket(
                &g_audioPacketQueue,
                packet,
                maxPacketCount);
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
  g_videoDecoder = std::move(videoDecoder);
  g_abortRequested.store(false);
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

  AVCodecParameters* codecParameters =
      videoStream->codecpar;

  VideoDecoderConfig decoderConfig;
  decoderConfig.codec =
      toVideoCodec(codecParameters->codec_id);
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
  }

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
      << "Opened video stream: "
      << avcodec_get_name(
          codecParameters->codec_id)
      << ", "
      << codecParameters->width
      << "x"
      << codecParameters->height
      << std::endl;

  if (!startPlaybackThreads())
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

            if (g_audioStartPtsSeconds.load() < 0.0)
            {
                g_audioStartPtsSeconds.store(audioPtsSeconds);
            }
        }

        if (g_audioPtsLogCount < 10)
        {
            std::cerr
                << "Audio PTS: "
                << audioPtsSeconds
                << " seconds"
                << std::endl;

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

            // 缓冲区仍在播放时等待，防止覆盖正在播放的数据。
            if ((audioHeader.dwFlags & WHDR_PREPARED) != 0 &&
                (audioHeader.dwFlags & WHDR_DONE) == 0)
            {
                while ((audioHeader.dwFlags & WHDR_DONE) == 0)
                {
                    if (!g_playbackRunning.load())
                    {
                        av_frame_unref(frame);
                        return;
                    }
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
    while (g_playbackRunning.load())
    {
        if (!g_outputStarted.load())
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
        decodeAudioPacket(packet, frame);
        //队列返回的是独立clone的packet,消费完成后由音频线程负责释放
        av_packet_free(&packet);
    }
    av_frame_free(&frame);

    // 文件结束时让已经交给 WaveOut 的尾部音频播完。
    while (g_playbackRunning.load() && g_audioOutput)
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
        frame.stride <= 0)
    {
        return false;
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

    return pushVideoFrame(
        &g_videoFrameQueue,
        frame.data,
        frame.width,
        frame.height,
        frame.stride,
        frame.ptsSeconds,
        kVideoFrameQueueSize);
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


static bool startPlaybackThreads()
{
    clearMediaPacketQueue(&g_audioPacketQueue);
    clearMediaPacketQueue(&g_videoPacketQueue);
    clearVideoFrameQueue(&g_videoFrameQueue);

    g_playbackPaused.store(false);
    g_outputStarted.store(false);
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

    output->data = g_displayFrame.pixels.data();
    output->width = g_displayFrame.width;
    output->height = g_displayFrame.height;
    output->stride = g_displayFrame.stride;
    output->ptsSeconds = g_displayFrame.ptsSeconds;
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
    g_videoDecodeFinished.store(false);
    g_audioDecodeFinished.store(true);

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

}

void setFfmpegAudioPaused(bool paused)
{
    g_playbackPaused.store(paused);

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

void startFfmpegPlayback()
{
    g_outputStarted.store(true);
}

//建立waveout实际时钟，用来观测
double getFfmpegAudioClockSeconds()
{
    double startPts =
        g_audioStartPtsSeconds.load();

    if(!g_audioOutput ||
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