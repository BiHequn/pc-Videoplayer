#include <windows.h>
#include <GL/gl.h>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "app/playback-session.h"
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>

#if __has_include("render/opengl-renderer.h")
#include "render/opengl-renderer.h"
#define HAVE_RENDERER 1
#endif

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

#ifdef HAVE_RENDERER
using ReadTexture = void (APIENTRY*)(GLenum, GLint, GLenum, GLenum, void*);
static std::vector<unsigned char> readTexture(OpenGLRenderer& renderer, int width, int height) {
    auto read = reinterpret_cast<ReadTexture>(wglGetProcAddress("glGetTexImage"));
    if (!read) read = reinterpret_cast<ReadTexture>(GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "glGetTexImage"));
    require(read != nullptr, "glGetTexImage unavailable");
    require(renderer.frameTexture() != 0, "renderer produced no texture");
    glBindTexture(GL_TEXTURE_2D, renderer.frameTexture());
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    read(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    require(glGetError() == GL_NO_ERROR, "GL texture read failed");
    return pixels;
}
static void pixel(const std::vector<unsigned char>& bytes, int width, int x, int y,
                  std::array<int,3> expected, int tolerance = 3) {
    const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
    for (int c = 0; c < 3; ++c) {
        if (std::abs(int(bytes.at(offset+c)) - expected[c]) > tolerance) {
            std::cerr << "pixel(" << x << ',' << y << ") channel " << c << ": "
                      << int(bytes[offset+c]) << " expected " << expected[c] << '\n';
            throw std::runtime_error("hand-color fixture mismatch");
        }
    }
    require(bytes.at(offset+3) == 255, "video output alpha is not opaque");
}
static std::shared_ptr<PlaybackFrame> bgra() {
    auto frame = std::make_shared<PlaybackFrame>();
    frame->width=8; frame->height=4; frame->stride=40; frame->serial=0;
    frame->pixels.resize(160, 177);
    const std::array<std::array<unsigned char,4>,4> colors{{{0,0,255,255},{0,255,0,255},{255,0,0,255},{255,255,255,255}}};
    for (int y=0;y<4;++y) for (int x=0;x<8;++x) {
        auto color=colors[(y>=2)*2+(x>=4)];
        std::copy(color.begin(),color.end(),frame->pixels.begin()+y*40+x*4);
    }
    return frame;
}
static std::shared_ptr<PlaybackFrame> yuv() {
    auto frame=std::make_shared<PlaybackFrame>();
    frame->width=5; frame->height=3; frame->serial=0;
    frame->pixelFormat=VideoPixelFormat::Yuv420p;
    frame->colorRange=VideoColorRange::Limited;
    frame->colorSpace=VideoColorSpace::Bt601;
    frame->planeOffsets={2,26,34}; frame->planeStrides={8,4,4}; frame->stride=8;
    frame->pixels.resize(42, 199);
    // BT.601 limited-range red: Y=81, U=90, V=240. Odd size and padded rows.
    for(int y=0;y<3;++y) for(int x=0;x<5;++x) frame->pixels[2+y*8+x]=81;
    for(int y=0;y<2;++y) for(int x=0;x<3;++x) {
        frame->pixels[26+y*4+x]=90; frame->pixels[34+y*4+x]=240;
    }
    return frame;
}
static void tests(HWND window, const std::filesystem::path& screenshot) {
    OpenGLRenderer renderer;
    require(!renderer.initialize(nullptr), "null HWND must fail");
    require(renderer.initialize(window), "WGL 3.3 initialization failed");
    std::cout << "GL " << glGetString(GL_VERSION) << " / " << glGetString(GL_RENDERER) << '\n';
    auto color=bgra();
    require(renderer.updateVideo(color, {}), "BGRA upload failed");
    auto image=readTexture(renderer,8,4);
    pixel(image,8,1,0,{255,0,0}); pixel(image,8,6,0,{0,255,0});
    pixel(image,8,1,3,{0,0,255}); pixel(image,8,6,3,{255,255,255});
    VideoEffects effects;
    effects.brightness=.5f;
    require(renderer.updateVideo(color,effects), "brightness update failed");
    pixel(readTexture(renderer,8,4),8,1,0,{128,0,0});
    effects.brightness=1; effects.filter=VideoFilter::Grayscale;
    require(renderer.updateVideo(color,effects), "grayscale update failed");
    pixel(readTexture(renderer,8,4),8,1,0,{76,76,76});
    effects.filter=VideoFilter::Invert;
    require(renderer.updateVideo(color,effects), "invert update failed");
    pixel(readTexture(renderer,8,4),8,1,0,{0,255,255});
    effects.filter=VideoFilter::Normal; effects.rotation=90;
    require(renderer.updateVideo(color,effects), "90 degree rotation failed");
    image=readTexture(renderer,4,8);
    pixel(image,4,0,1,{0,0,255}); pixel(image,4,3,1,{255,0,0});
    pixel(image,4,0,6,{255,255,255}); pixel(image,4,3,6,{0,255,0});
    effects.rotation=180;
    require(renderer.updateVideo(color,effects), "180 degree rotation failed");
    pixel(readTexture(renderer,8,4),8,1,0,{255,255,255});
    effects.rotation=270;
    require(renderer.updateVideo(color,effects), "270 degree rotation failed");
    pixel(readTexture(renderer,4,8),4,0,1,{0,255,0});
    effects.rotation=0; effects.zoom=.5f;
    require(renderer.updateVideo(color,effects), "zoom out failed");
    pixel(readTexture(renderer,8,4),8,0,0,{0,0,0});
    auto gradient=bgra();
    for(int y=0;y<4;++y) for(int x=0;x<8;++x) {
        gradient->pixels[y*40+x*4]=0; gradient->pixels[y*40+x*4+1]=0;
        gradient->pixels[y*40+x*4+2]=static_cast<unsigned char>(x*32);
    }
    effects.zoom=2;
    require(renderer.updateVideo(gradient,effects), "zoom in failed");
    pixel(readTexture(renderer,8,4),8,0,0,{56,0,0});
    auto planar=yuv(); effects={};
    require(renderer.updateVideo(planar,effects), "YUV upload failed");
    pixel(readTexture(renderer,5,3),5,0,0,{254,0,0});
    pixel(readTexture(renderer,5,3),5,4,2,{254,0,0});
    planar=std::make_shared<PlaybackFrame>(*planar);
    planar->colorRange=VideoColorRange::Full;
    for(int y=0;y<3;++y) for(int x=0;x<5;++x) planar->pixels[2+y*8+x]=76;
    for(int y=0;y<2;++y) for(int x=0;x<3;++x) { planar->pixels[26+y*4+x]=85; planar->pixels[34+y*4+x]=255; }
    require(renderer.updateVideo(planar,{}), "full range upload failed");
    pixel(readTexture(renderer,5,3),5,4,2,{254,0,0});
    planar=std::make_shared<PlaybackFrame>(*planar);
    planar->colorRange=VideoColorRange::Limited; planar->colorSpace=VideoColorSpace::Bt709;
    for(int y=0;y<3;++y) for(int x=0;x<5;++x) planar->pixels[2+y*8+x]=63;
    for(int y=0;y<2;++y) for(int x=0;x<3;++x) { planar->pixels[26+y*4+x]=102; planar->pixels[34+y*4+x]=240; }
    require(renderer.updateVideo(planar,{}), "BT.709 upload failed");
    pixel(readTexture(renderer,5,3),5,4,2,{255,1,0});
    auto malformed=std::make_shared<PlaybackFrame>(*planar); malformed->planeOffsets[2]=41;
    require(!renderer.updateVideo(malformed,{}), "out of bounds plane accepted");
    malformed->planeOffsets[2]=SIZE_MAX;
    require(!renderer.updateVideo(malformed,{}), "overflow plane accepted");
    effects.zoom=0; require(!renderer.updateVideo(planar,effects), "zero zoom accepted");
    effects={}; effects.brightness=NAN; require(!renderer.updateVideo(planar,effects), "NaN brightness accepted");
    // PNG orientation must agree with default ImGui UVs after the swap.
    require(renderer.updateVideo(color,{}), "BGRA switch before screenshot failed");
    renderer.resize(0,0); renderer.resize(64,48);
    ImGui_ImplOpenGL3_NewFrame(); ImGui::GetIO().DisplaySize=ImVec2(64,48); ImGui::GetIO().DeltaTime=1.f/60;
    ImGui::NewFrame();
    ImGui::GetBackgroundDrawList()->AddImage(static_cast<ImTextureID>(renderer.frameTexture()),ImVec2(0,0),ImVec2(64,48));
    ImGui::Render(); renderer.begin(); renderer.present();
    require(renderer.saveScreenshot(screenshot.c_str()), "PNG screenshot failed after present");
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> png;
    require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))),"WIC factory failed");
    require(SUCCEEDED(factory->CreateDecoderFromFilename(screenshot.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder)),"PNG read failed");
    require(SUCCEEDED(decoder->GetFrame(0,&png)),"PNG frame missing");
    UINT w=0,h=0; require(SUCCEEDED(png->GetSize(&w,&h))&&w==64&&h==48,"screenshot size mismatch");
    ComPtr<IWICFormatConverter> converter; require(SUCCEEDED(factory->CreateFormatConverter(&converter)),"converter failed");
    require(SUCCEEDED(converter->Initialize(png.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)),"PNG convert failed");
    image.resize(64*48*4); require(SUCCEEDED(converter->CopyPixels(nullptr,64*4,static_cast<UINT>(image.size()),image.data())),"PNG pixels failed");
    pixel(image,64,8,6,{255,0,0}); pixel(image,64,56,6,{0,255,0});
    pixel(image,64,8,42,{0,0,255}); pixel(image,64,56,42,{255,255,255});
    // Arbitrary byte strides need a row repack for GL's pixel-based row length.
    auto unaligned=std::make_shared<PlaybackFrame>(*color);
    unaligned->stride=35; unaligned->pixels.assign(137,133);
    for(int row=0;row<4;++row) std::copy_n(color->pixels.data()+row*40,32,unaligned->pixels.data()+row*35);
    require(renderer.updateVideo(unaligned,{}), "unaligned BGRA stride failed");
    pixel(readTexture(renderer,8,4),8,6,3,{255,255,255});
    planar=std::make_shared<PlaybackFrame>(*yuv());
    // A second hand-derived YUV color catches U/V swaps beyond the red fixture.
    for(int row=0;row<3;++row) for(int x=0;x<5;++x) planar->pixels[2+row*8+x]=145;
    for(int row=0;row<2;++row) for(int x=0;x<3;++x) { planar->pixels[26+row*4+x]=54; planar->pixels[34+row*4+x]=34; }
    require(renderer.updateVideo(planar,{}), "green YUV upload failed");
    pixel(readTexture(renderer,5,3),5,4,2,{0,255,1});
    require(renderer.updateVideo(nullptr,{}),"clear failed"); require(renderer.frameTexture()==0,"clear retained texture");
    require(renderer.updateVideo(color,{}),"reupload failed");
    renderer.shutdown(); renderer.shutdown(); require(renderer.frameTexture()==0,"shutdown retained texture");
    require(!renderer.updateVideo(color,{}),"upload without context accepted");
    require(renderer.initialize(window),"same HWND reinitialization failed");
    require(renderer.updateVideo(color,{}),"upload after reinitialize failed");
    pixel(readTexture(renderer,8,4),8,1,0,{255,0,0}); renderer.shutdown();
}
#endif
int main(int argc,char** argv) {
    try {
#ifndef HAVE_RENDERER
        require(false,"OpenGLRenderer missing: cannot render hand-color fixture");
#else
        require(argc==2,"usage: opengl-renderer-test screenshot.png");
        const auto screenshot=std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        std::filesystem::create_directories(screenshot.parent_path());
        const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"OpenGLColorFixture"; wc.style=CS_OWNDC;
        require(RegisterClassW(&wc)!=0,"register fixture window failed");
        RECT rect{0,0,64,48}; AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
        HWND window=CreateWindowW(wc.lpszClassName,L"GL color fixture",WS_OVERLAPPEDWINDOW,0,0,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,wc.hInstance,nullptr);
        require(window!=nullptr,"fixture HWND failed");
        ImGui::CreateContext(); ImGui::GetIO().IniFilename=nullptr;
        tests(window,screenshot);
        ImGui::DestroyContext(); DestroyWindow(window); UnregisterClassW(wc.lpszClassName,wc.hInstance);
        if(SUCCEEDED(com)) CoUninitialize();
        std::cout<<"PASS: real WGL BGRA/YUV colors, effects, bounds, PNG and lifecycle\n";
#endif
        return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; }
}
