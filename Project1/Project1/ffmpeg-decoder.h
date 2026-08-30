#pragma once
#include<cstddef>

struct DecodedVideoFrame
{
  const unsigned char* data;
  int width;
  int height;
  int stride;
  double ptsSeconds;
};

bool openFfmpegVideo(const char* source);
bool decodeNextFfmpegFrame(DecodedVideoFrame* output);
void closeFfmpegVideo();
unsigned int getFfmpegFrameIntervalMs();

//音频解码结果统一转成S16交错PCM
int getFfmpegAudioSampleRate();
int getFfmpegAudioChannels();
size_t getFfmpegAudioPcmBytes();

void setFfmpegAudioPaused(bool paused);

double getFfmpegAudioClockSeconds();
