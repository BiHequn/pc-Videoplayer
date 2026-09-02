#pragma once
#include <windows.h>

bool initializeD3D11(HWND hwnd);
void renderD3D11();
void shutdownD3D11();

void resizeD3D11(UINT width, UINT height);

void setD3D11Brightness(float brightness);

bool updateD3D11VideoFrame(
  const unsigned char* data,
  int width,
  int height,
  int stride);