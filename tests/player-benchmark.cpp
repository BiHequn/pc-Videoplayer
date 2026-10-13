#include <windows.h>
#include <psapi.h>
#include <GL/gl.h>
#include "app/playback-session.h"
#include "render/opengl-renderer.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
double seconds(Clock::time_point from, Clock::time_point to) { return std::chrono::duration<double>(to-from).count(); }
std::string quote(const std::string& value) {
    std::ostringstream out; out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
        else out << c;
    }
    out << '"'; return out.str();
}
std::string utc() {
    SYSTEMTIME time{}; GetSystemTime(&time); char result[40]{};
    std::snprintf(result,sizeof(result),"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds);
    return result;
}
double processCpu() {
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE, "GetProcessTimes failed");
    auto ticks = [](FILETIME value) { return (static_cast<unsigned long long>(value.dwHighDateTime)<<32) | value.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) / 10000000.0;
}
PROCESS_MEMORY_COUNTERS_EX memory() {
    PROCESS_MEMORY_COUNTERS_EX data{}; data.cb = sizeof(data);
    require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&data), sizeof(data)) != FALSE,
        "GetProcessMemoryInfo failed");
    return data;
}
double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(),values.end());
    const auto index = static_cast<size_t>(std::ceil(fraction * values.size())) - 1;
    return values[std::min(index,values.size()-1)];
}
struct Source {
    int width = 0, height = 0, audioStreams = 0;
    double fps = 0, duration = 0;
    std::int64_t frames = 0;
    std::uintmax_t bytes = 0;
    std::string codec, pixelFormat, container;
};
Source probe(const fs::path& file) {
    AVFormatContext* input = nullptr;
    require(avformat_open_input(&input,file.u8string().c_str(),nullptr,nullptr) >= 0,"cannot probe source");
    struct Close { AVFormatContext*& context; ~Close() { avformat_close_input(&context); } } close{input};
    require(avformat_find_stream_info(input,nullptr) >= 0,"cannot read source streams");
    const int index = av_find_best_stream(input,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
    require(index >= 0,"source has no video");
    Source source; const auto* video = input->streams[index]; const auto* parameters = video->codecpar;
    source.width = parameters->width; source.height = parameters->height;
    source.codec = avcodec_get_name(parameters->codec_id);
    const char* pixel = av_get_pix_fmt_name(static_cast<AVPixelFormat>(parameters->format));
    source.pixelFormat = pixel ? pixel : "unknown";
    source.container = input->iformat->name;
    source.fps = av_q2d(av_guess_frame_rate(input,input->streams[index],nullptr));
    source.frames = video->nb_frames;
    source.duration = video->duration != AV_NOPTS_VALUE ? video->duration * av_q2d(video->time_base) :
        (input->duration != AV_NOPTS_VALUE ? input->duration / double(AV_TIME_BASE) : 0);
    source.bytes = fs::file_size(file);
    for (unsigned int i=0; i<input->nb_streams; ++i)
        if (input->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ++source.audioStreams;
    require(source.width>0 && source.height>0 && source.duration>0 && source.fps>0 && source.frames>0,
        "benchmark requires finite duration and known frame count");
    require(source.audioStreams==0,"benchmark requires video-only source to avoid audio-device contention");
    return source;
}
struct Window {
    HWND handle = nullptr; bool registered = false, context = false;
    static constexpr const wchar_t* name = L"NativePlayerBenchmarkWindow";
    Window(bool visible) {
        WNDCLASSW klass{}; klass.style = CS_OWNDC; klass.lpfnWndProc = DefWindowProcW;
        klass.hInstance = GetModuleHandleW(nullptr); klass.lpszClassName = name;
        require(RegisterClassW(&klass)!=0,"register benchmark window failed"); registered = true;
        RECT rect{0,0,1280,720}; AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
        handle = CreateWindowW(name,L"PlaybackSession OpenGL benchmark",WS_OVERLAPPEDWINDOW,
            0,0,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,klass.hInstance,nullptr);
        if (!handle) { UnregisterClassW(name,klass.hInstance); throw std::runtime_error("create benchmark window failed"); }
        if (visible) ShowWindow(handle,SW_SHOWNOACTIVATE);
        IMGUI_CHECKVERSION(); ImGui::CreateContext(); context = true; ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().LogFilename = nullptr;
    }
    ~Window() {
        if (context) ImGui::DestroyContext();
        if (handle) DestroyWindow(handle);
        if (registered) UnregisterClassW(name,GetModuleHandleW(nullptr));
    }
};
void pump() {
    MSG message{};
    while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
void draw(OpenGLRenderer& renderer) {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2(1280,720); ImGui::GetIO().DeltaTime = 1.f/30;
    ImGui::NewFrame();
    if (renderer.frameTexture()) ImGui::GetBackgroundDrawList()->AddImage(
        static_cast<ImTextureID>(renderer.frameTexture()),ImVec2(0,0),ImVec2(1280,720));
    ImGui::Render(); renderer.begin(); renderer.present();
    // Complete submitted GPU work: timing includes actual update, UI drawing,
    // SwapBuffers and completion. This is deliberately not a GPU timer query.
    glFinish(); require(glGetError()==GL_NO_ERROR,"OpenGL present/completion failed");
}
struct FrameSample { std::uint64_t serial; double elapsed, sync, render, present; };
struct MemorySample { double elapsed; SIZE_T rss, privateBytes; };
struct Result {
    Source source;
    std::string started, ended, error, vendor, gpu, glVersion, observedFormat;
    double elapsed=0, cpu=0, firstFrame=0, playbackFps=0;
    DWORD cores=0; int swapInterval=-999; bool swapDisabled=false;
    std::uint64_t delivered=0, published=0, dropped=0, observerMissed=0;
    SIZE_T peakRss=0, peakCommit=0, peakSampledPrivate=0;
    std::vector<FrameSample> samples;
    std::vector<MemorySample> memorySamples;
};
Result measure(const fs::path& file, bool native, bool visible, double timeout, const fs::path& screenshot) {
    Result result; result.source = probe(file);
    result.cores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    require(result.cores>0,"logical processor query failed");
    Window window(visible); OpenGLRenderer renderer;
    require(renderer.initialize(window.handle),"real OpenGLRenderer initialization failed");
    auto glString = [](GLenum key) { const auto* value = glGetString(key); return value ? reinterpret_cast<const char*>(value) : "unknown"; };
    result.vendor=glString(GL_VENDOR); result.gpu=glString(GL_RENDERER); result.glVersion=glString(GL_VERSION);
    using SetSwap = BOOL (WINAPI*)(int); using GetSwap = int (WINAPI*)();
    auto setSwap = reinterpret_cast<SetSwap>(wglGetProcAddress("wglSwapIntervalEXT"));
    auto getSwap = reinterpret_cast<GetSwap>(wglGetProcAddress("wglGetSwapIntervalEXT"));
    if (setSwap) result.swapDisabled = setSwap(0)!=FALSE;
    if (getSwap) result.swapInterval = getSwap();
    draw(renderer); // Build ImGui font resources outside the playback interval.
    result.samples.reserve(static_cast<size_t>(result.source.frames));
    result.memorySamples.reserve(static_cast<size_t>(result.source.duration*100+100));
    result.started=utc(); const double cpuStart=processCpu(); const auto start=Clock::now();
    auto nextMemory=start;
    {
        // A fresh session in a fresh process for every run; construction, open,
        // decode, copy, render, wait and terminal resource cleanup are included.
        PlaybackSession session; session.open(file.u8string(),VideoDecoderBackend::Ffmpeg,native);
        try {
            for (;;) {
                pump();
                const auto now=Clock::now();
                if (seconds(start,now)>timeout) throw std::runtime_error("playback timeout");
                const auto frame=session.frame();
                const bool newFrame=frame && frame->serial!=result.published;
                if (newFrame) {
                    require(frame->serial>result.published,"frame serial moved backwards");
                    require(frame->width==result.source.width && frame->height==result.source.height,"decoded dimensions differ from probed source");
                    require(frame->pixelFormat==(native?VideoPixelFormat::Yuv420p:VideoPixelFormat::Bgra),"requested pixel path not observed");
                    result.observedFormat=native?"yuv420p":"bgra";
                    result.observerMissed += frame->serial-result.published-1; result.published=frame->serial;
                    const double sync=session.snapshot().syncDifference;
                    const auto renderStart=Clock::now();
                    require(renderer.updateVideo(frame,{}),"real renderer updateVideo failed");
                    const auto updateEnd=Clock::now(); draw(renderer); const auto presented=Clock::now();
                    ++result.delivered;
                    if (result.delivered==1) result.firstFrame=seconds(start,presented)*1000;
                    result.samples.push_back({frame->serial,seconds(start,presented),sync*1000,
                        seconds(renderStart,presented)*1000,seconds(updateEnd,presented)*1000});
                }
                const auto sampleTime=Clock::now();
                if (sampleTime>=nextMemory || newFrame) {
                    const auto mem=memory();
                    result.peakRss=std::max(result.peakRss,mem.PeakWorkingSetSize);
                    result.peakCommit=std::max(result.peakCommit,mem.PeakPagefileUsage);
                    result.peakSampledPrivate=std::max(result.peakSampledPrivate,mem.PrivateUsage);
                    if (sampleTime>=nextMemory) {
                        result.memorySamples.push_back({seconds(start,sampleTime),mem.WorkingSetSize,mem.PrivateUsage});
                        nextMemory=sampleTime+std::chrono::milliseconds(10);
                    }
                }
                const auto state=session.snapshot(); result.dropped=state.droppedFrames;
                if (state.state==PlaybackState::Error) throw std::runtime_error(state.error);
                if (state.state==PlaybackState::Ended) {
                    const auto last=session.frame();
                    if (last && last->serial!=result.published) continue;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(result.delivered>1 && renderer.frameTexture()!=0,"no real sequence rendered");
        } catch (const std::exception& error) { result.error=error.what(); }
    }
    result.elapsed=seconds(start,Clock::now()); result.cpu=processCpu()-cpuStart; result.ended=utc();
    const auto mem=memory(); result.peakRss=std::max(result.peakRss,mem.PeakWorkingSetSize);
    result.peakCommit=std::max(result.peakCommit,mem.PeakPagefileUsage);
    result.peakSampledPrivate=std::max(result.peakSampledPrivate,mem.PrivateUsage);
    if (result.samples.size()>1) result.playbackFps=(result.samples.size()-1)/
        (result.samples.back().elapsed-result.samples.front().elapsed);
    if (!screenshot.empty()) {
        fs::create_directories(screenshot.parent_path());
        if (!renderer.saveScreenshot(screenshot.c_str())) result.error="post-measurement screenshot failed";
    }
    return result;
}
void write(const fs::path& output, const fs::path& file, const std::string& mode, int repeat, bool visible, Result& r) {
    fs::create_directories(output.parent_path()); std::ofstream out(output,std::ios::binary);
    require(bool(out),"cannot open output JSON"); out << std::setprecision(12);
    std::vector<double> sync, absoluteSync, render, present;
    for (auto s:r.samples) { sync.push_back(s.sync); absoluteSync.push_back(std::abs(s.sync)); render.push_back(s.render); present.push_back(s.present); }
    auto field = [&](const char* key, const auto& value) { out << "  " << quote(key) << ": " << value << ",\n"; };
    out << "{\n"; field("schema_version",1); field("status",quote(r.error.empty()?"passed":"failed")); field("error",quote(r.error));
    field("source",quote(file.u8string())); field("mode",quote(mode)); field("repeat",repeat);
    field("started_utc",quote(r.started)); field("ended_utc",quote(r.ended));
    field("renderer",quote("OpenGLRenderer")); field("decoder",quote("ffmpeg"));
    field("gl_vendor",quote(r.vendor)); field("gl_renderer",quote(r.gpu)); field("gl_version",quote(r.glVersion));
    field("hidden_window",visible?"false":"true"); field("window_width",1280); field("window_height",720);
    field("swap_interval_disable_succeeded",r.swapDisabled?"true":"false"); field("swap_interval",r.swapInterval);
    field("observed_pixel_format",quote(r.observedFormat)); field("source_width",r.source.width); field("source_height",r.source.height);
    field("source_codec",quote(r.source.codec)); field("source_pixel_format",quote(r.source.pixelFormat)); field("source_container",quote(r.source.container));
    field("source_audio_streams",r.source.audioStreams); field("source_bytes",r.source.bytes); field("source_fps",r.source.fps);
    field("source_duration_seconds",r.source.duration); field("source_frames",r.source.frames);
    field("elapsed_seconds",r.elapsed); field("process_cpu_seconds",r.cpu); field("logical_processors",r.cores);
    field("cpu_core_equivalents",r.cpu/r.elapsed); field("cpu_one_core_percent",100*r.cpu/r.elapsed);
    field("cpu_machine_percent",100*r.cpu/r.elapsed/r.cores); field("peak_rss_bytes",r.peakRss);
    field("peak_private_commit_bytes",r.peakCommit); field("peak_sampled_private_bytes",r.peakSampledPrivate);
    field("first_frame_ms",r.firstFrame); field("unique_delivered_frames",r.delivered); field("session_published_frames",r.published);
    field("session_dropped_frames",r.dropped); field("observer_missed_frames",r.observerMissed);
    field("unaccounted_source_frames",std::max<std::int64_t>(0,r.source.frames-static_cast<std::int64_t>(r.published+r.dropped)));
    field("delivered_fps",r.delivered/r.elapsed); field("presentation_span_fps",r.playbackFps);
    field("sync_signed_p50_ms",percentile(sync,.5)); field("sync_signed_p95_ms",percentile(sync,.95));
    field("sync_abs_p50_ms",percentile(absoluteSync,.5)); field("sync_abs_p95_ms",percentile(absoluteSync,.95));
    field("render_complete_p50_ms",percentile(render,.5)); field("render_complete_p95_ms",percentile(render,.95));
    field("present_complete_p50_ms",percentile(present,.5)); field("present_complete_p95_ms",percentile(present,.95));
    out << "  \"frames\": [\n";
    for (size_t i=0;i<r.samples.size();++i) { const auto& s=r.samples[i];
        out << "    {\"serial\":" << s.serial << ",\"elapsed_seconds\":" << s.elapsed << ",\"sync_difference_ms\":" << s.sync
            << ",\"render_complete_ms\":" << s.render << ",\"present_complete_ms\":" << s.present << "}" << (i+1<r.samples.size()?",":"") << '\n';
    }
    out << "  ],\n  \"memory_samples\": [\n";
    for (size_t i=0;i<r.memorySamples.size();++i) { const auto& s=r.memorySamples[i];
        out << "    {\"elapsed_seconds\":" << s.elapsed << ",\"rss_bytes\":" << s.rss << ",\"private_bytes\":" << s.privateBytes << "}"
            << (i+1<r.memorySamples.size()?",":"") << '\n';
    }
    out << "  ]\n}\n"; out.flush(); require(bool(out),"write JSON failed");
}
}
int main(int argc,char** argv) {
    try {
        require(argc>=4,"usage: player-benchmark source.avi output.json native-yuv|bgra [--repeat N] [--timeout seconds] [--visible] [--screenshot file.png]");
        const auto file=fs::absolute(fs::u8path(argv[1])), output=fs::absolute(fs::u8path(argv[2]));
        const std::string mode=argv[3]; require(mode=="native-yuv" || mode=="bgra","unknown mode");
        int repeat=1; double timeout=30; bool visible=false; fs::path screenshot;
        for (int i=4;i<argc;++i) {
            const std::string key=argv[i];
            if (key=="--visible") { visible=true; continue; }
            require(i+1<argc,"missing option value");
            if (key=="--repeat") repeat=std::stoi(argv[++i]);
            else if (key=="--timeout") timeout=std::stod(argv[++i]);
            else if (key=="--screenshot") screenshot=fs::absolute(fs::u8path(argv[++i]));
            else throw std::runtime_error("unknown option: "+key);
        }
        require(repeat>0 && std::isfinite(timeout) && timeout>=1 && timeout<=300,"invalid repeat/timeout");
        auto result=measure(file,mode=="native-yuv",visible,timeout,screenshot);
        write(output,file,mode,repeat,visible,result);
        std::cout << (result.error.empty()?"PASS":"FAIL") << " " << mode << " " << result.source.width << 'x' << result.source.height
            << " frames=" << result.delivered << " dropped=" << result.dropped << " elapsed=" << result.elapsed
            << " cpu=" << result.cpu << " JSON=" << output.u8string() << '\n';
        if (!result.error.empty()) std::cerr << result.error << '\n';
        return result.error.empty()?0:1;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
