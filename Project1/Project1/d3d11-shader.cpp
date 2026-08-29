#include "d3d11-shader.h"

#include <d3dcompiler.h>
#include <windows.h>

static ID3D11VertexShader* g_vertexShader = nullptr;
static ID3D11PixelShader* g_pixelShader = nullptr;

static ID3D11Buffer* g_brightnessBuffer = nullptr;

struct BrightnessData
{
    float brightness;
    float padding[3];
};

// 使用全屏三角形，避免创建顶点缓冲区和 InputLayout。
static const char g_shaderSource[] = R"(

//视频纹理绑定到Pixel Shader的t0
Texture2D videoTexture : register(t0);
//纹理采样器绑定到Pixel Shader的s0
SamplerState videoSampler : register(s0);

cbuffer BrightnessBuffer:register(b0)
{
 float brightness;
 float3 brightnessPadding;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VertexOutput vertexMain(uint vertexId : SV_VertexID)
{
    float2 positions[3] =
    {
        float2(-1.0,  1.0),
        float2( 3.0,  1.0),
        float2(-1.0, -3.0)
    };

    float2 uvs[3] =
    {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0)
    };

    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}
float4 pixelMain(VertexOutput input):SV_TARGET
{
  //从ffmpeg解码并上传BGRA纹理中取样
 float4 color=videoTexture.Sample(videoSampler,input.uv);
 return float4(
    saturate(color.rgb*brightness),
   1.0);
}
)";

static bool compileShader(
  const char* entryPoint,
  const char* target,
  ID3DBlob** shaderBlob)
{
  ID3DBlob* errorBlob = nullptr;

  HRESULT result = D3DCompile(
    g_shaderSource,
    sizeof(g_shaderSource) - 1,
    nullptr,
    nullptr,
    nullptr,
    entryPoint,
    target,
    D3DCOMPILE_ENABLE_STRICTNESS,
    0,
    shaderBlob,
    &errorBlob);

  // 编译错误输出到 VS 调试输出窗口。
  if (errorBlob)
  {
    OutputDebugStringA(
      static_cast<const char*>(errorBlob->GetBufferPointer()));

    errorBlob->Release();
  }

  return SUCCEEDED(result);
}

bool initializeD3D11Shaders(ID3D11Device* device)
{
  if (!device)
    return false;

  ID3DBlob* vertexBlob = nullptr;

  if (!compileShader("vertexMain", "vs_5_0", &vertexBlob))
    return false;

  HRESULT result = device->CreateVertexShader(
    vertexBlob->GetBufferPointer(),
    vertexBlob->GetBufferSize(),
    nullptr,
    &g_vertexShader);

  vertexBlob->Release();

  if (FAILED(result))
    return false;

  ID3DBlob* pixelBlob = nullptr;

  if (!compileShader("pixelMain", "ps_5_0", &pixelBlob))
  {
    shutdownD3D11Shaders();
    return false;
  }

  result = device->CreatePixelShader(
    pixelBlob->GetBufferPointer(),
    pixelBlob->GetBufferSize(),
    nullptr,
    &g_pixelShader);

  pixelBlob->Release();

  if (FAILED(result))
  {
    shutdownD3D11Shaders();
    return false;
  }
  D3D11_BUFFER_DESC bufferDesc = {};
  bufferDesc.ByteWidth = sizeof(BrightnessData);
  bufferDesc.Usage = D3D11_USAGE_DEFAULT;
  bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

  BrightnessData initialData = {};
  initialData.brightness = 1.0f;

  D3D11_SUBRESOURCE_DATA resourceData = {};
  resourceData.pSysMem = &initialData;

  result = device->CreateBuffer(
      &bufferDesc,
      &resourceData,
      &g_brightnessBuffer);

  if (FAILED(result))
  {
      shutdownD3D11Shaders();
      return false;
  }
  return true;
}



bool bindD3D11Shaders(ID3D11DeviceContext* context)
{
    if (!context ||
        !g_vertexShader ||
        !g_pixelShader ||
        !g_brightnessBuffer)
    {
        return false;
    }

    context->VSSetShader(
        g_vertexShader,
        nullptr,
        0);

    context->PSSetShader(
        g_pixelShader,
        nullptr,
        0);

    context->PSSetConstantBuffers(
        0,
        1,
        &g_brightnessBuffer);

    return true;
}

void shutdownD3D11Shaders()
{
    if (g_brightnessBuffer)
    {
        g_brightnessBuffer->Release();
        g_brightnessBuffer = nullptr;
    }

    if (g_pixelShader)
    {
        g_pixelShader->Release();
        g_pixelShader = nullptr;
    }

    if (g_vertexShader)
    {
        g_vertexShader->Release();
        g_vertexShader = nullptr;
    }
}

void setD3D11ShaderBrightness(
    ID3D11DeviceContext* context,
    float brightness)
{
    if (!context || !g_brightnessBuffer)
    {
        return;
    }

    BrightnessData data = {};
    data.brightness = brightness;

    context->UpdateSubresource(
        g_brightnessBuffer,
        0,
        nullptr,
        &data,
        0,
        0);
}