#include "desktop-player.h"
#include "playback-session.h"
#include "render/desktop-renderer.h"
#include "render/opengl-renderer.h"
#include "cache-controller.h"
#include "desktop-state.h"
#include "demo-http-server.h"
#include "service/service-client.h"
#include "service/local-server.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_opengl3.h"
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace {
constexpr ImVec4 kAccent{.38f, .80f, .68f, 1};
constexpr ImVec4 kMuted{.48f, .54f, .63f, 1};
constexpr ImVec4 kText{.88f, .92f, .97f, 1};
std::string overrideDataRoot;
struct UiRenderer {
    bool opengl=true;
    DesktopRenderer d3d;
    OpenGLRenderer gl;
    bool initialize(HWND window){return opengl?gl.initialize(window):d3d.initialize(window);}
    void resize(unsigned int w,unsigned int h){if(opengl)gl.resize(w,h);else d3d.resize(w,h);}
    bool updateVideo(const std::shared_ptr<const PlaybackFrame>& frame,VideoEffects effects){return opengl?gl.updateVideo(frame,effects):d3d.updateVideo(frame,effects.brightness);}
    ImTextureID texture()const{return opengl?static_cast<ImTextureID>(gl.frameTexture()):reinterpret_cast<ImTextureID>(d3d.videoView());}
    void newFrame(){if(opengl)ImGui_ImplOpenGL3_NewFrame();else ImGui_ImplDX11_NewFrame();}
    void begin(){if(opengl)gl.begin();else d3d.begin();}
    void present(){if(opengl)gl.present();else d3d.present();}
    bool saveScreenshot(const wchar_t* path){return opengl?gl.saveScreenshot(path):d3d.saveScreenshot(path);}
    void shutdown(){if(opengl)gl.shutdown();else d3d.shutdown();}
};
struct DesktopApp {
    HWND window = nullptr;
    UiRenderer renderer;
    PlaybackSession player;
    CacheController cache;
    ServiceClient service;
    std::shared_ptr<LocalServer> localServer;
    std::future<std::shared_ptr<LocalServer>> serverStart;
    std::string serverError;
    std::shared_ptr<DemoHttpServer> demo;
    char networkUrl[2048]{}, host[128]="127.0.0.1", username[96]{}, password[128]{};
    int port=18760, networkMode=1;
    std::uint64_t openedCacheGeneration=0;
    bool cacheAutoPlay=false,cachePlayWhenComplete=false,serverStopping=false;
    std::vector<std::string> recent;
    VideoDecoderBackend decoder = VideoDecoderBackend::Auto;
    float brightness = 1;
    VideoEffects effects;
    bool fullscreen = false, topmost = false, showInfo = false, quitting = false, minimized = false;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    int page = 0;
    double seekValue = 0;
    bool seeking = false;
    std::string renderError;
    std::vector<DownloadRecord> downloads;
    std::string stateError,stateLoadError,logError,persistedSignature,lastStatusSignature,cleanupSource,cleanupTaskKey,cleanupConfirmSource,cleanupConfirmKey,cleanupNotice,removedCacheSource;
    ULONGLONG lastPersistTick=0;
    std::future<std::pair<bool,std::string>> cacheCleanup;
    bool stateSaveSmoke=false,stateLoadSmoke=false;
};
std::string dataRoot();
DesktopState captureDesktopState(const DesktopApp& app);
void persistDesktopState(DesktopApp& app,bool force=false);
std::string utf8(const wchar_t* value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if(size <= 1) return {};
    std::string output(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, output.data(), size, nullptr, nullptr);
    output.pop_back(); return output;
}
std::string basename(const std::string& path) {
    const auto pos = path.find_last_of("/\\"); return pos == std::string::npos ? path : path.substr(pos + 1);
}
std::string timeText(double seconds) {
    const auto time = static_cast<int>(std::max(0.0, seconds));
    char buffer[48]{};
    if(time >= 3600) sprintf_s(buffer, "%d:%02d:%02d", time / 3600, time / 60 % 60, time % 60);
    else sprintf_s(buffer, "%02d:%02d", time / 60, time % 60);
    return buffer;
}
const char* statusText(PlaybackState state) {
    switch(state) {
      case PlaybackState::Opening: return "正在打开";
      case PlaybackState::Playing: return "正在播放";
      case PlaybackState::Buffering: return "正在缓冲";
      case PlaybackState::Paused: return "已暂停";
      case PlaybackState::Ended: return "播放结束";
      case PlaybackState::Error: return "播放失败";
      default: return "准备就绪";
    }
}
const char* backendText(VideoDecoderBackend backend) {
    return backend == VideoDecoderBackend::Ffmpeg ? "FFmpeg 软件解码" :
           backend == VideoDecoderBackend::MediaFoundation ? "Media Foundation" : "自动选择";
}
void openSource(DesktopApp& app, const std::string& path) {
    if(path.empty()) return;
    if(path!=app.cache.snapshot().playbackUrl) {
        app.recent.erase(std::remove(app.recent.begin(), app.recent.end(), path), app.recent.end());
        app.recent.insert(app.recent.begin(), path); if(app.recent.size() > 10) app.recent.resize(10);
    }
    app.page = 0; app.seeking = false; app.renderError.clear(); app.player.open(path, app.decoder,app.renderer.opengl);
}
void pickFile(DesktopApp& app) {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = app.window; dialog.lpstrFile = path; dialog.nMaxFile = 32768;
    dialog.lpstrTitle = L"选择本地视频";
    dialog.lpstrFilter = L"视频文件\0*.mp4;*.mkv;*.avi;*.mov;*.flv;*.ts;*.webm;*.m4v;*.wmv\0所有文件\0*.*\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if(GetOpenFileNameW(&dialog)) openSource(app, utf8(path));
}
void toggleFullscreen(DesktopApp& app) {
    if(!app.fullscreen) {
        GetWindowPlacement(app.window, &app.placement);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(app.window, MONITOR_DEFAULTTONEAREST), &info);
        SetWindowLongPtrW(app.window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(app.window, app.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, info.rcMonitor.left, info.rcMonitor.top,
          info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrW(app.window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(app.window, &app.placement);
        SetWindowPos(app.window, app.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0,0,0,0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
    }
    app.fullscreen = !app.fullscreen;
}
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if(message == WM_NCCREATE) {
        app = static_cast<DesktopApp*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if(ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) return 1;
    if(message == WM_SIZE && app) {
        app->minimized = wParam == SIZE_MINIMIZED;
        if(!app->minimized) app->renderer.resize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    }
    if(message == WM_GETMINMAXINFO) {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam); info->ptMinTrackSize = {960, 640}; return 0;
    }
    if(message == WM_DROPFILES && app) {
        auto drop = reinterpret_cast<HDROP>(wParam); const UINT size = DragQueryFileW(drop, 0, nullptr, 0);
        std::vector<wchar_t> path(size + 1); DragQueryFileW(drop, 0, path.data(), size + 1); DragFinish(drop);
        openSource(*app, utf8(path.data())); return 0;
    }
    if(message == WM_CLOSE && app) { app->quitting = true; return 0; }
    if(message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if(message == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_KEYMENU) return 0;
    return DefWindowProcW(window, message, wParam, lParam);
}
void theme(float scale) {
    ImGui::StyleColorsDark(); auto& style = ImGui::GetStyle();
    style.WindowPadding = {24, 20}; style.FramePadding = {12, 8}; style.ItemSpacing = {12, 12};
    style.WindowRounding = 0; style.ChildRounding = 12; style.FrameRounding = 7; style.GrabRounding = 7;
    style.WindowBorderSize = 0; style.ChildBorderSize = 0;
    auto* c = style.Colors;
    c[ImGuiCol_WindowBg] = {.043f,.051f,.071f,1};
    c[ImGuiCol_ChildBg] = {.065f,.078f,.105f,1};
    c[ImGuiCol_Text] = kText; c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_FrameBg] = {.10f,.125f,.16f,1};
    c[ImGuiCol_FrameBgHovered] = {.15f,.19f,.23f,1};
    c[ImGuiCol_FrameBgActive] = {.18f,.24f,.27f,1};
    c[ImGuiCol_Button] = {.10f,.13f,.17f,1};
    c[ImGuiCol_ButtonHovered] = {.16f,.23f,.26f,1};
    c[ImGuiCol_ButtonActive] = {.20f,.32f,.32f,1};
    c[ImGuiCol_Header] = {.12f,.23f,.22f,1};
    c[ImGuiCol_HeaderHovered] = {.15f,.28f,.27f,1};
    c[ImGuiCol_CheckMark] = kAccent; c[ImGuiCol_SliderGrab] = kAccent;c[ImGuiCol_PlotHistogram]=kAccent;
    c[ImGuiCol_SliderGrabActive] = {.55f,.94f,.80f,1}; c[ImGuiCol_Separator] = {.13f,.16f,.20f,1};
    style.ScaleAllSizes(scale);
}
bool accentButton(const char* label, ImVec2 size = {}) {
    ImGui::PushStyleColor(ImGuiCol_Button, {.20f,.54f,.44f,1});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.26f,.65f,.53f,1});
    const bool pressed = ImGui::Button(label, size); ImGui::PopStyleColor(2); return pressed;
}
void subtitle(const char* text) { ImGui::TextColored(kMuted, "%s", text); }
void shortcuts(DesktopApp& app, const PlaybackSnapshot& state) {
    if(ImGui::GetIO().WantTextInput) return;
    if(ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) pickFile(app);
    const bool active = state.state == PlaybackState::Playing || state.state == PlaybackState::Paused || state.state==PlaybackState::Buffering;
    if(active && ImGui::IsKeyPressed(ImGuiKey_Space)) app.player.pause(state.state != PlaybackState::Paused);
    if(active && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) app.player.seek(state.position - 10);
    if(active && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) app.player.seek(state.position + 10);
    if(ImGui::IsKeyPressed(ImGuiKey_F11)) toggleFullscreen(app);
    if(app.fullscreen && ImGui::IsKeyPressed(ImGuiKey_Escape)) toggleFullscreen(app);
}
void sidebar(DesktopApp& app, float scale) {
    ImGui::BeginChild("sidebar", {190*scale, 0});
    ImGui::TextColored(kAccent, "NATIVE / PLAYER"); subtitle("本地媒体工作台");
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    const char* pages[]{"播放器", "最近打开", "播放设置", "网络播放", "账号与连接", "传输中心"};
    for(int i=0;i<6;++i) if(ImGui::Selectable(pages[i], app.page == i, 0, {0,36*scale})) app.page=i;
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    if(accentButton("打开视频", {-1,38*scale})) pickFile(app);
    subtitle("Ctrl + O  选择文件");
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight()-145*scale));
    subtitle("快捷操作");
    ImGui::Text("空格      播放 / 暂停");
    ImGui::Text("左右键    跳转 10 秒");
    ImGui::Text("F11       全屏切换");
    subtitle(app.renderer.opengl?"FFmpeg / OpenGL":"FFmpeg / D3D11");
    ImGui::EndChild();
}
void videoPanel(DesktopApp& app, const PlaybackSnapshot& state, const std::shared_ptr<const PlaybackFrame>& frame, float scale) {
    ImGui::BeginChild("video-panel", {0, -165*scale}, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 origin=ImGui::GetCursorScreenPos(), area=ImGui::GetContentRegionAvail();
    auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin,{origin.x+area.x,origin.y+area.y},IM_COL32(8,12,18,255),12*scale);
    if(frame && app.renderer.texture()) {
        const bool rotated=app.renderer.opengl && app.effects.rotation%180!=0;
        const int width=rotated?frame->height:frame->width,height=rotated?frame->width:frame->height;
        const float ratio=std::min(area.x/width,area.y/height);
        const ImVec2 size{width*ratio,height*ratio};
        ImGui::SetCursorScreenPos({origin.x+(area.x-size.x)/2,origin.y+(area.y-size.y)/2});
        ImGui::Image(app.renderer.texture(),size);
        if(state.state == PlaybackState::Paused || state.state == PlaybackState::Buffering || state.state == PlaybackState::Ended || state.state == PlaybackState::Error) {
            draw->AddRectFilled({origin.x+16*scale,origin.y+16*scale},{origin.x+118*scale,origin.y+48*scale},IM_COL32(20,31,42,235),6*scale);
            draw->AddText({origin.x+28*scale,origin.y+22*scale},IM_COL32(220,235,240,255),statusText(state.state));
        }
    } else {
        const float centerX=origin.x+area.x/2, centerY=origin.y+area.y/2;
        draw->AddCircleFilled({centerX,centerY-65*scale},34*scale,IM_COL32(26,53,53,255));
        draw->AddTriangleFilled({centerX-8*scale,centerY-79*scale},{centerX-8*scale,centerY-51*scale},{centerX+16*scale,centerY-65*scale},IM_COL32(96,204,173,255));
        const char* title=state.state == PlaybackState::Opening ? "正在准备视频" : state.state == PlaybackState::Error ? "视频未能打开" : "打开一段视频，开始播放";
        const auto titleSize=ImGui::CalcTextSize(title);
        ImGui::SetCursorScreenPos({centerX-titleSize.x/2,centerY-10*scale}); ImGui::TextUnformatted(title);
        const char* hint=state.state == PlaybackState::Opening ? "读取媒体信息与解码首帧，窗口仍可操作" : "选择本地文件，或将视频拖放到这里";
        const auto hintSize=ImGui::CalcTextSize(hint);
        ImGui::SetCursorScreenPos({centerX-hintSize.x/2,centerY+26*scale}); subtitle(hint);
        if(state.state != PlaybackState::Opening) {
            ImGui::SetCursorScreenPos({centerX-75*scale,centerY+65*scale});
            if(accentButton("选择视频文件",{150*scale,40*scale})) pickFile(app);
        } else {
            ImGui::SetCursorScreenPos({centerX-50*scale,centerY+65*scale});
            if(ImGui::Button("取消打开",{100*scale,40*scale})) app.player.stop();
        }
    }
    ImGui::EndChild();
}
void controls(DesktopApp& app, const PlaybackSnapshot& state, float scale) {
    ImGui::BeginChild("controls",{0,0});
    const bool active=state.state == PlaybackState::Playing || state.state == PlaybackState::Paused || state.state==PlaybackState::Buffering;
    if(!app.seeking) app.seekValue=state.position;
    const auto cache=app.cache.snapshot();
    if(!cache.playbackUrl.empty() && state.source==cache.playbackUrl && cache.total) {
        const auto origin=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x;
        auto* draw=ImGui::GetWindowDrawList();draw->AddRectFilled(origin,{origin.x+width,origin.y+4*scale},IM_COL32(25,34,45,255));
        if(cache.segmentsTotal)draw->AddRectFilled(origin,{origin.x+width*float(double(cache.downloaded)/cache.total),origin.y+4*scale},IM_COL32(57,119,108,255));
        else for(const auto& range:cache.ranges)draw->AddRectFilled({origin.x+width*float(double(range.first)/cache.total),origin.y},
            {origin.x+width*float(double(range.second)/cache.total),origin.y+4*scale},IM_COL32(57,119,108,255));
        ImGui::Dummy({0,5*scale});
    }
    ImGui::BeginDisabled(!active || state.duration <= 0);
    ImGui::SetNextItemWidth(std::max(80.0f,ImGui::GetContentRegionAvail().x-140*scale));
    const double zero=0, duration=std::max(0.0,state.duration);
    ImGui::SliderScalar("##timeline",ImGuiDataType_Double,&app.seekValue,&zero,&duration,"");
    if(ImGui::IsItemActivated()) app.seeking=true;
    if(ImGui::IsItemDeactivated()) { if(app.seeking) app.player.seek(app.seekValue); app.seeking=false; }
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::Text("%s / %s",timeText(app.seekValue).c_str(),timeText(state.duration).c_str());
    ImGui::BeginDisabled(!active && state.source.empty());
    if(accentButton(state.state == PlaybackState::Paused ? "继续播放" : active ? "暂停" : "重新播放",{110*scale,36*scale})) {
        if(active) app.player.pause(state.state != PlaybackState::Paused); else openSource(app,state.source);
    }
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(!active && state.state != PlaybackState::Opening);
    if(ImGui::Button("停止",{65*scale,36*scale})) app.player.stop();
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(!active);
    ImGui::SetNextItemWidth(92*scale);
    const char* rates[]{"0.5x","1.0x","1.5x","2.0x"}; const double values[]{.5,1,1.5,2};
    int selected=state.rate == .5 ? 0 : state.rate == 1.5 ? 2 : state.rate == 2 ? 3 : 1;
    if(ImGui::Combo("##rate",&selected,rates,4)) app.player.setRate(values[selected]);
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::SetNextItemWidth(125*scale); float volume=state.volume*100;
    if(ImGui::SliderFloat("音量",&volume,0,100,"%.0f%%")) app.player.setVolume(volume/100);
    ImGui::SameLine();
    if(ImGui::Button(app.fullscreen ? "退出全屏" : "全屏",{90*scale,36*scale})) toggleFullscreen(app);
    if(state.rate != 1) subtitle("当前倍速采用视频时钟，音频静音；恢复 1.0x 后恢复声音。");
    else subtitle("音频主时钟同步  ·  视频早到等待，晚到丢帧");
    if(!state.error.empty()) { ImGui::TextColored({1,.52f,.48f,1},"%s",state.error.c_str()); }
    else if(!app.renderError.empty()) ImGui::TextColored({1,.52f,.48f,1},"%s",app.renderError.c_str());
    ImGui::EndChild();
}
void settings(DesktopApp& app, float scale) {
    ImGui::BeginChild("settings",{0,0});
    ImGui::Text("播放设置"); subtitle("解码设置在下次打开文件时生效"); ImGui::Separator();
    const char* backends[]{"自动：优先 MF，失败回退 FFmpeg","FFmpeg 软件解码","Media Foundation"};
    int decoder=app.decoder == VideoDecoderBackend::Ffmpeg ? 1 : app.decoder == VideoDecoderBackend::MediaFoundation ? 2 : 0;
    ImGui::SetNextItemWidth(410*scale);
    if(ImGui::Combo("解码器",&decoder,backends,3)) app.decoder=decoder == 1 ? VideoDecoderBackend::Ffmpeg : decoder == 2 ? VideoDecoderBackend::MediaFoundation : VideoDecoderBackend::Auto;
    ImGui::SetNextItemWidth(300*scale); ImGui::SliderFloat("画面亮度",&app.brightness,.25f,2.0f,"%.2fx");
    if(ImGui::Button("恢复默认亮度")) app.brightness=1;
    ImGui::BeginDisabled(!app.renderer.opengl);
    int rotation=app.effects.rotation/90;
    const char* rotations[]{"0°","90°","180°","270°"};
    ImGui::SetNextItemWidth(180*scale);if(ImGui::Combo("画面旋转",&rotation,rotations,4))app.effects.rotation=rotation*90;
    ImGui::SetNextItemWidth(300*scale);ImGui::SliderFloat("画面缩放",&app.effects.zoom,.5f,3.f,"%.2fx");
    int filter=static_cast<int>(app.effects.filter);const char* filters[]{"原色","灰度","反色"};
    ImGui::SetNextItemWidth(180*scale);if(ImGui::Combo("画面滤镜",&filter,filters,3))app.effects.filter=static_cast<VideoFilter>(filter);
    ImGui::EndDisabled();
    if(ImGui::Checkbox("窗口置顶",&app.topmost)) SetWindowPos(app.window,app.topmost ? HWND_TOPMOST : HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
    ImGui::Checkbox("显示播放技术信息",&app.showInfo);
    ImGui::Spacing(); ImGui::Separator(); subtitle("本地与网络播放器");
    ImGui::TextWrapped("支持本地文件、网络点播、边下边播、本地缓存和传输恢复。");
    if(app.showInfo)ImGui::TextWrapped("OpenGL 模式上传 YUV 平面并由着色器转换颜色；--renderer d3d11 可启动兼容渲染器。");
    ImGui::EndChild();
}
void recent(DesktopApp& app, float scale) {
    ImGui::BeginChild("recent",{0,0}); ImGui::Text("最近打开"); subtitle("已保存的播放记录，点击可重新播放");
    ImGui::Separator();
    if(app.recent.empty()) { ImGui::Spacing(); ImGui::Text("还没有打开视频。"); if(accentButton("选择视频")) pickFile(app); }
    const auto paths=app.recent;
    for(std::size_t i=0;i<paths.size();++i) {
        ImGui::PushID(static_cast<int>(i));
        if(ImGui::Selectable(basename(paths[i]).c_str(),false,0,{0,32*scale})) openSource(app,paths[i]);
        ImGui::PushTextWrapPos(); subtitle(paths[i].c_str()); ImGui::PopTextWrapPos(); ImGui::Separator(); ImGui::PopID();
    }
    ImGui::EndChild();
}
std::string dataRoot() {
    if(!overrideDataRoot.empty())return overrideDataRoot;
    wchar_t local[32768]{};
    const DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
    auto root=length&&length<32768?std::filesystem::path(local):std::filesystem::temp_directory_path();
    return (root/L"NativePlayer").u8string();
}
DesktopState captureDesktopState(const DesktopApp& app) {
    DesktopState state;state.recent=app.recent;state.downloads=app.downloads;
    state.decoder=app.decoder==VideoDecoderBackend::Ffmpeg?1:app.decoder==VideoDecoderBackend::MediaFoundation?2:0;
    state.brightness=app.brightness;state.volume=app.player.snapshot().volume;state.rotation=app.effects.rotation;
    state.zoom=app.effects.zoom;state.filter=static_cast<int>(app.effects.filter);state.networkMode=app.networkMode;
    state.showInfo=app.showInfo;state.host=app.host;state.port=std::clamp(app.port,1,65535);return state;
}
void persistDesktopState(DesktopApp& app,bool force) {
    const auto now=GetTickCount64();if(!force&&now-app.lastPersistTick<1000)return;app.lastPersistTick=now;
    const auto cache=app.cache.snapshot();
    if(!cache.source.empty()&&cache.source!=app.removedCacheSource&&cache.state!=CacheState::Idle) {
        auto found=std::find_if(app.downloads.begin(),app.downloads.end(),[&](const DownloadRecord& r){return r.source==cache.source;});
        DownloadRecord record{cache.source,cache.total,cache.downloaded,std::to_string(static_cast<int>(cache.state)),false};
        if(found==app.downloads.end()){app.downloads.insert(app.downloads.begin(),std::move(record));if(app.downloads.size()>100)app.downloads.resize(100);}
        else {record.taskKey=found->taskKey;*found=std::move(record);}
    }
    const auto state=captureDesktopState(app);
    nlohmann::json signature{{"recent",state.recent},{"decoder",state.decoder},{"brightness",state.brightness},{"volume",state.volume},
        {"rotation",state.rotation},{"zoom",state.zoom},{"filter",state.filter},{"networkMode",state.networkMode},{"showInfo",state.showInfo},{"host",state.host},{"port",state.port},{"downloads",nlohmann::json::array()}};
    for(const auto& record:state.downloads)signature["downloads"].push_back({record.source,record.total,record.downloaded,record.state,record.requiresSource,record.taskKey});
    const auto value=signature.dump();if(value==app.persistedSignature&&!force)return;
    if(saveDesktopState(dataRoot(),state,app.stateError))app.persistedSignature=value;
}
void openDataFolder(DesktopApp& app,const char* child) {
    try {const auto path=std::filesystem::u8path(dataRoot())/child;std::filesystem::create_directories(path);
        if(reinterpret_cast<INT_PTR>(ShellExecuteW(app.window,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)app.stateError="无法打开数据目录。";
    }catch(const std::exception&){app.stateError="无法创建或打开数据目录，请检查权限。";}
}
void beginCacheCleanup(DesktopApp& app,const std::string& source,const std::string& taskKey={}) {
    if(app.cacheCleanup.valid()||!app.cleanupSource.empty())return;
    app.cleanupSource=source;app.cleanupTaskKey=taskKey;app.cleanupNotice="正在停止任务并清理缓存…";app.cacheAutoPlay=false;app.player.stop();app.cache.cancel();
}
const char* cacheStatus(CacheState state) {
    switch(state) {
        case CacheState::Preparing:return "准备下载";case CacheState::Downloading:return "下载中";
        case CacheState::Paused:return "已暂停";case CacheState::Complete:return "已完成";
        case CacheState::Cancelled:return "已取消，进度已保存";case CacheState::Error:return "下载失败";
        default:return "暂无下载";
    }
}
const char* uploadStatus(UploadState state) {
    switch(state) {
        case UploadState::Hashing:return "准备文件";case UploadState::Uploading:return "上传中";
        case UploadState::Paused:return "已暂停";case UploadState::Reconnecting:return "等待重连";
        case UploadState::Completed:return "已完成";case UploadState::Canceled:return "已取消";
        case UploadState::Failed:return "上传失败";default:return "暂无上传";
    }
}
void startCache(DesktopApp& app) {
    if(!app.networkUrl[0]||!app.cleanupSource.empty()||app.cacheCleanup.valid())return;
    app.removedCacheSource.clear();
    app.player.stop();app.cacheAutoPlay=true;app.cachePlayWhenComplete=app.networkMode==2;
    app.cache.start(app.networkUrl,(std::filesystem::u8path(dataRoot())/"cache").u8string());
}
void networkPage(DesktopApp& app,float scale) {
    ImGui::BeginChild("network",{0,0});ImGui::Text("网络播放");
    subtitle("输入点播地址，选择播放方式");ImGui::Separator();
    ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##network-url","https://.../video.flv 或 .../index.m3u8",app.networkUrl,sizeof(app.networkUrl));
    const char* modes[]{"直接网络播放","边下边播并保存缓存","下载完成后播放"};
    ImGui::SetNextItemWidth(360*scale);ImGui::Combo("播放方式",&app.networkMode,modes,3);
    if(app.demo) {
        if(ImGui::Button("本地 FLV 示例"))snprintf(app.networkUrl,sizeof(app.networkUrl),"http://127.0.0.1:%u/stream.flv",app.demo->port());
        ImGui::SameLine();if(ImGui::Button("本地 HLS 示例"))snprintf(app.networkUrl,sizeof(app.networkUrl),"http://127.0.0.1:%u/hls/index.m3u8",app.demo->port());
    }
    if(accentButton("开始播放",{140*scale,40*scale})) {
        if(app.networkMode==0){app.cacheAutoPlay=false;openSource(app,app.networkUrl);}
        else startCache(app);
    }
    ImGui::Spacing();
    ImGui::TextWrapped(app.networkMode==0?"支持 FFmpeg 可读取的网络地址，包括 HLS、HTTP 和 RTMP。":
        "支持 HTTP/HTTPS FLV 和 MPEG-TS HLS 点播。暂停或取消后保留缓存，再次打开相同地址可继续下载。");
    const auto state=app.cache.snapshot();
    if(state.state!=CacheState::Idle){ImGui::Spacing();ImGui::Separator();ImGui::Text("%s",cacheStatus(state.state));
        ImGui::ProgressBar(state.total?float(double(state.downloaded)/state.total):0,{-1,0});
        ImGui::Text("%.2f / %.2f MB",state.downloaded/1048576.,state.total/1048576.);
        if(ImGui::Button("查看传输中心"))app.page=5;
        if(!state.error.empty())ImGui::TextWrapped("%s",state.error.c_str());}
    ImGui::EndChild();
}
void startLocalServer(DesktopApp& app) {
    if(app.serverStart.valid() || app.localServer)return;
    const auto root=(std::filesystem::u8path(dataRoot())/"server").u8string();const auto port=std::uint16_t(app.port);
    app.serverError.clear();app.serverStopping=false;
    app.serverStart=std::async(std::launch::async,[root,port]{auto server=std::make_shared<LocalServer>(root,port);
        return server->start()?server:std::shared_ptr<LocalServer>{};});
}
void accountPage(DesktopApp& app,float scale) {
    ImGui::BeginChild("account",{0,0});ImGui::Text("账号与连接");subtitle("本地演示服务 · 注册、登录与文件传输");ImGui::Separator();
    const auto state=app.service.snapshot();
    const char* status=state.state==ServiceState::Connected?"已连接":state.state==ServiceState::Connecting?"正在连接":state.state==ServiceState::Reconnecting?"正在重连":"未连接";
    ImGui::TextColored(state.connected?kAccent:kMuted,"%s",status);
    ImGui::Text("重连次数：%llu",static_cast<unsigned long long>(state.reconnectAttempts));
    ImGui::SetNextItemWidth(280*scale);ImGui::InputText("服务地址",app.host,sizeof(app.host));
    ImGui::SetNextItemWidth(160*scale);ImGui::InputInt("端口",&app.port);app.port=std::clamp(app.port,1,65535);
    ImGui::BeginDisabled(app.serverStart.valid());
    if(ImGui::Button(app.localServer?"停止本地服务":"启动本地服务")) {
        if(app.localServer){app.serverStopping=true;auto server=std::move(app.localServer);app.serverStart=std::async(std::launch::async,[server]{server->stop();return std::shared_ptr<LocalServer>{};});}
        else startLocalServer(app);
    }
    ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("连接"))app.service.connect(app.host,std::uint16_t(app.port));ImGui::SameLine();
    if(ImGui::Button("断开"))app.service.disconnect();
    if(!app.serverError.empty())ImGui::TextWrapped("%s",app.serverError.c_str());
    ImGui::Spacing();ImGui::Separator();
    if(state.authenticated){ImGui::Text("已登录：%s",state.username.c_str());if(ImGui::Button("退出账号"))app.service.logout();}
    else {
        ImGui::SetNextItemWidth(320*scale);ImGui::InputText("用户名",app.username,sizeof(app.username));
        ImGui::SetNextItemWidth(320*scale);ImGui::InputText("密码",app.password,sizeof(app.password),ImGuiInputTextFlags_Password);
        ImGui::BeginDisabled(!state.connected||state.busy);
        if(accentButton("登录")){app.service.login(app.username,app.password);SecureZeroMemory(app.password,sizeof(app.password));}
        ImGui::SameLine();if(ImGui::Button("注册账号")){app.service.registerAccount(app.username,app.password);SecureZeroMemory(app.password,sizeof(app.password));}
        ImGui::EndDisabled();
    }
    if(state.busy)subtitle("正在处理请求…");
    if(!state.error.empty())ImGui::TextWrapped("%s",state.error.c_str());
    if(!state.code.empty())subtitle(state.code.c_str());
    ImGui::EndChild();
}
void pickUpload(DesktopApp& app) {
    wchar_t path[32768]{};OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner=app.window;dialog.lpstrFile=path;dialog.nMaxFile=32768;
    dialog.lpstrTitle=L"选择上传文件";dialog.lpstrFilter=L"所有文件\0*.*\0";
    dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER;
    if(GetOpenFileNameW(&dialog))app.service.upload(utf8(path));
}
void transferPage(DesktopApp& app,float scale) {
    ImGui::BeginChild("transfers",{0,0});ImGui::Text("传输中心");subtitle("下载缓存与分片上传");ImGui::Separator();
    auto cache=app.cache.snapshot();if(cache.source==app.removedCacheSource)cache={};ImGui::Text("下载 · %s",cacheStatus(cache.state));
    if(!cache.source.empty()){ImGui::TextWrapped("%s",cache.source.c_str());
        ImGui::ProgressBar(cache.total?float(double(cache.downloaded)/cache.total):0,{-1,24*scale});
        ImGui::Text("%.2f / %.2f MB · %.2f MB/s",cache.downloaded/1048576.,cache.total/1048576.,cache.bytesPerSecond/1048576.);
        if(cache.segmentsTotal)ImGui::Text("分片：%zu / %zu",cache.segmentsComplete,cache.segmentsTotal);
        if(cache.waitingForData)subtitle("正在获取播放所需的数据");
        if(cache.state==CacheState::Downloading){if(ImGui::Button("暂停下载"))app.cache.pause();ImGui::SameLine();}
        if(cache.state==CacheState::Paused){if(ImGui::Button("继续下载"))app.cache.resume();ImGui::SameLine();}
        if(cache.state==CacheState::Paused||cache.state==CacheState::Downloading||cache.state==CacheState::Preparing){
            if(ImGui::Button("取消并保存进度")){app.cacheAutoPlay=false;app.player.stop();app.cache.cancel();}}
        if(cache.state==CacheState::Cancelled||cache.state==CacheState::Error){if(ImGui::Button("恢复此下载")){
            strncpy_s(app.networkUrl,cache.source.c_str(),_TRUNCATE);startCache(app);}}
        if(cache.state==CacheState::Complete && ImGui::Button("播放缓存"))openSource(app,cache.playbackUrl);
        if(!cache.error.empty())ImGui::TextWrapped("%s",cache.error.c_str());
    }else {subtitle("从网络播放页添加下载。");if(ImGui::Button("添加网络点播"))app.page=3;}
    ImGui::Spacing();
    if(ImGui::Button("打开缓存目录"))openDataFolder(app,"cache");ImGui::SameLine();
    if(ImGui::Button("打开诊断目录"))openDataFolder(app,"");
    if(!app.cleanupNotice.empty())ImGui::TextWrapped("%s",app.cleanupNotice.c_str());
    if(!app.downloads.empty()&&ImGui::CollapsingHeader("下载历史与恢复")) {
        const auto history=app.downloads;
        for(std::size_t i=0;i<history.size();++i){const auto& record=history[i];ImGui::PushID(static_cast<int>(i));
            ImGui::TextWrapped("%s",record.source.c_str());ImGui::Text("%.2f / %.2f MB",record.downloaded/1048576.,record.total/1048576.);
            ImGui::BeginDisabled(record.requiresSource||!app.cleanupSource.empty()||app.cacheCleanup.valid());
            if(ImGui::Button("恢复下载")){strncpy_s(app.networkUrl,record.source.c_str(),_TRUNCATE);startCache(app);}
            ImGui::EndDisabled();
            ImGui::SameLine();ImGui::BeginDisabled((record.requiresSource&&record.taskKey.empty())||!app.cleanupSource.empty()||app.cacheCleanup.valid());
            if(ImGui::Button("清理缓存")){app.cleanupNotice="";app.cleanupConfirmSource=record.source;app.cleanupConfirmKey=record.taskKey;}ImGui::EndDisabled();
            if(record.requiresSource)subtitle("此地址含临时访问参数，请在网络页重新输入完整地址。");
            ImGui::Separator();ImGui::PopID();
        }
    }
    if(!app.cleanupConfirmSource.empty())ImGui::OpenPopup("清理此任务");
    bool confirmOpen=true;
    if(ImGui::BeginPopupModal("清理此任务",&confirmOpen,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("将停止当前播放与下载，删除所选任务缓存。恢复时需要重新下载。");
        ImGui::TextWrapped("%s",app.cleanupConfirmSource.c_str());
        if(ImGui::Button("删除缓存")){const auto source=app.cleanupConfirmSource,key=app.cleanupConfirmKey;app.cleanupConfirmSource.clear();app.cleanupConfirmKey.clear();beginCacheCleanup(app,source,key);ImGui::CloseCurrentPopup();}
        ImGui::SameLine();if(ImGui::Button("保留")){app.cleanupConfirmSource.clear();ImGui::CloseCurrentPopup();}ImGui::EndPopup();
    }
    if(!confirmOpen)app.cleanupConfirmSource.clear();
    ImGui::Spacing();ImGui::Separator();const auto upload=app.service.uploadSnapshot();
    ImGui::Text("上传 · %s",uploadStatus(upload.state));
    const auto service=app.service.snapshot();ImGui::BeginDisabled(!service.authenticated);
    if(accentButton("选择上传文件"))pickUpload(app);ImGui::EndDisabled();
    if(!service.authenticated)subtitle("请先连接服务并登录账号。");
    if(!upload.localPath.empty()) {
        ImGui::TextWrapped("%s",upload.filename.c_str());ImGui::ProgressBar(float(upload.progress),{-1,24*scale});
        ImGui::Text("%.2f / %.2f MB · %.2f MB/s",upload.uploadedBytes/1048576.,upload.totalBytes/1048576.,upload.bytesPerSecond/1048576.);
        if(upload.state==UploadState::Uploading){if(ImGui::Button("暂停上传"))app.service.pauseUpload();ImGui::SameLine();}
        if(upload.state==UploadState::Paused||upload.state==UploadState::Canceled||upload.state==UploadState::Failed){
            ImGui::BeginDisabled(!service.authenticated);
            if(ImGui::Button("继续上传")){if(upload.state==UploadState::Paused)app.service.resumeUpload();else app.service.upload(upload.localPath);}
            ImGui::EndDisabled();ImGui::SameLine();}
        if(upload.state==UploadState::Uploading||upload.state==UploadState::Paused||upload.state==UploadState::Hashing||upload.state==UploadState::Reconnecting){if(ImGui::Button("取消上传"))app.service.cancelUpload();}
        if(!upload.error.empty())ImGui::TextWrapped("%s",upload.error.c_str());
        if(!upload.serverPath.empty()){subtitle("文件已保存到服务端。");if(app.showInfo)ImGui::TextWrapped("%s",upload.serverPath.c_str());}
    }
    ImGui::EndChild();
}
void pollBackground(DesktopApp& app) {
    if(!app.cleanupSource.empty()&&!app.cacheCleanup.valid()&&app.player.snapshot().state==PlaybackState::Idle) {
        const auto root=dataRoot(),source=app.cleanupSource,key=app.cleanupTaskKey;
        app.cacheCleanup=std::async(std::launch::async,[root,source,key]{std::string error;
            for(int attempt=0;attempt<30;++attempt){if(key.empty()?removeDesktopCache(root,source,error):removeDesktopCacheByKey(root,key,error))return std::make_pair(true,std::string());std::this_thread::sleep_for(std::chrono::milliseconds(100));}
            return std::make_pair(false,error);});
    }
    if(app.cacheCleanup.valid()&&app.cacheCleanup.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        const auto result=app.cacheCleanup.get();
        if(result.first){app.removedCacheSource=app.cleanupSource;app.downloads.erase(std::remove_if(app.downloads.begin(),app.downloads.end(),[&](const DownloadRecord& r){return app.cleanupTaskKey.empty()?r.source==app.cleanupSource:r.taskKey==app.cleanupTaskKey;}),app.downloads.end());app.cleanupNotice="缓存已清理。";}
        else app.cleanupNotice="缓存清理失败："+result.second;
        app.cleanupSource.clear();app.cleanupTaskKey.clear();persistDesktopState(app,true);
    }
    if(app.serverStart.valid()&&app.serverStart.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try {app.localServer=app.serverStart.get();if(app.localServer) {
            const auto connection=app.service.snapshot();
            if(connection.state==ServiceState::Disconnected || connection.host!="127.0.0.1" || connection.port!=app.localServer->port())
                app.service.connect("127.0.0.1",app.localServer->port());
        }
            else if(!app.serverStopping)app.serverError="本地服务启动失败，请检查端口占用或数据目录。";}
        catch(const std::exception& e){app.serverError=e.what();}
    }
    const auto status=app.cache.status();const auto& cache=status.second;const auto generation=status.first;
    if(app.cacheAutoPlay && generation!=app.openedCacheGeneration && !cache.playbackUrl.empty() &&
        (!app.cachePlayWhenComplete || cache.state==CacheState::Complete)) {
        app.openedCacheGeneration=generation;openSource(app,cache.playbackUrl);
    }
    const auto playback=app.player.snapshot();const auto connection=app.service.snapshot();const auto upload=app.service.uploadSnapshot();
    const auto logSignature=std::to_string(static_cast<int>(playback.state))+":"+playback.error+":"+std::to_string(static_cast<int>(cache.state))+":"+cache.error+":"+
        std::to_string(static_cast<int>(connection.state))+":"+connection.code+":"+connection.error+":"+std::to_string(static_cast<int>(upload.state))+":"+upload.code+":"+upload.error;
    if(logSignature!=app.lastStatusSignature){app.lastStatusSignature=logSignature;appendDesktopLog(dataRoot(),"state",logSignature,app.logError);}
    persistDesktopState(app);
}
void draw(DesktopApp& app, const PlaybackSnapshot& state, const std::shared_ptr<const PlaybackFrame>& frame, float scale) {
    ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Native Player",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBringToFrontOnFocus);
    sidebar(app,scale); ImGui::SameLine(); ImGui::BeginGroup();
    ImGui::BeginChild("header",{0,77*scale});
    ImGui::Text("%s",state.source.empty() ? "播放器" : basename(state.source).c_str());
    ImGui::TextColored(state.state == PlaybackState::Error ? ImVec4{1,.52f,.48f,1} : kAccent,"%s",statusText(state.state));
    ImGui::SameLine(); subtitle(state.width > 0 ? (std::to_string(state.width)+" x "+std::to_string(state.height)+"  /  "+backendText(state.backend)).c_str() : "本地播放 · 独立窗口");
    ImGui::EndChild();
    if(!app.stateError.empty())ImGui::TextWrapped("设置保存：%s",app.stateError.c_str());
    if(!app.stateLoadError.empty()){ImGui::TextWrapped("设置读取：%s",app.stateLoadError.c_str());ImGui::SameLine();if(ImGui::SmallButton("知道了"))app.stateLoadError.clear();}
    if(!app.logError.empty())ImGui::TextWrapped("诊断记录：%s",app.logError.c_str());
    if(app.page == 1) recent(app,scale);
    else if(app.page == 2) settings(app,scale);
    else if(app.page == 3) networkPage(app,scale);
    else if(app.page == 4) accountPage(app,scale);
    else if(app.page == 5) transferPage(app,scale);
    else { videoPanel(app,state,frame,scale); controls(app,state,scale); }
    ImGui::EndGroup(); ImGui::End();
    if(app.showInfo) {
        ImGui::SetNextWindowSize({370*scale,240*scale},ImGuiCond_FirstUseEver);
        ImGui::Begin("播放技术信息",&app.showInfo);
        ImGui::Text("状态：%s",statusText(state.state)); ImGui::Text("解码器：%s",backendText(state.backend));
        ImGui::Text("画面：%d x %d",state.width,state.height);
        ImGui::Text("播放位置：%.3f 秒",state.position); ImGui::Text("同步差值：%.1f ms",state.syncDifference*1000);
        ImGui::Text("丢弃迟到帧：%llu",static_cast<unsigned long long>(state.droppedFrames));
        ImGui::Text("渲染帧率：%.1f FPS",ImGui::GetIO().Framerate); ImGui::End();
    }
    shortcuts(app,state);
}
}
int runDesktopPlayer(int argc,char** argv) {
    ImGui_ImplWin32_EnableDpiAwareness();
    DesktopApp app;
    for(int i=1;i<argc;++i){
        if(std::strcmp(argv[i],"--data-root")==0&&i+1<argc)overrideDataRoot=argv[++i];
        else if(std::strcmp(argv[i],"--state-save-smoke")==0)app.stateSaveSmoke=true;
        else if(std::strcmp(argv[i],"--state-load-smoke")==0)app.stateLoadSmoke=true;
    }
    for(int i=1;i+1<argc;++i)if(std::strcmp(argv[i],"--second-layer-smoke")==0)overrideDataRoot=(std::filesystem::absolute(std::filesystem::u8path(argv[++i]))/"data").u8string();
    const auto saved=loadDesktopState(dataRoot(),app.stateLoadError);app.recent=saved.recent;app.downloads=saved.downloads;
    app.decoder=saved.decoder==1?VideoDecoderBackend::Ffmpeg:saved.decoder==2?VideoDecoderBackend::MediaFoundation:VideoDecoderBackend::Auto;
    app.brightness=saved.brightness;app.effects.rotation=saved.rotation;app.effects.zoom=saved.zoom;app.effects.filter=static_cast<VideoFilter>(saved.filter);
    app.showInfo=saved.showInfo;app.networkMode=saved.networkMode;app.port=saved.port;strncpy_s(app.host,saved.host.c_str(),_TRUNCATE);app.player.setVolume(saved.volume);
    if(app.stateLoadSmoke&&(std::abs(saved.brightness-1.25f)>.001f||std::abs(saved.zoom-1.5f)>.001f||saved.rotation!=90||saved.filter!=1||std::abs(saved.volume-.4f)>.001f||saved.networkMode!=2||saved.recent.empty()))return 6;
    for(int i=1;i+1<argc;++i)if(std::strcmp(argv[i],"--renderer")==0)app.renderer.opengl=std::strcmp(argv[++i],"d3d11")!=0;
    WNDCLASSEXW wc{sizeof(wc)}; wc.style=CS_CLASSDC; wc.lpfnWndProc=windowProc;
    wc.hInstance=GetModuleHandleW(nullptr); wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.lpszClassName=L"NativePlayerDesktop";
    if(!RegisterClassExW(&wc)) return 1;
    const HWND window=CreateWindowExW(0,wc.lpszClassName,L"Native Player — 本地媒体工作台",WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT,CW_USEDEFAULT,1240,820,nullptr,nullptr,wc.hInstance,&app);
    if(!window) { UnregisterClassW(wc.lpszClassName,wc.hInstance); return 1; }
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    auto& io=ImGui::GetIO(); io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; io.IniFilename=nullptr;
    const float scale=ImGui_ImplWin32_GetDpiScaleForHwnd(window);
    theme(scale);
    wchar_t windows[MAX_PATH]{}; GetWindowsDirectoryW(windows,MAX_PATH);
    const auto font=(std::filesystem::path(windows)/"Fonts"/"msyh.ttc").u8string();
    io.Fonts->TexDesiredWidth=4096;
    io.Fonts->AddFontFromFileTTF(font.c_str(),18*scale,nullptr,io.Fonts->GetGlyphRangesChineseFull());
    if(io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    const bool platform=ImGui_ImplWin32_Init(window);
    if(!platform || !app.renderer.initialize(window)) {
        MessageBoxW(window,L"无法初始化图形后端，可尝试 --renderer d3d11。",L"播放器启动失败",MB_ICONERROR);
        app.renderer.shutdown(); if(platform) ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); DestroyWindow(window); return 2;
    }
    DragAcceptFiles(window,TRUE);
    HANDLE parent=nullptr;
    std::string smokeDirectory;
    std::string secondSmoke, demoFixtures,historySmoke,historySource,historyAction;
    for(int i=1;i<argc;++i) {
        if(std::strcmp(argv[i],"--decoder")==0 && i+1<argc) { const auto value=std::string(argv[++i]);
          app.decoder=value == "ffmpeg" ? VideoDecoderBackend::Ffmpeg : value == "mf" ? VideoDecoderBackend::MediaFoundation : VideoDecoderBackend::Auto; }
        else if(std::strcmp(argv[i],"--parent-pid")==0 && i+1<argc) parent=OpenProcess(SYNCHRONIZE,FALSE,static_cast<DWORD>(std::strtoul(argv[++i],nullptr,10)));
        else if(std::strcmp(argv[i],"--smoke-test")==0 && i+1<argc) smokeDirectory=argv[++i];
        else if(std::strcmp(argv[i],"--second-layer-smoke")==0 && i+1<argc) secondSmoke=argv[++i];
        else if(std::strcmp(argv[i],"--demo-fixtures")==0 && i+1<argc) demoFixtures=argv[++i];
        else if(std::strcmp(argv[i],"--history-smoke")==0 && i+1<argc) historySmoke=argv[++i];
        else if(std::strcmp(argv[i],"--history-source")==0 && i+1<argc) historySource=argv[++i];
        else if(std::strcmp(argv[i],"--history-action")==0 && i+1<argc) historyAction=argv[++i];
        else if(std::strcmp(argv[i],"--data-root")==0 && i+1<argc) overrideDataRoot=argv[++i];
    }
    if(!demoFixtures.empty()) {
        app.demo=std::make_shared<DemoHttpServer>(std::filesystem::u8path(demoFixtures));
        if(app.demo->start())snprintf(app.networkUrl,sizeof(app.networkUrl),"http://127.0.0.1:%u/stream.flv",app.demo->port());
        else app.serverError="演示视频服务启动失败。";
        app.page=3;
    }
    for(int i=1;i<argc-1;++i) if(std::strcmp(argv[i],"--source")==0) { openSource(app,argv[i+1]); break; }
    ShowWindow(window,SW_SHOW); UpdateWindow(window);
    SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_SHOWWINDOW);
    const ULONGLONG smokeStart=GetTickCount64();
    ULONGLONG stageStart=smokeStart;
    int smokeStage=0, smokeResult=0;
    std::uint64_t smokePreviewSerial=0;
    int secondStage=0;
    ULONGLONG secondStageStart=smokeStart;
    std::uint16_t smokeServicePort=0;
    auto secondCapture=[&](const char* name) {
        const auto path=std::filesystem::u8path(secondSmoke)/(std::string(name)+".png");
        if(!app.renderer.saveScreenshot(path.c_str()))smokeResult=1;
    };
    if(!secondSmoke.empty()) {
        std::filesystem::create_directories(std::filesystem::u8path(secondSmoke));
        overrideDataRoot=(std::filesystem::absolute(std::filesystem::u8path(secondSmoke))/"data").u8string();
        app.port=0;app.page=3;app.networkMode=1;app.effects={};app.brightness=1;
    }
    if(app.stateSaveSmoke){app.brightness=1.25f;app.effects.rotation=90;app.effects.zoom=1.5f;app.effects.filter=VideoFilter::Grayscale;app.player.setVolume(.4f);app.networkMode=2;}
    if(!smokeDirectory.empty()) std::filesystem::create_directories(std::filesystem::u8path(smokeDirectory));
    int historyStage=0;std::uint64_t restoredBytes=0;std::string historyTaskKey;
    if(!historySmoke.empty()) {
        std::filesystem::create_directories(std::filesystem::u8path(historySmoke));app.page=5;app.networkMode=1;
        const auto record=std::find_if(app.downloads.begin(),app.downloads.end(),[&](const DownloadRecord& r){return r.source==historySource;});
        if(historySource.empty()||(historyAction!="seed"&&historyAction!="resume")||(historyAction=="resume"&&(record==app.downloads.end()||record->requiresSource||!record->downloaded)))smokeResult=1;
        if(record!=app.downloads.end()){restoredBytes=record->downloaded;historyTaskKey=record->taskKey;}
        strncpy_s(app.networkUrl,historySource.c_str(),_TRUNCATE);
        if(!smokeResult)startCache(app);
    }
    auto smokeCapture=[&](const char* name) {
        const auto path=std::filesystem::u8path(smokeDirectory)/(std::string(name)+".png");
        if(!app.renderer.saveScreenshot(path.c_str())) smokeResult=1;
    };
    while(!app.quitting) {
        MSG message{};
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { if(message.message == WM_QUIT) app.quitting=true; TranslateMessage(&message); DispatchMessageW(&message); }
        if(app.quitting || (parent && WaitForSingleObject(parent,0)==WAIT_OBJECT_0)) break;
        pollBackground(app);
        if(app.minimized) { Sleep(20); continue; }
        const auto renderedState=app.player.snapshot();
        const auto renderedFrame=app.player.frame();
        app.effects.brightness=app.brightness;
        if(!app.renderer.updateVideo(renderedFrame,app.effects)) { app.renderError="视频纹理上传失败，请停止后重新打开。"; app.player.stop(); }
        app.renderer.newFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        draw(app,renderedState,renderedFrame,scale); ImGui::Render(); app.renderer.begin(); app.renderer.present();
        if(!historySmoke.empty()) {
            const auto cache=app.cache.snapshot();
            auto report=[&](bool passed,bool cleaned){std::ofstream(std::filesystem::u8path(historySmoke)/"result.json")<<nlohmann::json{{"passed",passed},{"stage",historyStage},{"downloaded",cache.downloaded},{"total",cache.total},{"restoredBytes",restoredBytes},{"cleaned",cleaned},{"cleanup",app.cleanupNotice},{"cacheError",cache.error}}.dump(2);};
            if(historyStage==0&&renderedState.state==PlaybackState::Playing&&renderedFrame&&cache.downloaded>0&&cache.downloaded<cache.total) {
                app.renderer.saveScreenshot((std::filesystem::u8path(historySmoke)/"playing.png").c_str());
                if(historyAction=="seed"){app.cacheAutoPlay=false;app.player.stop();app.cache.cancel();}
                else beginCacheCleanup(app,historySource,historyTaskKey);
                historyStage=1;
            }else if(historyStage==1&&historyAction=="seed"&&cache.state==CacheState::Cancelled&&cache.downloaded>0&&cache.downloaded<cache.total&&renderedState.state==PlaybackState::Idle) {
                persistDesktopState(app,true);report(true,false);app.quitting=true;
            }else if(historyStage==1&&historyAction=="resume"&&app.cleanupSource.empty()&&!app.cacheCleanup.valid()) {
                const bool clean=app.removedCacheSource==historySource&&app.player.snapshot().state==PlaybackState::Idle;
                smokeResult=clean?0:1;report(clean,clean);app.quitting=true;
            }
            if(smokeResult||cache.state==CacheState::Error||GetTickCount64()-smokeStart>30000){smokeResult=1;report(false,false);app.quitting=true;}
        }
        if(!secondSmoke.empty()) {
            const auto cache=app.cache.snapshot();const auto service=app.service.snapshot();const auto upload=app.service.uploadSnapshot();
            auto nextStage=[&]{++secondStage;secondStageStart=GetTickCount64();};
            if(secondStage==0){secondCapture("network");startCache(app);nextStage();}
            else if(secondStage==1 && renderedState.state==PlaybackState::Playing && renderedFrame && cache.downloaded<cache.total) {
                secondCapture("progressive-playing");app.player.pause(true);app.cache.pause();app.page=5;nextStage();
            } else if(secondStage==2 && cache.state==CacheState::Paused && renderedState.state==PlaybackState::Paused) {
                secondCapture("download-paused");app.cacheAutoPlay=false;app.player.stop();app.cache.cancel();nextStage();
            } else if(secondStage==3 && cache.state==CacheState::Cancelled && renderedState.state==PlaybackState::Idle) {
                secondCapture("download-cancelled");startCache(app);nextStage();
            } else if(secondStage==4 && renderedState.state==PlaybackState::Playing && renderedFrame) {
                secondCapture("download-resumed");app.player.pause(true);app.page=4;startLocalServer(app);nextStage();
            } else if(secondStage==5 && service.connected && app.localServer) {
                smokeServicePort=app.localServer->port();app.port=smokeServicePort;
                snprintf(app.username,sizeof(app.username),"demo_%lu",GetCurrentProcessId());
                app.service.registerAccount(app.username,"Demo-password-123!");nextStage();
            } else if(secondStage==6 && !service.busy && service.code=="OK") {
                app.service.login(app.username,"Demo-password-123!");nextStage();
            } else if(secondStage==7 && service.authenticated && GetTickCount64()-secondStageStart>150) {
                secondCapture("account");app.page=5;app.service.upload((std::filesystem::u8path(demoFixtures)/"stream.flv").u8string());nextStage();
            } else if(secondStage==8 && upload.state==UploadState::Uploading && upload.uploadedBytes>0 && upload.uploadedBytes<upload.totalBytes) {
                app.service.pauseUpload();nextStage();
            } else if(secondStage==9 && upload.state==UploadState::Paused) {
                secondCapture("upload-paused");app.service.resumeUpload();nextStage();
            } else if(secondStage==10 && upload.state==UploadState::Completed && cache.state==CacheState::Complete) {
                secondCapture("transfers-complete");app.page=4;
                app.serverStopping=true;auto server=std::move(app.localServer);
                app.serverStart=std::async(std::launch::async,[server]{server->stop();return std::shared_ptr<LocalServer>{};});nextStage();
            } else if(secondStage==11 && !service.connected && !app.serverStart.valid()) {
                secondCapture("reconnecting");app.port=smokeServicePort;startLocalServer(app);nextStage();
            } else if(secondStage==12 && service.connected && service.authenticated && GetTickCount64()-secondStageStart>150) {
                secondCapture("session-restored");app.page=3;app.networkMode=2;
                snprintf(app.networkUrl,sizeof(app.networkUrl),"http://127.0.0.1:%u/hls/index.m3u8",app.demo->port());
                startCache(app);nextStage();
            } else if(secondStage==13 && cache.state==CacheState::Downloading && cache.segmentsTotal==8 && renderedState.state==PlaybackState::Idle) {
                secondCapture("hls-download-first");nextStage();
            } else if(secondStage==14 && cache.state==CacheState::Complete && renderedState.state==PlaybackState::Playing && renderedFrame && renderedState.source==cache.playbackUrl) {
                secondCapture("hls-complete-playing");
                std::ofstream(std::filesystem::u8path(secondSmoke)/"result.json")<<nlohmann::json{{"passed",smokeResult==0},{"stages",secondStage+1},{"cachedBytes",cache.downloaded},{"segments",cache.segmentsTotal},{"uploadedBytes",upload.uploadedBytes},{"authenticated",service.authenticated}}.dump(2);
                app.quitting=true;
            }
            if(GetTickCount64()-smokeStart>90000 || GetTickCount64()-secondStageStart>30000) {
                secondCapture("timeout");smokeResult=1;app.quitting=true;
                std::ofstream(std::filesystem::u8path(secondSmoke)/"result.json")<<nlohmann::json{{"passed",false},{"stage",secondStage},{"playback",statusText(renderedState.state)},{"cache",cacheStatus(cache.state)},{"cacheError",cache.error},{"serviceError",service.error},{"upload",uploadStatus(upload.state)}}.dump(2);
            }
        }
        if(!smokeDirectory.empty()) {
            // Evaluate the snapshot and frame actually drawn, rather than a
            // newer worker state whose texture has not reached this window yet.
            const auto& state=renderedState;
            if(smokeStage==0 && state.state==PlaybackState::Playing && state.position>=.3) {
                smokeCapture("playing"); app.player.pause(true); smokeStage=1; stageStart=GetTickCount64();
            } else if(smokeStage==1 && state.state==PlaybackState::Paused && GetTickCount64()-stageStart>100) {
                smokeCapture("paused"); smokePreviewSerial=renderedFrame ? renderedFrame->serial : 0;
                app.player.seek(std::min(2.0,state.duration*.5)); smokeStage=2; stageStart=GetTickCount64();
            } else if(smokeStage==2 && state.state==PlaybackState::Paused && renderedFrame && renderedFrame->serial>smokePreviewSerial) {
                smokeCapture("seek-preview"); app.player.stop(); smokeStage=3; stageStart=GetTickCount64();
            } else if(smokeStage==3 && state.state==PlaybackState::Idle && !renderedFrame) {
                smokeCapture("idle"); app.player.open("Z:/native-player-missing-test.mp4",VideoDecoderBackend::Ffmpeg); smokeStage=4; stageStart=GetTickCount64();
            } else if(smokeStage==4 && state.state==PlaybackState::Error) {
                smokeCapture("error");
                nlohmann::json report{{"passed",smokeResult==0},{"stages",{"playing","paused","seek-preview","idle","error"}},{"error",state.error},{"stateLoadDiagnostic",app.stateLoadError}};
                std::ofstream(std::filesystem::u8path(smokeDirectory)/"result.json")<<report.dump(2);
                app.quitting=true;
            }
            if(GetTickCount64()-smokeStart>20000) {
                smokeCapture("timeout"); smokeResult=1; app.quitting=true;
                std::ofstream(std::filesystem::u8path(smokeDirectory)/"result.json")<<nlohmann::json{{"passed",false},{"stage",smokeStage},{"state",statusText(state.state)}}.dump(2);
            }
        }
    }
    persistDesktopState(app,true);
    appendDesktopLog(dataRoot(),"exit","desktop closed",app.logError);
    app.player.stop(); if(parent) CloseHandle(parent);
    app.renderer.shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    DestroyWindow(window); UnregisterClassW(wc.lpszClassName,wc.hInstance); return smokeResult;
}
