#pragma once

bool createPipWindow();
void runPipWindowLoop();
void requestPipWindowClose();

bool showPipVideoFrame(
  const unsigned char* data,
  int width,
  int height,
  int stride);