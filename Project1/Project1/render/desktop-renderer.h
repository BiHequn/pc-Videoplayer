#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include "app/playback-session.h"

class DesktopRenderer {
public:
    bool initialize(HWND window);
    void shutdown();
    void resize(unsigned int width, unsigned int height);
    bool updateVideo(const std::shared_ptr<const PlaybackFrame>& frame, float brightness);
    ID3D11ShaderResourceView* videoView() const { return videoView_.Get(); }
    void begin();
    void present();
    bool saveScreenshot(const wchar_t* path);
private:
    bool createTarget();
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_, videoTarget_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> videoView_;
    int width_ = 0, height_ = 0;
    std::uint64_t serial_ = 0;
    bool imguiInitialized_ = false;
};
