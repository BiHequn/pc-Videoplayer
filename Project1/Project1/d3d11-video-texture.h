#pragma once
#include<d3d11.h>

bool uploadD3D11VideoTexture(
  ID3D11Device* device,
  ID3D11DeviceContext* context,
  const unsigned char* data,
  int width,
  int height,
  int stride);
bool bindD3D11VideoTexture(ID3D11DeviceContext* context);
void shutdownD3D11VideoTexture();