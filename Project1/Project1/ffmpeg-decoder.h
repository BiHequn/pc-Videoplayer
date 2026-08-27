#pragma once

struct DecodedVideoFrame
{
  const unsigned char* data;
  int width;
  int height;
  int stride;
};

bool openFfmpegVideo(const char* source);
bool decodeFirstFfmpegFrame(DecodedVideoFrame* output);
void closeFfmpegVideo();
