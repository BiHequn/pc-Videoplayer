#include "app-runtime.h"
#include "media/ffmpeg-decoder.h"
#include "media/ffmpeg-runtime.h"
#include "platform/parent-watch.h"
#include "platform/pip-window.h"
#include "platform/player-channel.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#include<string>

#include"media/video-decoder-factory.h"
#include<utility>

#include"source-selection-dialog.h"
//参数解析
static DWORD parseParentPid(int argc, char* argv[])
{
  int i;
  for (i = 1; i < argc - 1; i++)
  {
    if (strcmp(argv[i], "--parent-pid") == 0)
      return (DWORD)atoi(argv[i + 1]);
  }
  return 0;
}

static const char* parseSource(int argc, char* argv[])
{
  for (int i = 1; i < argc - 1; i++)
  {
    if (strcmp(argv[i], "--source") == 0)
      return argv[i + 1];
  }
  return nullptr;
}

//参数解析函数
static bool parseVideoDecoderBackend(int argc, char* argv[], VideoDecoderBackend* output)
{
    if (!output)
        return false;

    //默认保持ffmpeg
    *output = VideoDecoderBackend::Auto;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--decoder") != 0)
            continue;
        if (i + 1 >= argc)
            return false;
        const char* value = argv[i + 1];
        if (strcmp(value, "ffmpeg") == 0)
        {
            *output =
                VideoDecoderBackend::Ffmpeg;
            return true;
        }
        if (strcmp(value, "mf") == 0)
        {
            *output =
                VideoDecoderBackend::MediaFoundation;
            return true;
        }
        if (strcmp(value, "auto") == 0)
        {
            *output =
                VideoDecoderBackend::Auto;
            return true;
        }
        return false;
    }
    return true;

}

//文件选择函数

int runNativePlayer(int argc, char* argv[])
{
    VideoDecoderBackend decoderBackend;
    if (!parseVideoDecoderBackend(argc, argv, &decoderBackend))
    {
        fprintf(
            stderr,
            "Invalid --decoder value. "
            "Expected ffmpeg, mf or auto.\n");
        return 4;
    }
    std::unique_ptr<IVideoDecoder> videoDecoder = createVideoDecoder(decoderBackend);
    if (!videoDecoder)
    {
        fprintf(
            stderr,
            "Requested video decoder "
            "is not available in this build.\n");
        return 4;
    }

    std::string selectedSource;
    const char* source = parseSource(argc, argv);

    if (!source || source[0] == '\0')
    {
        if (!showSourceSelectionDialog(
            &selectedSource))
        {
            return 0;
        }

        source = selectedSource.c_str();
    }

  startParentWatch(parseParentPid(argc, argv));

  if (!initializeFfmpeg())
    return 1;
  if (!openFfmpegVideo(source,std::move(videoDecoder),decoderBackend))
  {
    shutdownFfmpeg();
    return 2;
  }

  DecodedVideoFrame firstFrame = {};
  while (!decodeNextFfmpegFrame(&firstFrame))
  {
    if (isFfmpegVideoFinished())
    {
      closeFfmpegVideo();
      shutdownFfmpeg();
      return 3;
    }

    Sleep(1);
  }
  if (!createPipWindow())
  {
  //窗口失败，释放已经打开的视频解码器
    closeFfmpegVideo();
    shutdownFfmpeg();
    return 2;
  }
  if (!showPipVideoFrame(
    firstFrame.data,
    firstFrame.width,
    firstFrame.height,
    firstFrame.stride))
  {
    closeFfmpegVideo();
    shutdownFfmpeg();
    return 4;
  }

  startFfmpegPlayback();

  if (!startPipPlaybackTimer())
  {
      closeFfmpegVideo();
      shutdownFfmpeg();
      return 5;
  }
  startCloseWatcher();
  sendReady();
  runPipWindowLoop();

  //窗口退出后，按顺序释放视频和ffmpeg网络模块
  closeFfmpegVideo();
  shutdownFfmpeg();

  return 0;
}