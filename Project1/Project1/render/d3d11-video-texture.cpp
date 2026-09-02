#include "d3d11-video-texture.h"

#include <cstring>

static ID3D11Texture2D* g_videoTexture = nullptr;
static ID3D11ShaderResourceView* g_textureView = nullptr;
static ID3D11SamplerState* g_sampler = nullptr;

static int g_textureWidth = 0;
static int g_textureHeight = 0;

static void releaseTexture()
{
  if (g_textureView)
  {
    g_textureView->Release();
    g_textureView = nullptr;
  }

  if (g_videoTexture)
  {
    g_videoTexture->Release();
    g_videoTexture = nullptr;
  }

  g_textureWidth = 0;
  g_textureHeight = 0;
}

static bool createTexture(
  ID3D11Device* device,
  int width,
  int height)
{
  releaseTexture();

  D3D11_TEXTURE2D_DESC textureDesc = {};
  textureDesc.Width = width;
  textureDesc.Height = height;
  textureDesc.MipLevels = 1;
  textureDesc.ArraySize = 1;
  textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  textureDesc.SampleDesc.Count = 1;
  textureDesc.Usage = D3D11_USAGE_DYNAMIC;
  textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  HRESULT result = device->CreateTexture2D(
    &textureDesc,
    nullptr,
    &g_videoTexture);

  if (FAILED(result))
    return false;

  // ShaderResourceView 允许 Pixel Shader 读取视频纹理。
  result = device->CreateShaderResourceView(
    g_videoTexture,
    nullptr,
    &g_textureView);

  if (FAILED(result))
  {
    releaseTexture();
    return false;
  }

  if (!g_sampler)
  {
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    result = device->CreateSamplerState(
      &samplerDesc,
      &g_sampler);

    if (FAILED(result))
    {
      releaseTexture();
      return false;
    }
  }

  g_textureWidth = width;
  g_textureHeight = height;
  return true;
}

bool uploadD3D11VideoTexture(
  ID3D11Device* device,
  ID3D11DeviceContext* context,
  const unsigned char* data,
  int width,
  int height,
  int stride)
{
  if (!device || !context || !data ||
    width <= 0 || height <= 0 ||
    stride < width * 4)
  {
    return false;
  }

  // 视频尺寸改变时重新创建纹理。
  if (!g_videoTexture ||
    width != g_textureWidth ||
    height != g_textureHeight)
  {
    if (!createTexture(device, width, height))
      return false;
  }

  // 上传前解除 Pixel Shader 对纹理的绑定。
  ID3D11ShaderResourceView* emptyView = nullptr;
  context->PSSetShaderResources(0, 1, &emptyView);

  D3D11_MAPPED_SUBRESOURCE mapped = {};

  HRESULT result = context->Map(
    g_videoTexture,
    0,
    D3D11_MAP_WRITE_DISCARD,
    0,
    &mapped);

  if (FAILED(result))
    return false;

  const size_t rowBytes =
    static_cast<size_t>(width) * 4;

  // RowPitch 可能大于 width * 4，因此必须逐行复制。
  for (int y = 0; y < height; y++)
  {
    unsigned char* destination =
      static_cast<unsigned char*>(mapped.pData) +
      static_cast<size_t>(y) * mapped.RowPitch;

    const unsigned char* source =
      data + static_cast<size_t>(y) * stride;

    std::memcpy(destination, source, rowBytes);
  }

  context->Unmap(g_videoTexture, 0);
  return true;
}

bool bindD3D11VideoTexture(ID3D11DeviceContext* context)
{
  if (!context || !g_textureView || !g_sampler)
    return false;

  context->PSSetShaderResources(0, 1, &g_textureView);
  context->PSSetSamplers(0, 1, &g_sampler);
  return true;
}

void shutdownD3D11VideoTexture()
{
  releaseTexture();

  if (g_sampler)
  {
    g_sampler->Release();
    g_sampler = nullptr;
  }
}