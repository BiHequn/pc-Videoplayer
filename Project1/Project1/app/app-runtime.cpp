#include "app-runtime.h"
#include "media/ffmpeg-decoder.h"
#include "media/ffmpeg-runtime.h"
#include "platform/parent-watch.h"
#include "platform/pip-window.h"
#include "platform/player-channel.h"
#include "service/api-client.h"
#include "service/media-client.h"
#include "service/server-settings.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#include<atomic>
#include<string>
#include<thread>
#include<vector>

#include"media/video-decoder-factory.h"
#include<utility>

#include"source-selection-dialog.h"

static constexpr ULONGLONG kFirstFrameTimeoutMs = 30000;

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

static const char* parseArgument(int argc, char* argv[], const char* name)
{
  for (int i = 1; i < argc - 1; ++i)
  {
    if (strcmp(argv[i], name) == 0)
      return argv[i + 1];
  }
  return nullptr;
}

static int runServerSmokeTest(int argc, char* argv[])
{
  const char* host = parseArgument(argc, argv, "--host");
  const char* username = parseArgument(argc, argv, "--username");
  const char* password = nullptr;
  char* environmentPassword = nullptr;
  std::size_t environmentPasswordLength = 0;
  if (_dupenv_s(&environmentPassword, &environmentPasswordLength,
                "VIDEOPLAYER_TEST_PASSWORD") == 0)
    password = environmentPassword;
  if (!host || !username || !password)
  {
    fprintf(stderr,
            "--server-smoke requires --host, --username and "
            "VIDEOPLAYER_TEST_PASSWORD.\n");
    free(environmentPassword);
    return 10;
  }

  ServerSettings settings;
  settings.host.assign(host, host + strlen(host));
  fprintf(stderr, "SMOKE_STAGE api_login\n");
  LoginResult login = loginToApi(settings, username, password);
  if (!login.success)
  {
    fwprintf(stderr, L"API login failed: %ls\n", login.error.c_str());
    free(environmentPassword);
    return 11;
  }

  MediaClient media;
  fprintf(stderr, "SMOKE_STAGE media_auth\n");
  MediaResult connected = media.connectAndAuthenticate(settings, login.accessToken);
  if (!connected.success)
  {
    fwprintf(stderr, L"Media authentication failed: %ls\n", connected.error.c_str());
    free(environmentPassword);
    return 12;
  }

  std::vector<RemoteVideoInfo> videos;
  fprintf(stderr, "SMOKE_STAGE video_list\n");
  MediaResult listed = media.listVideos(&videos);
  if (!listed.success)
  {
    fwprintf(stderr, L"Video list failed: %ls\n", listed.error.c_str());
    free(environmentPassword);
    return 13;
  }
  if (videos.empty())
  {
    printf("SERVER_SMOKE_OK videos=0\n");
    free(environmentPassword);
    return 0;
  }

  std::atomic_bool cancelled{false};
  std::string path;
  fprintf(stderr, "SMOKE_STAGE video_download id=%d\n", videos.front().id);
  MediaResult downloaded = media.downloadVideo(videos.front().id, {}, cancelled, &path);
  if (!downloaded.success)
  {
    fwprintf(stderr, L"Video download failed: %ls\n", downloaded.error.c_str());
    free(environmentPassword);
    return 14;
  }
  printf("SERVER_SMOKE_OK videos=%zu video_id=%d path=%s\n",
         videos.size(), videos.front().id, path.c_str());
  free(environmentPassword);
  return 0;
}

//参数解析函数
static bool parseVideoDecoderBackend(int argc, char* argv[], VideoDecoderBackend* output)
{
    if (!output)
        return false;

    // 默认优先使用 MF，失败时回退 FFmpeg。
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
    for (int i = 1; i < argc; ++i)
    {
      if (strcmp(argv[i], "--server-smoke") == 0)
        return runServerSmokeTest(argc, argv);
    }

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

    PlaybackSelection selection;
    const char* source = parseSource(argc, argv);

    if (!source || source[0] == '\0')
    {
        if (!showSourceSelectionDialog(&selection))
        {
            return 0;
        }

        source = selection.source.c_str();
    }

  startParentWatch(parseParentPid(argc, argv));

  if (!initializeFfmpeg())
    return 1;

  bool fallbackAttempted = false;

  auto startSoftwareFallback = [&]() -> bool
  {
    fallbackAttempted = true;
    closeFfmpegVideo();

    std::unique_ptr<IVideoDecoder> fallbackDecoder =
        createVideoDecoder(VideoDecoderBackend::Ffmpeg);

    if (!fallbackDecoder)
      return false;

    fprintf(
        stderr,
        "Retrying playback with FFmpeg software decoder.\n");

    return openFfmpegVideo(
        source,
        std::move(fallbackDecoder),
        VideoDecoderBackend::Ffmpeg);
  };

  bool opened = openFfmpegVideo(
      source,
      std::move(videoDecoder),
      decoderBackend);

  // Auto 模式下，BSF 或硬解初始化失败时再尝试一次软解。
  if (!opened &&
      decoderBackend == VideoDecoderBackend::Auto)
  {
    opened = startSoftwareFallback();
  }

  if (!opened)
  {
    closeFfmpegVideo();
    shutdownFfmpeg();
    return 2;
  }

  DecodedVideoFrame firstFrame = {};
  ULONGLONG firstFrameStart = GetTickCount64();

  while (!decodeNextFfmpegFrame(&firstFrame))
  {
    const bool firstFrameTimedOut =
        GetTickCount64() - firstFrameStart >=
        kFirstFrameTimeoutMs;

    if (isFfmpegVideoFinished() ||
        firstFrameTimedOut)
    {
      if (firstFrameTimedOut)
      {
        fprintf(
            stderr,
            "Timed out waiting for the first video frame.\n");
      }

      // Auto 模式只回退一次，避免无法播放的文件无限重试。
      if (decoderBackend == VideoDecoderBackend::Auto &&
          !fallbackAttempted &&
          startSoftwareFallback())
      {
        firstFrameStart = GetTickCount64();
        continue;
      }

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

  std::thread playReportWorker;
  if (selection.videoId > 0 && !selection.accessToken.empty())
  {
      ServerSettings settings;
      settings.host = selection.serverHost;
      settings.mediaPort = selection.mediaPort;
      std::string token = selection.accessToken;
      int videoId = selection.videoId;
      playReportWorker = std::thread([settings, token, videoId] {
          MediaClient reporter;
          MediaResult connection = reporter.connectAndAuthenticate(settings, token);
          if (connection.success)
              reporter.reportPlay(videoId);
      });
  }

  if (!startPipPlaybackTimer())
  {
      if (playReportWorker.joinable())
          playReportWorker.join();
      closeFfmpegVideo();
      shutdownFfmpeg();
      return 5;
  }
  startCloseWatcher();
  sendReady();
  runPipWindowLoop();
  if (playReportWorker.joinable())
      playReportWorker.join();

  //窗口退出后，按顺序释放视频和ffmpeg网络模块
  closeFfmpegVideo();
  shutdownFfmpeg();

  return 0;
}
