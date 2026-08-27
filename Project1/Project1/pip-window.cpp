#include "pip-window.h"
#include"d3d11-renderer.h"
#include <windows.h>

static HWND g_hwnd = 0;
//缩放时保持16:9
static void keep16By9AspectRatio(WPARAM edge, RECT* rect)
{
  const int aspectWidth = 16;
  const int aspectHeight = 9;

  int width = rect->right - rect->left;
  int height = rect->bottom - rect->top;

  // 只拖动上边或下边时，根据高度计算宽度。
  if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM)
  {
    int targetWidth = height * aspectWidth / aspectHeight;
    rect->right = rect->left + targetWidth;
    return;
  }

  // 拖动左右边或四个角时，根据宽度计算高度。
  int targetHeight = width * aspectHeight / aspectWidth;

  if (edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
  {
    // 拖动上方时固定底边。
    rect->top = rect->bottom - targetHeight;
  }
  else
  {
    // 拖动其他方向时固定顶边。
    rect->bottom = rect->top + targetHeight;
  }
}

LRESULT CALLBACK PipWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)//窗口表示，消息类型，参数1，参数2
{
  if (msg == WM_NCHITTEST)
  {
    LRESULT hit = DefWindowProc(hwnd, msg, wParam, lParam);
    if (hit == HTCLIENT)
      return HTCAPTION; // 中间区域用来拖窗口
    return hit;
  }

  //把D3D11窗口接在PiP窗口
  //在窗口消息中绘制和释放
  if (msg == WM_SIZING)
  {
    RECT* resizeRect = reinterpret_cast<RECT*>(lParam);
    //用户拖动窗口边缘时。将尺寸修正为16:9
    keep16By9AspectRatio(wParam, resizeRect);
    return TRUE;

  }

  if (msg == WM_SIZE && wParam != SIZE_MINIMIZED)
  {
    UINT width = LOWORD(lParam);
    UINT height = HIWORD(lParam);
    //窗口大小变化，同步调整D3D11后台缓冲区
    resizeD3D11(width, height);
    return 0;

  }
  if (msg == WM_PAINT)
  {
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);
    renderD3D11();
    EndPaint(hwnd, &ps);
    return 0;

  }
  if (msg == WM_DESTROY)
  {
    shutdownD3D11();
    g_hwnd = 0;
    PostQuitMessage(0);
    return 0;
  }

  return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool createPipWindow()
{
  WNDCLASS wc;
  ZeroMemory(&wc, sizeof(wc));
  wc.lpfnWndProc = PipWndProc;
  wc.hInstance = GetModuleHandle(0);
  wc.lpszClassName = L"NativePlayerPip";
  wc.hCursor = LoadCursor(0, IDC_ARROW);
  //wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.hbrBackground = 0;

  RegisterClass(&wc);

  g_hwnd = CreateWindowEx(
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
    L"NativePlayerPip",
    L"Native Player",
    WS_POPUP | WS_THICKFRAME,
    100, 100, 640, 360,
    0, 0, wc.hInstance, 0);

  if (!g_hwnd)
    return false;

  if (!initializeD3D11(g_hwnd))
  {
    DestroyWindow(g_hwnd);
    g_hwnd = 0;
    return false;
  }

  ShowWindow(g_hwnd, SW_SHOW);
  renderD3D11();
  return true;
}

void runPipWindowLoop()
{
  MSG msg;
  ZeroMemory(&msg, sizeof(msg));

  while (GetMessage(&msg, 0, 0, 0) > 0)
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

void requestPipWindowClose()
{
  if (g_hwnd)
    PostMessage(g_hwnd, WM_CLOSE, 0, 0);
}

bool showPipVideoFrame(
  const unsigned char* data,
  int width,
  int height,
  int stride)
{
  return updateD3D11VideoFrame(
    data,
    width,
    height,
    stride);
}
