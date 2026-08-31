#include "app-runtime.h"
#include "parent-watch.h"
#include "player-channel.h"
#include "pip-window.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include"ffmpeg-runtime.h"
#include"ffmpeg-decoder.h"

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

int runNativePlayer(int argc, char* argv[])
{
  const char* source = parseSource(argc, argv);
  if (!source || source[0] == '\0')
  {
    fprintf(stderr,
      "Usage: native-player.exe --source <file-or-url> "
      "[--parent-pid <pid>]\n");
    return 1;
  }

  startParentWatch(parseParentPid(argc, argv));

  if (!initializeFfmpeg())
    return 1;
  if (!openFfmpegVideo(source))
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