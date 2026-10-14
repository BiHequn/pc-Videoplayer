#include "desktop-renderer.h"
#include "d3d11-shader.h"
#include "d3d11-video-texture.h"
#include "imgui_impl_dx11.h"
#include "imgui.h"
#include <wincodec.h>

bool DesktopRenderer::createTarget() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer;
    return SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&buffer))) &&
           SUCCEEDED(device_->CreateRenderTargetView(buffer.Get(), nullptr, &target_));
}
bool DesktopRenderer::initialize(HWND window) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2; desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.OutputWindow = window;
    desc.SampleDesc.Count = 1; desc.Windowed = TRUE; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION, &desc, &swapChain_, &device_, nullptr, &context_);
    if(FAILED(result)) result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION, &desc, &swapChain_, &device_, nullptr, &context_);
    if(FAILED(result) || !createTarget() || !initializeD3D11Shaders(device_.Get())) { shutdown(); return false; }
    imguiInitialized_ = ImGui_ImplDX11_Init(device_.Get(), context_.Get());
    return imguiInitialized_;
}
void DesktopRenderer::resize(unsigned int width, unsigned int height) {
    if(!swapChain_ || width == 0 || height == 0) return;
    context_->OMSetRenderTargets(0, nullptr, nullptr); target_.Reset();
    if(SUCCEEDED(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) createTarget();
}
bool DesktopRenderer::updateVideo(const std::shared_ptr<const PlaybackFrame>& frame, float brightness) {
    if(!frame) { videoView_.Reset(); videoTarget_.Reset(); serial_ = 0; width_ = height_ = 0; return true; }
    if(!device_ || frame->pixels.empty()) return false;
    if(width_ != frame->width || height_ != frame->height) {
        videoTarget_.Reset(); videoView_.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = frame->width; desc.Height = frame->height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if(FAILED(device_->CreateTexture2D(&desc, nullptr, &texture)) ||
           FAILED(device_->CreateRenderTargetView(texture.Get(), nullptr, &videoTarget_)) ||
           FAILED(device_->CreateShaderResourceView(texture.Get(), nullptr, &videoView_))) return false;
        width_ = frame->width; height_ = frame->height; serial_ = 0;
    }
    if(serial_ != frame->serial) {
        if(!uploadD3D11VideoTexture(device_.Get(), context_.Get(), frame->pixels.data(), frame->width, frame->height, frame->stride)) return false;
        serial_ = frame->serial;
    }
    ID3D11ShaderResourceView* empty = nullptr; context_->PSSetShaderResources(0, 1, &empty);
    auto* target = videoTarget_.Get(); context_->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr); context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF); context_->OMSetDepthStencilState(nullptr, 0);
    context_->RSSetState(nullptr); context_->GSSetShader(nullptr, nullptr, 0);
    setD3D11ShaderBrightness(context_.Get(), brightness);
    if(!bindD3D11Shaders(context_.Get()) || !bindD3D11VideoTexture(context_.Get())) return false;
    context_->Draw(3, 0);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}
void DesktopRenderer::begin() {
    if(!target_) return;
    const float color[]{.043f, .051f, .071f, 1};
    auto* target = target_.Get(); context_->OMSetRenderTargets(1, &target, nullptr);
    context_->ClearRenderTargetView(target, color);
}
void DesktopRenderer::present() {
    if(!target_) return;
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(1, 0);
}
bool DesktopRenderer::saveScreenshot(const wchar_t* path) {
    using Microsoft::WRL::ComPtr;
    if(!device_ || !swapChain_) return false;
    // DISCARD swap chains need a freshly rendered buffer after Present.
    begin(); ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    // Capture this application's back buffer, never another desktop window.
    ComPtr<ID3D11Texture2D> buffer, staging;
    if(FAILED(swapChain_->GetBuffer(0,IID_PPV_ARGS(&buffer)))) return false;
    D3D11_TEXTURE2D_DESC desc{}; buffer->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0;
    if(FAILED(device_->CreateTexture2D(&desc,nullptr,&staging))) return false;
    context_->CopyResource(staging.Get(),buffer.Get());
    D3D11_MAPPED_SUBRESOURCE data{};
    if(FAILED(context_->Map(staging.Get(),0,D3D11_MAP_READ,0,&data))) return false;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    ComPtr<IWICImagingFactory> factory; ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder; ComPtr<IWICBitmapFrameEncode> frame;
    bool ok=false;
    if(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))) &&
       SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path,GENERIC_WRITE)) &&
       SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder)) &&
       SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache)) &&
       SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
       SUCCEEDED(frame->SetSize(desc.Width,desc.Height))) {
        WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
        ok=SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA) &&
           SUCCEEDED(frame->WritePixels(desc.Height,data.RowPitch,data.RowPitch*desc.Height,static_cast<BYTE*>(data.pData))) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    frame.Reset(); encoder.Reset(); stream.Reset(); factory.Reset();
    if(SUCCEEDED(com)) CoUninitialize();
    context_->Unmap(staging.Get(),0); return ok;
}
void DesktopRenderer::shutdown() {
    if(imguiInitialized_) { ImGui_ImplDX11_Shutdown(); imguiInitialized_ = false; }
    if(context_) context_->ClearState();
    shutdownD3D11VideoTexture(); shutdownD3D11Shaders();
    videoView_.Reset(); videoTarget_.Reset(); target_.Reset(); swapChain_.Reset(); context_.Reset(); device_.Reset();
}
