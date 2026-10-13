#include "desktop-player.h"
#include "playback-session.h"
#include "render/desktop-renderer.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
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
#include <nlohmann/json.hpp>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace {
constexpr ImVec4 kAccent{.38f, .80f, .68f, 1};
constexpr ImVec4 kMuted{.48f, .54f, .63f, 1};
constexpr ImVec4 kText{.88f, .92f, .97f, 1};
struct DesktopApp {
    HWND window = nullptr;
    DesktopRenderer renderer;
    PlaybackSession player;
    std::vector<std::string> recent;
    VideoDecoderBackend decoder = VideoDecoderBackend::Auto;
    float brightness = 1;
    bool fullscreen = false, topmost = false, showInfo = false, quitting = false, minimized = false;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    int page = 0;
    double seekValue = 0;
    bool seeking = false;
    std::string renderError;
};
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
    app.recent.erase(std::remove(app.recent.begin(), app.recent.end(), path), app.recent.end());
    app.recent.insert(app.recent.begin(), path); if(app.recent.size() > 10) app.recent.resize(10);
    app.page = 0; app.seeking = false; app.renderError.clear(); app.player.open(path, app.decoder);
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
    c[ImGuiCol_CheckMark] = kAccent; c[ImGuiCol_SliderGrab] = kAccent;
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
    const bool active = state.state == PlaybackState::Playing || state.state == PlaybackState::Paused;
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
    const char* pages[]{"播放器", "最近打开", "播放设置"};
    for(int i=0;i<3;++i) if(ImGui::Selectable(pages[i], app.page == i, 0, {0,36*scale})) app.page=i;
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    if(accentButton("打开视频", {-1,38*scale})) pickFile(app);
    subtitle("Ctrl + O  选择文件");
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight()-145*scale));
    subtitle("快捷操作");
    ImGui::Text("空格      播放 / 暂停");
    ImGui::Text("左右键    跳转 10 秒");
    ImGui::Text("F11       全屏切换");
    subtitle("FFmpeg / D3D11");
    ImGui::EndChild();
}
void videoPanel(DesktopApp& app, const PlaybackSnapshot& state, const std::shared_ptr<const PlaybackFrame>& frame, float scale) {
    ImGui::BeginChild("video-panel", {0, -165*scale}, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 origin=ImGui::GetCursorScreenPos(), area=ImGui::GetContentRegionAvail();
    auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin,{origin.x+area.x,origin.y+area.y},IM_COL32(8,12,18,255),12*scale);
    if(frame && app.renderer.videoView()) {
        const float ratio=std::min(area.x/frame->width,area.y/frame->height);
        const ImVec2 size{frame->width*ratio,frame->height*ratio};
        ImGui::SetCursorScreenPos({origin.x+(area.x-size.x)/2,origin.y+(area.y-size.y)/2});
        ImGui::Image(reinterpret_cast<ImTextureID>(app.renderer.videoView()),size);
        if(state.state == PlaybackState::Paused || state.state == PlaybackState::Ended || state.state == PlaybackState::Error) {
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
    const bool active=state.state == PlaybackState::Playing || state.state == PlaybackState::Paused;
    if(!app.seeking) app.seekValue=state.position;
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
    if(ImGui::Checkbox("窗口置顶",&app.topmost)) SetWindowPos(app.window,app.topmost ? HWND_TOPMOST : HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
    ImGui::Checkbox("显示播放技术信息",&app.showInfo);
    ImGui::Spacing(); ImGui::Separator(); subtitle("第一层 / 本地播放器");
    ImGui::TextWrapped("Win32 + Dear ImGui 界面，复用 D3D11 渲染和 FFmpeg / Media Foundation 播放链路。");
    ImGui::TextWrapped("支持本地视频、拖放、暂停、跳转、音量、亮度和正常结束后重新播放。");
    ImGui::EndChild();
}
void recent(DesktopApp& app, float scale) {
    ImGui::BeginChild("recent",{0,0}); ImGui::Text("最近打开"); subtitle("本次会话打开的文件，点击可重新播放");
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
void draw(DesktopApp& app, const PlaybackSnapshot& state, const std::shared_ptr<const PlaybackFrame>& frame, float scale) {
    ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Native Player",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBringToFrontOnFocus);
    sidebar(app,scale); ImGui::SameLine(); ImGui::BeginGroup();
    ImGui::BeginChild("header",{0,77*scale});
    ImGui::Text("%s",state.source.empty() ? "播放器" : basename(state.source).c_str());
    ImGui::TextColored(state.state == PlaybackState::Error ? ImVec4{1,.52f,.48f,1} : kAccent,"%s",statusText(state.state));
    ImGui::SameLine(); subtitle(state.width > 0 ? (std::to_string(state.width)+" x "+std::to_string(state.height)+"  /  "+backendText(state.backend)).c_str() : "本地播放 · 独立窗口");
    ImGui::EndChild();
    if(app.page == 1) recent(app,scale);
    else if(app.page == 2) settings(app,scale);
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
        MessageBoxW(window,L"无法初始化 D3D11 或界面后端。",L"播放器启动失败",MB_ICONERROR);
        app.renderer.shutdown(); if(platform) ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); DestroyWindow(window); return 2;
    }
    DragAcceptFiles(window,TRUE);
    HANDLE parent=nullptr;
    std::string smokeDirectory;
    for(int i=1;i<argc;++i) {
        if(std::strcmp(argv[i],"--decoder")==0 && i+1<argc) { const auto value=std::string(argv[++i]);
          app.decoder=value == "ffmpeg" ? VideoDecoderBackend::Ffmpeg : value == "mf" ? VideoDecoderBackend::MediaFoundation : VideoDecoderBackend::Auto; }
        else if(std::strcmp(argv[i],"--parent-pid")==0 && i+1<argc) parent=OpenProcess(SYNCHRONIZE,FALSE,static_cast<DWORD>(std::strtoul(argv[++i],nullptr,10)));
        else if(std::strcmp(argv[i],"--smoke-test")==0 && i+1<argc) smokeDirectory=argv[++i];
    }
    for(int i=1;i<argc-1;++i) if(std::strcmp(argv[i],"--source")==0) { openSource(app,argv[i+1]); break; }
    ShowWindow(window,SW_SHOW); UpdateWindow(window);
    SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_SHOWWINDOW);
    const ULONGLONG smokeStart=GetTickCount64();
    ULONGLONG stageStart=smokeStart;
    int smokeStage=0, smokeResult=0;
    std::uint64_t smokePreviewSerial=0;
    if(!smokeDirectory.empty()) std::filesystem::create_directories(std::filesystem::u8path(smokeDirectory));
    auto smokeCapture=[&](const char* name) {
        const auto path=std::filesystem::u8path(smokeDirectory)/(std::string(name)+".png");
        if(!app.renderer.saveScreenshot(path.c_str())) smokeResult=1;
    };
    while(!app.quitting) {
        MSG message{};
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { if(message.message == WM_QUIT) app.quitting=true; TranslateMessage(&message); DispatchMessageW(&message); }
        if(app.quitting || (parent && WaitForSingleObject(parent,0)==WAIT_OBJECT_0)) break;
        if(app.minimized) { Sleep(20); continue; }
        const auto renderedState=app.player.snapshot();
        const auto renderedFrame=app.player.frame();
        if(!app.renderer.updateVideo(renderedFrame,app.brightness)) { app.renderError="视频纹理上传失败，请停止后重新打开。"; app.player.stop(); }
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        draw(app,renderedState,renderedFrame,scale); ImGui::Render(); app.renderer.begin(); app.renderer.present();
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
                nlohmann::json report{{"passed",smokeResult==0},{"stages",{"playing","paused","seek-preview","idle","error"}},{"error",state.error}};
                std::ofstream(std::filesystem::u8path(smokeDirectory)/"result.json")<<report.dump(2);
                app.quitting=true;
            }
            if(GetTickCount64()-smokeStart>20000) {
                smokeCapture("timeout"); smokeResult=1; app.quitting=true;
                std::ofstream(std::filesystem::u8path(smokeDirectory)/"result.json")<<nlohmann::json{{"passed",false},{"stage",smokeStage},{"state",statusText(state.state)}}.dump(2);
            }
        }
    }
    app.player.stop(); if(parent) CloseHandle(parent);
    app.renderer.shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    DestroyWindow(window); UnregisterClassW(wc.lpszClassName,wc.hInstance); return smokeResult;
}
