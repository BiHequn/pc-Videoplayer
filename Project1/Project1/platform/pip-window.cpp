#include "pip-window.h"
#include "media/ffmpeg-decoder.h"
#include "render/d3d11-renderer.h"

#include <windows.h>
#include"player-channel.h"
#include<iostream>
#include<windowsx.h>

static HWND g_hwnd = 0;
//临时按约30帧播放，1000/30约等于33毫秒
static const UINT_PTR kPlaybackTimerId = 1;

static const UINT kSetOpacityMessage = WM_APP + 1;
static const UINT kSetBrightnessMessage = WM_APP + 2;
static const UINT kSetPausedMessage = WM_APP + 3;

static int g_clockLogCount = 0;
static DecodedVideoFrame g_pendingFrame = {};
static bool g_hasPendingFrame = false;



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
  if (msg == WM_NCCALCSIZE)
  {
      // 让客户区覆盖整个窗口，隐藏系统边框。
      return 0;
  }

  if (msg == WM_NCHITTEST)
  {
      RECT rect = {};
      GetWindowRect(hwnd, &rect);

      const int x = GET_X_LPARAM(lParam);
      const int y = GET_Y_LPARAM(lParam);
      const int border = 8;

      const bool left = x < rect.left + border;
      const bool right = x >= rect.right - border;
      const bool top = y < rect.top + border;
      const bool bottom = y >= rect.bottom - border;

      if (top && left) return HTTOPLEFT;
      if (top && right) return HTTOPRIGHT;
      if (bottom && left) return HTBOTTOMLEFT;
      if (bottom && right) return HTBOTTOMRIGHT;

      if (left) return HTLEFT;
      if (right) return HTRIGHT;
      if (top) return HTTOP;
      if (bottom) return HTBOTTOM;

      // 中间区域用于拖动窗口。
      return HTCAPTION;
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
  if (msg == WM_TIMER && wParam == kPlaybackTimerId)
  {
      while (true)
      {
          if (!g_hasPendingFrame)
          {
              if (!decodeNextFfmpegFrame(
                  &g_pendingFrame))
              {
                  if (isFfmpegPlaybackFinished())
                  {
                      PlaybackEndReason reason =
                          getFfmpegPlaybackEndReason();

                      if (reason ==
                          PlaybackEndReason::Completed)
                      {
                          sendEnded();
                      }
                      else if (reason ==
                          PlaybackEndReason::DemuxError)
                      {
                          sendError("DEMUX_ERROR");
                      }
                      else if (reason ==
                          PlaybackEndReason::DecodeError)
                      {
                          sendError("DECODE_ERROR");
                      }
                      else
                      {
                          sendError("UNKNOWN_ERROR");
                      }

                      KillTimer(hwnd, kPlaybackTimerId);
                      DestroyWindow(hwnd);
                  }
                  return 0;
              }

              g_hasPendingFrame = true;
          }

          double audioClock =
              getFfmpegAudioClockSeconds();

          if (audioClock >= 0.0 &&
              g_pendingFrame.ptsSeconds >= 0.0)
          {
              double difference =
                  g_pendingFrame.ptsSeconds -
                  audioClock;

              if (g_clockLogCount < 20)
              {
                  std::cerr
                      << "Sync: video="
                      << g_pendingFrame.ptsSeconds
                      << ", audio="
                      << audioClock
                      << ", diff="
                      << difference
                      << " seconds"
                      << std::endl;
                  ++g_clockLogCount;
              }

              // 视频早了就等下一次定时器，晚太多就丢掉这一帧。
              if (difference > 0.05)
                  return 0;

              if (difference < -0.10)
              {
                  g_hasPendingFrame = false;
                  continue;
              }
          }

          if (!showPipVideoFrame(
              g_pendingFrame.data,
              g_pendingFrame.width,
              g_pendingFrame.height,
              g_pendingFrame.stride))
          {
              KillTimer(hwnd, kPlaybackTimerId);
              DestroyWindow(hwnd);
              return 0;
          }

          g_hasPendingFrame = false;
          return 0;
      }
  }
  if (msg == kSetPausedMessage)
  {
      bool paused = wParam != 0;
      if (paused)
      {
          //停止视频定时器，并暂停系统音频输出
          KillTimer(hwnd, kPlaybackTimerId);
          setFfmpegAudioPaused(true);
      }
      else
      {
          //恢复音频，并重新启动视频定时器
          setFfmpegAudioPaused(false);
          SetTimer(
              hwnd,
              kPlaybackTimerId,
              getFfmpegFrameIntervalMs(),
              nullptr);
      }
      return 0;
  }

  //亮度处理
  if (msg == kSetBrightnessMessage)
  {
      int  brightnessPercent =static_cast<int>(wParam);
      float brightness = static_cast<float>(brightnessPercent) /
          100.0f;
      setD3D11Brightness(brightness);
      return 0;
  }
  if (msg == kSetOpacityMessage)
  {
      int opacityPercent = static_cast<int>(wParam);
      BYTE alpha = static_cast<BYTE>(
          255 * opacityPercent / 100);

      SetLayeredWindowAttributes(
          hwnd,
          0,
          alpha,
          LWA_ALPHA);

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
    KillTimer(hwnd, kPlaybackTimerId);
    g_hasPendingFrame = false;
    g_pendingFrame = {};
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
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
    L"NativePlayerPip",
    L"Native Player",
    WS_POPUP | WS_THICKFRAME,
    100, 100, 640, 360,
    0, 0, wc.hInstance, 0);

  if (!g_hwnd)
    return false;

  //分层窗口创建后默认保持完全可见
  SetLayeredWindowAttributes(
      g_hwnd,
      0,
      255,
      LWA_ALPHA);


  if (!initializeD3D11(g_hwnd))
  {
    DestroyWindow(g_hwnd);
    g_hwnd = 0;
    return false;
  }
  ShowWindow(g_hwnd, SW_SHOW);
  SetWindowPos(
      g_hwnd,
      HWND_TOPMOST,
      0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  UpdateWindow(g_hwnd);
  renderD3D11();
  return true;
}


bool startPipPlaybackTimer()
{
    if (!g_hwnd)
        return false;
    UINT_PTR timerId = SetTimer(
        g_hwnd,
        kPlaybackTimerId,
        getFfmpegFrameIntervalMs(),
        nullptr);
    return timerId != 0;
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

bool requestPipWindowClose()
{
  if (g_hwnd)
    return PostMessage(
        g_hwnd,
        WM_CLOSE,
        0,
        0) != FALSE;

  return false;
}

void requestPipWindowOpacity(int opacityPercent)
{
    if (!g_hwnd)
        return;
    PostMessage(
        g_hwnd,
        kSetOpacityMessage,
        static_cast<WPARAM>(opacityPercent),
        0);
}

void requestPipWindowPaused(bool paused)
{
    if (!g_hwnd)
        return;
    PostMessage(
        g_hwnd,
        kSetPausedMessage,
        paused ? 1 : 0,
        0);
}

void requestPipWindowBrightness(
    int brightnessPercent)
{
    if (!g_hwnd)
        return;

    PostMessage(
        g_hwnd,
        kSetBrightnessMessage,
        static_cast<WPARAM>(brightnessPercent),
        0);
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
