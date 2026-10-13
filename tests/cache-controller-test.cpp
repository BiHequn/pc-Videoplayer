#include "app/cache-controller.h"
#include "media/loopback-http.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using Clock=std::chrono::steady_clock;
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
template<class P>void until(P predicate) {
    const auto deadline=Clock::now()+std::chrono::seconds(6);
    while(!predicate()){require(Clock::now()<deadline,"controller timeout");std::this_thread::sleep_for(std::chrono::milliseconds(5));}
}

// Removing the generation check from old Cancel's post-stop cleanup must fail:
// B stays Preparing while A's network read times out and queued controls drain.
void oldCancelStopPreservesReplacement() {
    using namespace std::chrono_literals;
    std::atomic<bool> releaseA{false},releaseB{false},aDisconnected{false};
    std::atomic<unsigned int> aBodies{0},bProbes{0};
    const std::vector<unsigned char> bytes(512*1024,0x5a);
    LoopbackHttp origin;
    require(origin.start([&](const LocalHttpRequest& request,LocalHttpResponse& response) {
        const bool a=request.path=="/a.flv",b=request.path=="/b.flv";
        if(!a&&!b){response.headers(404,0,"text/plain");return;}
        const bool probe=request.range=="bytes=0-0";
        if(a&&!probe) {
            ++aBodies;
            // No headers arrive: the real WinHTTP receive must time out before
            // the cache worker can exit and old Cancel's stop can finish.
            while(!releaseA&&response.connected())std::this_thread::sleep_for(2ms);
            if(!response.connected()){aDisconnected=true;return;}
        }
        if(b&&probe) {
            ++bProbes;
            while(!releaseB&&response.connected())std::this_thread::sleep_for(2ms);
            if(!response.connected())return;
        }
        const std::size_t count=probe?1:bytes.size();
        const std::string headers="ETag: \"cancel-replacement-v1\"\r\nContent-Range: bytes 0-"+
            std::to_string(count-1)+"/524288\r\n";
        if(response.headers(206,count,"video/x-flv",headers))response.write(bytes.data(),count);
    }),"cancel replacement origin start");
    const auto base="http://127.0.0.1:"+std::to_string(origin.port());
    const auto aUrl=base+"/a.flv",bUrl=base+"/b.flv";
    const auto root=(std::filesystem::temp_directory_path()/
        ("cache-controller-cancel-"+std::to_string(GetCurrentProcessId())+"-"+
         std::to_string(GetTickCount64()))).u8string();
    {
        CacheController cache;
        struct Cleanup {
            CacheController& cache;
            std::atomic<bool>& a;
            std::atomic<bool>& b;
            ~Cleanup(){a=true;b=true;cache.cancel();}
        } cleanup{cache,releaseA,releaseB};
        cache.start(aUrl,root);
        until([&]{return aBodies>0&&cache.status().second.state==CacheState::Downloading;});
        const auto old=cache.status();
        require(!old.second.playbackUrl.empty(),"A never exposed a real playback endpoint");
        const auto cancelledAt=Clock::now();
        cache.cancel();
        // Let the controller enter stop while the already-observed origin
        // request is still blocked. Later require its natural timeout too.
        const auto settle=Clock::now()+150ms;
        while(Clock::now()<settle) {
            require(!aDisconnected,"A timed out before old Cancel entered its stop window");
            std::this_thread::sleep_for(2ms);
        }
        // These real public commands widen the post-stop/pre-Start boundary;
        // no private-state instrumentation or probabilistic thread race loop.
        for(unsigned int i=0;i<262144;++i)cache.pause();
        cache.start(bUrl,root);
        const auto generation=cache.status().first;
        require(generation>old.first,"replacement did not get a new generation");
        const auto checkPreparing=[&] {
            const auto current=cache.status();
            require(current.first==generation&&current.second.source==bUrl,
                "old Cancel associated the replacement generation with A");
            require(current.second.state==CacheState::Preparing&&current.second.playbackUrl.empty(),
                "old Cancel cleared replacement Preparing or exposed an old playback URL");
        };
        const auto probeDeadline=Clock::now()+6s;
        while(!bProbes) {
            checkPreparing();
            require(Clock::now()<probeDeadline,"B did not reach its held metadata probe");
            // Poll continuously across cleanup: sleeping here can miss the
            // stale status if queued controls drain before the next sample.
        }
        require(aDisconnected&&Clock::now()-cancelledAt>=2s,
            "fixture did not exercise old Cancel blocked on a real network timeout");
        // Continue polling while B really is probing. Old cleanup must never
        // surface A's cancelled cache while B's initialization is in progress.
        const auto heldProbe=Clock::now()+100ms;
        while(Clock::now()<heldProbe){checkPreparing();std::this_thread::sleep_for(1ms);}
        releaseB=true;
        until([&] {
            const auto current=cache.status();
            require(current.first==generation&&current.second.source==bUrl,
                "replacement source/generation changed during completion");
            require(current.second.playbackUrl.empty()||current.second.playbackUrl!=old.second.playbackUrl,
                "replacement exposed A's old playback URL");
            require(current.second.state!=CacheState::Error&&current.second.state!=CacheState::Cancelled,
                "replacement failed or was cancelled by the obsolete command");
            return current.second.state==CacheState::Complete;
        });
        const auto completed=cache.status().second;
        require(completed.downloaded==524288&&completed.total==524288&&
            !completed.playbackUrl.empty()&&completed.playbackUrl!=old.second.playbackUrl,
            "B did not complete on its own playback endpoint");
        std::cout<<"PASS old Cancel blocked stop, coherent replacement Preparing, no old URL, complete B\n";
    }
    releaseA=true;releaseB=true;origin.stop();
}

int main() {
    try {
        std::atomic<bool> release{false};std::atomic<int> probes{0};LoopbackHttp origin;
        require(origin.start([&](const LocalHttpRequest&,LocalHttpResponse& out){++probes;
            while(!release)std::this_thread::sleep_for(std::chrono::milliseconds(5));out.headers(404,0,"text/plain");}),"origin start");
        const auto url="http://127.0.0.1:"+std::to_string(origin.port())+"/index.m3u8";
        const auto root=(std::filesystem::temp_directory_path()/std::to_string(GetCurrentProcessId())).u8string();
        {
            CacheController cache;cache.start(url,root);cache.cancel();
            until([&]{return cache.snapshot().state==CacheState::Cancelled;});
            require(cache.status().second.playbackUrl.empty(),"cancel opens cache");
        }
        {
            CacheController cache;cache.start(url,root);
            until([&]{return probes>0;});
            const auto prior=cache.status();cache.cancel();
            until([&]{return cache.snapshot().state==CacheState::Cancelled;});
            cache.start("http://127.0.0.1:1/replacement.flv",root);
            const auto next=cache.status();
            require(next.first>prior.first && next.second.source.find("replacement.flv")!=std::string::npos,
                "task generation and source mismatch");
            cache.cancel();
        }
        release=true;origin.stop();
        std::cout<<"PASS pending cancel, probing cancel, coherent task snapshot and bounded teardown\n";
        oldCancelStopPreservesReplacement();
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
