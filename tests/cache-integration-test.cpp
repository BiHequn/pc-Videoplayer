#include "media/progressive-cache.h"
#include "media/loopback-http.h"
#include <Windows.h>
#include <winhttp.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

void check(bool value,const char* text) { if(!value) throw std::runtime_error(text); }
std::vector<unsigned char> get(const std::string& url,std::size_t limit,const wchar_t* range = nullptr) {
    const std::wstring wide(url.begin(),url.end()); URL_COMPONENTS c{sizeof(c)};
    c.dwHostNameLength=c.dwUrlPathLength=DWORD(-1); check(WinHttpCrackUrl(wide.c_str(),0,0,&c),"parse cache URL");
    auto s=WinHttpOpen(L"cache-test",WINHTTP_ACCESS_TYPE_NO_PROXY,nullptr,nullptr,0);
    WinHttpSetTimeouts(s,3000,3000,3000,3000);
    const std::wstring host(c.lpszHostName,c.dwHostNameLength),path(c.lpszUrlPath,c.dwUrlPathLength);
    auto con=WinHttpConnect(s,host.c_str(),c.nPort,0); auto req=WinHttpOpenRequest(con,L"GET",path.c_str(),nullptr,nullptr,nullptr,0);
    bool ok=WinHttpSendRequest(req,range,range?DWORD(-1):0,nullptr,0,0,0)&&WinHttpReceiveResponse(req,nullptr);
    std::vector<unsigned char> data; if(ok) { unsigned char b[4096]; while(data.size()<limit) { DWORD n=0; if(!WinHttpReadData(req,b,DWORD(std::min(sizeof(b),limit-data.size())),&n)||!n)break;data.insert(data.end(),b,b+n); } }
    WinHttpCloseHandle(req);WinHttpCloseHandle(con);WinHttpCloseHandle(s);check(ok,"cache HTTP read failed");return data;
}
int main(int argc,char** argv) {
    try {
        check(argc==2,"pass test output directory"); const auto root=std::filesystem::u8path(argv[1]);
        std::filesystem::create_directories(root);
        std::vector<unsigned char> expected(6*1024*1024); for(std::size_t i=0;i<expected.size();++i)expected[i]=static_cast<unsigned char>((i*13+7)%251);
        LoopbackHttp origin;
        check(origin.start([&](const LocalHttpRequest& r,LocalHttpResponse& out) {
            std::uint64_t first=0,last=expected.size()-1;
            if(!r.range.empty()) { const auto dash=r.range.find('-');first=std::stoull(r.range.substr(6,dash-6));if(dash+1<r.range.size())last=std::stoull(r.range.substr(dash+1));last=std::min(last,std::uint64_t(expected.size()-1)); }
            const bool partial=!r.range.empty();
            out.headers(partial?206:200,last-first+1,"video/x-flv", "ETag: \"fixed\"\r\nAccept-Ranges: bytes\r\n"+(partial?"Content-Range: bytes "+std::to_string(first)+"-"+std::to_string(last)+"/"+std::to_string(expected.size())+"\r\n":""));
            if(r.method=="HEAD")return;
            for(auto offset=first;offset<=last;) { const auto size=std::min<std::uint64_t>(16384,last-offset+1);if(!out.write(expected.data()+offset,size))break;offset+=size;std::this_thread::sleep_for(std::chrono::milliseconds(12)); }
        }),"origin start");
        const auto source="http://127.0.0.1:"+std::to_string(origin.port())+"/video.flv";
        ProgressiveCache cache; if(!cache.start(source,root.u8string()))throw std::runtime_error("progressive start: "+cache.snapshot().error);
        const auto beginning=get(cache.playbackUrl(),65536,L"Range: bytes=0-65535\r\n");
        check(beginning.size()==65536 && std::equal(beginning.begin(),beginning.end(),expected.begin()),"first data mismatch");
        check(cache.snapshot().downloaded<cache.snapshot().total,"first readable data waited for complete file");
        const auto distant=get(cache.playbackUrl(),65536,L"Range: bytes=5000000-5065535\r\n");
        check(distant.size()==65536 && std::equal(distant.begin(),distant.end(),expected.begin()+5000000),"seek range returned holes or wrong bytes");
        cache.pause(); const auto saved=cache.snapshot().downloaded;std::this_thread::sleep_for(std::chrono::milliseconds(150));
        check(cache.snapshot().state==CacheState::Paused,"pause state missing"); cache.cancel();cache.stop();
        ProgressiveCache resumed;check(resumed.start(source,root.u8string()),"resume start");
        check(resumed.snapshot().downloaded>0,"resume discarded checkpoint");resumed.resume();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(resumed.snapshot().state!=CacheState::Complete && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(resumed.snapshot().state==CacheState::Complete,"download did not complete");
        check(get(resumed.playbackUrl(),expected.size())==expected,"complete cache differs from original");resumed.stop();origin.stop();
        std::cout<<"PASS progressive bytes before complete, priority seek, cancel and resume; paused bytes="<<saved<<"\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}
}
