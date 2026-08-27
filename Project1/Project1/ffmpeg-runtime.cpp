#include "ffmpeg-runtime.h"
#include <iostream>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}
bool initializeFfmpeg()
{
  //初始化HTTP/HTTPS等网络协议支持
  int result = avformat_network_init();
  if (result < 0)
  {
    char errorText[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(result, errorText, sizeof(errorText));

    std::cerr
      << "Ffmpeg initialization failed: "
      << errorText
      << std::endl;

    return false;
  }
  //输出版本用于确认头文件、lib和dll来自同一套SDK
std::cerr
<< "Ffmpeg version: "
<< av_version_info()
<< std::endl;

return true;
}

void shutdownFfmpeg()
{
  //与initializeffmpeg对应，释放网络模块资源
  avformat_network_deinit();
}


