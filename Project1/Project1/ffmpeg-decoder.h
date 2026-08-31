#pragma once

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
bool isFfmpegVideoFinished();
bool isFfmpegPlaybackFinished();
void closeFfmpegVideo();

unsigned int getFfmpegFrameIntervalMs();
void startFfmpegPlayback();
void setFfmpegAudioPaused(bool paused);
double getFfmpegAudioClockSeconds();
