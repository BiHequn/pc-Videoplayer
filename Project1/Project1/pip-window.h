#pragma once

bool createPipWindow();
bool startPipPlaybackTimer();
void runPipWindowLoop();
void requestPipWindowClose();

void requestPipWindowOpacity(int opacityPercent);

bool showPipVideoFrame(
  const unsigned char* data,
  int width,
  int height,
  int stride);

void requestPipWindowBrightness(int brightnessPercent);
void requestPipWindowPaused(bool paused);