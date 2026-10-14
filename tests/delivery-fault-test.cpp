#include "app/playback-session.h"
#include "media/progressive-cache.h"
#include "media/loopback-http.h"
#include <Windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
namespace fs=std::filesystem;using Clock=std::chrono::steady_clock;
void require(bool ok,const std::string& text){if(!ok)throw std::runtime_error(text);}
template<class P>void until(P condition,const char* text,int seconds=12) {
    const auto end=Clock::now()+std::chrono::seconds(seconds);
    while(!condition()){require(Clock::now()<end,text);std::this_thread::sleep_for(std::chrono::milliseconds(10));}
}
struct Origin {
    std::vector<unsigned char> bytes=std::vector<unsigned char>(8*1024*1024);
    std::atomic<bool> offline{false};std::atomic<unsigned> failures{0};LoopbackHttp http;
    Origin(){for(std::size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<unsigned char>((i*19+31)%251);
        require(http.start([this](const LocalHttpRequest& r,LocalHttpResponse& out) {
            if(offline){++failures;return;}
            std::uint64_t first=0,last=bytes.size()-1;
            try{require(r.range.rfind("bytes=",0)==0,"origin expects range");const auto dash=r.range.find('-');first=std::stoull(r.range.substr(6,dash-6));last=std::stoull(r.range.substr(dash+1));require(first<=last&&last<bytes.size(),"invalid origin range");}catch(...){out.headers(416,0,"text/plain");return;}
            const auto extra="ETag: \"delivery-fault-v1\"\r\nContent-Range: bytes "+std::to_string(first)+"-"+std::to_string(last)+"/"+std::to_string(bytes.size())+"\r\n";
            if(!out.headers(206,last-first+1,"video/x-flv",extra))return;
            for(auto pos=first;pos<=last;) {
                if(offline){++failures;return;}
                const auto count=std::min<std::uint64_t>(8192,last-pos+1);
                if(!out.write(bytes.data()+pos,static_cast<std::size_t>(count)))return;pos+=count;
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        }),"fault HTTP origin did not start");}
    std::string url()const{return "http://127.0.0.1:"+std::to_string(http.port())+"/video.flv";}
};
std::vector<unsigned char> read(const fs::path& path) {
    std::ifstream f(path,std::ios::binary);require(bool(f),"cannot read completed cache");return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
}
int main(int argc,char** argv) {
    try {
        require(argc==3,"usage: delivery-fault-test audio-fixture output-dir");
        const auto root=fs::absolute(fs::u8path(argv[2]))/(std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));fs::create_directories(root);
        nlohmann::json report{{"passed",false},{"scenarios",nlohmann::json::array()}};
        {
            const auto corrupt=root/"corrupt.avi";std::ofstream(corrupt,std::ios::binary)<<"not a valid media container";
            PlaybackSession player;player.setVolume(0);player.open(corrupt.u8string(),VideoDecoderBackend::Ffmpeg,true);
            until([&]{return player.snapshot().state==PlaybackState::Error;},"corrupt media did not report error");
            require(!player.snapshot().error.empty(),"corrupt media lacks visible error");
            player.open(fs::absolute(fs::u8path(argv[1])).u8string(),VideoDecoderBackend::Ffmpeg,true);
            until([&]{return player.snapshot().state==PlaybackState::Playing&&player.frame();},"valid reopen after corruption failed");
            player.stop();until([&]{return player.snapshot().state==PlaybackState::Idle;},"stop after media recovery failed");
            report["scenarios"].push_back("corrupt-media-error-and-valid-reopen");std::cout<<"PASS corrupt media -> visible error -> valid reopen\n";
        }
        Origin origin;
        {
            const auto blocked=root/"blocked-root";std::ofstream(blocked)<<"regular file prevents cache directory creation";
            ProgressiveCache cache;require(!cache.start(origin.url(),blocked.u8string()),"blocked cache root unexpectedly succeeded");
            require(cache.snapshot().state==CacheState::Error&&!cache.snapshot().error.empty(),"cache root failure lacks error state");
            report["scenarios"].push_back("blocked-cache-root-error");std::cout<<"PASS blocked cache root -> visible Error\n";
        }
        {
            const auto cacheRoot=root/"reconnect-cache";ProgressiveCache cache;
            require(cache.start(origin.url(),cacheRoot.u8string()),"fault cache start failed");
            until([&]{return cache.snapshot().downloaded>128*1024&&cache.snapshot().downloaded<cache.snapshot().total;},"no partial cache before disconnect");
            origin.offline=true;until([&]{return origin.failures>0&&!cache.snapshot().error.empty();},"network disconnect did not surface retry status");
            const auto retained=cache.snapshot().downloaded;
            require(retained>0&&cache.snapshot().state==CacheState::Downloading,"transient disconnect discarded progress or failed immediately");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));origin.offline=false;
            until([&]{return cache.snapshot().state==CacheState::Complete||cache.snapshot().state==CacheState::Error;},"reconnected cache never finished",20);
            require(cache.snapshot().state==CacheState::Complete,"reconnected cache failed: "+cache.snapshot().error);
            fs::path completed;for(const auto& entry:fs::recursive_directory_iterator(cacheRoot))if(entry.path().extension()==".flv")completed=entry.path();
            require(!completed.empty()&&read(completed)==origin.bytes,"reconnected cache differs from original bytes");
            cache.stop();report["scenarios"].push_back("source-disconnect-retry-byte-exact-completion");report["retainedDuringDisconnect"]=retained;
            std::cout<<"PASS source socket disconnect -> retries -> exact complete bytes; retained="<<retained<<'\n';
        }
        report["passed"]=true;report["diskFullTested"]=false;report["permissionAclDeniedTested"]=false;
        std::ofstream(root/"result.json")<<report.dump(2);std::cout<<"PASS delivery fault tests result="<<(root/"result.json").u8string()<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
