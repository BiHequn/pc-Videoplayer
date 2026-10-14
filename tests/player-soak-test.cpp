#include "app/playback-session.h"
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using Clock=std::chrono::steady_clock;
namespace fs=std::filesystem;
using Json=nlohmann::json;
void require(bool ok,const std::string& reason){if(!ok)throw std::runtime_error(reason);}
std::uint64_t ticks(FILETIME value){return (std::uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime;}
struct Resources {
    double cpu=0;std::uint64_t rss=0,privateBytes=0;DWORD handles=0,threads=0;
};
Resources resources() {
    Resources r;FILETIME created{},exited{},kernel{},user{};
    require(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)!=FALSE,"process CPU query failed");
    r.cpu=(ticks(kernel)+ticks(user))/10000000.;
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
    require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))!=FALSE,"memory query failed");
    r.rss=memory.WorkingSetSize;r.privateBytes=memory.PrivateUsage;
    require(GetProcessHandleCount(GetCurrentProcess(),&r.handles)!=FALSE,"handle query failed");
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    require(snapshot!=INVALID_HANDLE_VALUE,"thread query failed");
    THREADENTRY32 entry{sizeof(entry)};
    if(Thread32First(snapshot,&entry))do{if(entry.th32OwnerProcessID==GetCurrentProcessId())++r.threads;}while(Thread32Next(snapshot,&entry));
    CloseHandle(snapshot);return r;
}
std::string utc() {
    SYSTEMTIME s{};GetSystemTime(&s);char text[64]{};
    sprintf_s(text,"%04u-%02u-%02uT%02u:%02u:%02uZ",s.wYear,s.wMonth,s.wDay,s.wHour,s.wMinute,s.wSecond);return text;
}
template<class P>void until(PlaybackSession& player,P condition,const char* reason) {
    const auto deadline=Clock::now()+std::chrono::seconds(8);
    while(!condition()) {
        const auto state=player.snapshot();require(state.state!=PlaybackState::Error,std::string(reason)+": "+state.error);
        require(Clock::now()<deadline,reason);std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
}
int main(int argc,char** argv) {
    Json report{{"passed",false},{"startedUtc",utc()}};fs::path output;
    try {
        require(argc>=3,"usage: player-soak-test fixture output-dir [--seconds N] [--cycles N]");
        output=fs::absolute(fs::u8path(argv[2]));fs::create_directories(output);
        const auto fixture=fs::absolute(fs::u8path(argv[1]));
        require(fs::is_regular_file(fixture),"missing soak fixture");
        int seconds=3600,cycles=100;
        for(int i=3;i<argc;++i) {
            const std::string option=argv[i];require(i+1<argc,"missing option value");
            const int value=std::stoi(argv[++i]);
            if(option=="--seconds")seconds=value;else if(option=="--cycles")cycles=value;else throw std::runtime_error("unknown option");
        }
        require(seconds>=1&&seconds<=86400&&cycles>=1&&cycles<=10000,"invalid soak duration/cycles");
        report["requestedSeconds"]=seconds;report["requestedCycles"]=cycles;report["fixture"]=fixture.u8string();
        PlaybackSession player;player.setVolume(0);
        const auto cycleStart=Clock::now();std::uint64_t operations=0;
        for(int i=0;i<cycles;++i) {
            player.open(fixture.u8string(),VideoDecoderBackend::Ffmpeg,true);++operations;
            until(player,[&]{return player.snapshot().state==PlaybackState::Playing&&player.frame();},"cycle open did not play");
            player.pause(true);++operations;
            until(player,[&]{return player.snapshot().state==PlaybackState::Paused;},"cycle pause failed");
            const auto serial=player.frame()->serial;
            player.seek(std::min(1.,player.snapshot().duration*.25));++operations;
            until(player,[&]{return player.frame()&&player.frame()->serial>serial;},"cycle seek did not preview");
            require(player.snapshot().state==PlaybackState::Paused,"cycle seek lost pause");
            player.pause(false);++operations;
            until(player,[&]{return player.snapshot().state==PlaybackState::Playing;},"cycle resume failed");
            player.stop();++operations;
            until(player,[&]{return player.snapshot().state==PlaybackState::Idle&&!player.frame();},"cycle stop failed");
            if((i+1)%10==0)std::cout<<"cycles="<<i+1<<" operations="<<operations<<std::endl;
        }
        report["completedCycles"]=cycles;report["operations"]=operations;
        report["cycleSeconds"]=std::chrono::duration<double>(Clock::now()-cycleStart).count();
        player.open(fixture.u8string(),VideoDecoderBackend::Ffmpeg,true);
        until(player,[&]{return player.snapshot().state==PlaybackState::Playing&&player.frame();},"soak first play failed");
        const auto baseline=resources();auto previous=baseline;
        SYSTEM_INFO system{};GetSystemInfo(&system);
        std::ofstream samples(output/"samples.csv");require(bool(samples),"cannot create soak samples");
        samples<<"elapsed_seconds,cpu_percent_machine,rss_bytes,private_bytes,handles,threads,frames,loops,position,sync_seconds,state\n";
        const auto start=Clock::now();auto lastSample=start;std::uint64_t lastSerial=0,frames=0,loops=0;
        Resources peak=baseline;double maxSync=0;std::size_t sampleCount=0;
        report["soakStartedUtc"]=utc();report["logicalProcessors"]=system.dwNumberOfProcessors;
        std::ofstream(output/"progress.json")<<report.dump(2);
        for(;;) {
            const auto now=Clock::now();const double elapsed=std::chrono::duration<double>(now-start).count();
            const auto state=player.snapshot();const auto frame=player.frame();
            require(state.state!=PlaybackState::Error,"soak playback error: "+state.error);
            require(state.state!=PlaybackState::Buffering,"local fixture unexpectedly buffering");
            if(frame&&frame->serial!=lastSerial){lastSerial=frame->serial;++frames;if(std::isfinite(state.syncDifference))maxSync=std::max(maxSync,std::abs(state.syncDifference));}
            if(now-lastSample>=std::chrono::seconds(1)||elapsed>=seconds) {
                const auto r=resources();const double interval=std::chrono::duration<double>(now-lastSample).count();
                const double cpu=interval>0?100*(r.cpu-previous.cpu)/interval/std::max<DWORD>(1,system.dwNumberOfProcessors):0;
                samples<<elapsed<<','<<cpu<<','<<r.rss<<','<<r.privateBytes<<','<<r.handles<<','<<r.threads<<','<<frames<<','<<loops<<','<<state.position<<','<<state.syncDifference<<','<<int(state.state)<<'\n';samples.flush();
                require(bool(samples),"soak sample write failed");
                peak.rss=std::max(peak.rss,r.rss);peak.privateBytes=std::max(peak.privateBytes,r.privateBytes);peak.handles=std::max(peak.handles,r.handles);peak.threads=std::max(peak.threads,r.threads);
                previous=r;lastSample=now;++sampleCount;
                if(sampleCount%60==0){report["elapsedSeconds"]=elapsed;report["framesObserved"]=frames;report["completedLoops"]=loops;report["samples"]=sampleCount;std::ofstream(output/"progress.json")<<report.dump(2);std::cout<<"soak seconds="<<elapsed<<" loops="<<loops<<" rss="<<r.rss<<" handles="<<r.handles<<std::endl;}
            }
            if(elapsed>=seconds)break;
            if(state.state==PlaybackState::Ended){++loops;player.open(fixture.u8string(),VideoDecoderBackend::Ffmpeg,true);until(player,[&]{return player.snapshot().state==PlaybackState::Playing;},"soak loop reopen failed");}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const double elapsed=std::chrono::duration<double>(Clock::now()-start).count();
        const auto activeEnd=resources();player.stop();until(player,[&]{return player.snapshot().state==PlaybackState::Idle;},"soak final stop failed");
        require(frames>0,"soak observed no frames");
        const auto growth=[](std::uint64_t end,std::uint64_t begin){return end>begin?end-begin:0;};
        report["elapsedSeconds"]=elapsed;report["framesObserved"]=frames;report["completedLoops"]=loops;report["samples"]=sampleCount;report["maxAbsSyncSeconds"]=maxSync;
        report["baseline"]={{"rssBytes",baseline.rss},{"privateBytes",baseline.privateBytes},{"handles",baseline.handles},{"threads",baseline.threads}};
        report["activeEnd"]={{"rssBytes",activeEnd.rss},{"privateBytes",activeEnd.privateBytes},{"handles",activeEnd.handles},{"threads",activeEnd.threads}};
        report["peak"]={{"rssBytes",peak.rss},{"privateBytes",peak.privateBytes},{"handles",peak.handles},{"threads",peak.threads}};
        report["cpuSeconds"]=activeEnd.cpu-baseline.cpu;
        report["cpuPercentMachine"]=100*(activeEnd.cpu-baseline.cpu)/elapsed/std::max<DWORD>(1,system.dwNumberOfProcessors);
        require(growth(activeEnd.privateBytes,baseline.privateBytes)<64*1024*1024,"soak private memory grew by >=64MiB");
        require(growth(activeEnd.handles,baseline.handles)<32,"soak handles grew by >=32");
        require(growth(activeEnd.threads,baseline.threads)<8,"soak threads grew by >=8");
        report["passed"]=true;report["finishedUtc"]=utc();std::ofstream(output/"result.json")<<report.dump(2);
        std::cout<<"PASS cycles="<<cycles<<" operations="<<operations<<" elapsed="<<elapsed<<" frames="<<frames<<" loops="<<loops<<std::endl;return 0;
    }catch(const std::exception& e){report["error"]=e.what();report["finishedUtc"]=utc();if(!output.empty())std::ofstream(output/"result.json")<<report.dump(2);std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}
}
