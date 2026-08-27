#include "d3d11-renderer.h"
#include "d3d11-shader.h"
#include"d3d11-video-texture.h"

#include <d3d11.h>
#include <dxgi.h>

// D3D11 核心资源由渲染模块统一管理。
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_renderTarget = nullptr;

static bool createRenderTarget()
{
  if (!g_device || !g_swapChain)
    return false;

  // 获取交换链中将要显示到窗口的后台纹理。
  ID3D11Texture2D* backBuffer = nullptr;

  HRESULT result = g_swapChain->GetBuffer(
    0,
    __uuidof(ID3D11Texture2D),
    reinterpret_cast<void**>(&backBuffer));

  if (FAILED(result))
    return false;

  // 创建 RenderTargetView，使 GPU 可以向后台纹理绘制。
  result = g_device->CreateRenderTargetView(
    backBuffer,
    nullptr,
    &g_renderTarget);

  // RenderTargetView 已持有纹理引用，释放临时引用。
  backBuffer->Release();

  return SUCCEEDED(result);
}

static void setViewport(UINT width, UINT height)
{
  if (!g_context || width == 0 || height == 0)
    return;

  // Viewport 决定 Shader 实际绘制的窗口区域。
  D3D11_VIEWPORT viewport = {};
  viewport.Width = static_cast<float>(width);
  viewport.Height = static_cast<float>(height);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;

  g_context->RSSetViewports(1, &viewport);
}

bool initializeD3D11(HWND hwnd)
{
  DXGI_SWAP_CHAIN_DESC desc = {};

  // 双缓冲可以避免直接修改正在显示的画面。
  desc.BufferCount = 2;
  desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.OutputWindow = hwnd;
  desc.SampleDesc.Count = 1;
  desc.Windowed = TRUE;

  // MVP 先使用最简单的交换模式。
  desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  // BGRA 支持用于后续接入 DirectComposition。
  HRESULT result = D3D11CreateDeviceAndSwapChain(
    nullptr,
    D3D_DRIVER_TYPE_HARDWARE,
    nullptr,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    nullptr,
    0,
    D3D11_SDK_VERSION,
    &desc,
    &g_swapChain,
    &g_device,
    nullptr,
    &g_context);

  if (FAILED(result))
  {
    shutdownD3D11();
    return false;
  }

  if (!createRenderTarget())
  {
    shutdownD3D11();
    return false;
  }

  // 设置首次绘制使用的窗口尺寸。
  RECT clientRect = {};
  GetClientRect(hwnd, &clientRect);

  UINT width = static_cast<UINT>(
    clientRect.right - clientRect.left);

  UINT height = static_cast<UINT>(
    clientRect.bottom - clientRect.top);

  setViewport(width, height);

  // Shader 必须在 Device 创建完成后初始化。
  if (!initializeD3D11Shaders(g_device))
  {
    shutdownD3D11();
    return false;
  }

  return true;
}

void resizeD3D11(UINT width, UINT height)
{
  // 窗口最小化时宽高可能为 0，不能调整缓冲区。
  if (!g_device || !g_context || !g_swapChain ||
    width == 0 || height == 0)
  {
    return;
  }

  // ResizeBuffers 前必须解除绑定并释放旧 RenderTarget。
  g_context->OMSetRenderTargets(0, nullptr, nullptr);

  if (g_renderTarget)
  {
    g_renderTarget->Release();
    g_renderTarget = nullptr;
  }

  HRESULT result = g_swapChain->ResizeBuffers(
    0,
    width,
    height,
    DXGI_FORMAT_UNKNOWN,
    0);

  if (FAILED(result))
    return;

  if (!createRenderTarget())
    return;

  setViewport(width, height);
  renderD3D11();
}

void renderD3D11()
{
  if (!g_context || !g_renderTarget || !g_swapChain)
    return;

  // 先清除上一帧，便于发现 Shader 未覆盖的区域。
  const float background[] = {
      0.0f,
      0.0f,
      0.0f,
      1.0f
  };

  g_context->ClearRenderTargetView(
    g_renderTarget,
    background);

  // 将 Shader 输出绑定到窗口后台缓冲区。
  g_context->OMSetRenderTargets(
    1,
    &g_renderTarget,
    nullptr);

  // 全屏三角形使用 SV_VertexID，不需要 InputLayout。
  g_context->IASetInputLayout(nullptr);
  g_context->IASetPrimitiveTopology(
    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  if (!bindD3D11Shaders(g_context))
    return;

  if (!bindD3D11VideoTexture(g_context))
    return;

  // Shader 根据 SV_VertexID 自动生成三个顶点。
  g_context->Draw(3, 0);

  // 等待一次垂直同步后显示画面。
  g_swapChain->Present(1, 0);
}
bool updateD3D11VideoFrame(
  const unsigned char* data,
  int width,
  int height,
  int stride)
{
  if (!uploadD3D11VideoTexture(
    g_device,
    g_context,
    data,
    width,
    height,
    stride))
  {
    return false;
  }

  renderD3D11();
  return true;
}
void shutdownD3D11()
{
  // 先解除 Context 对资源的引用。
  if (g_context)
    g_context->ClearState();

  shutdownD3D11VideoTexture();

  // Shader 必须在 Device 之前释放。
  shutdownD3D11Shaders();

  if (g_renderTarget)
  {
    g_renderTarget->Release();
    g_renderTarget = nullptr;
  }

  if (g_swapChain)
  {
    g_swapChain->Release();
    g_swapChain = nullptr;
  }

  if (g_context)
  {
    g_context->Release();
    g_context = nullptr;
  }

  if (g_device)
  {
    g_device->Release();
    g_device = nullptr;
  }
}