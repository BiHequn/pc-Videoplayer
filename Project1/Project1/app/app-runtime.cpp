#include "app-runtime.h"
#include "media/ffmpeg-decoder.h"
#include "media/ffmpeg-runtime.h"
#include "platform/parent-watch.h"
#include "platform/pip-window.h"
#include "platform/player-channel.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#include<commdlg.h>
#include<string>

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

//文件选择函数
static std::string wideToUtf8(const wchar_t* text)
{
    int size = WideCharToMultiByte(
        CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);

    if (size <= 1)
        return {};

    std::string result(size, '\0');

    WideCharToMultiByte(
        CP_UTF8, 0, text, -1,
        result.data(), size, nullptr, nullptr);

    result.pop_back();
    return result;
}

static std::string selectVideoFile()
{
    wchar_t filePath[32768] = {};

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = filePath;
    dialog.nMaxFile = static_cast<DWORD>(std::size(filePath));
    dialog.lpstrFilter =
        L"视频文件\0*.mp4;*.flv;*.m3u8;*.mkv;*.avi;*.mov;*.webm\0"
        L"所有文件\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags =
        OFN_EXPLORER |
        OFN_FILEMUSTEXIST |
        OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&dialog))
        return {};

    return wideToUtf8(filePath);
}

int runNativePlayer(int argc, char* argv[])
{
    std::string selectedSource;
    const char* source = parseSource(argc, argv);

    if (!source || source[0] == '\0')
    {
        selectedSource = selectVideoFile();

        // 用户取消选择，正常退出
        if (selectedSource.empty())
            return 0;

        source = selectedSource.c_str();
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