#include "demo-http-server.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>
#include <vector>

DemoHttpServer::DemoHttpServer(std::filesystem::path root):root_(std::move(root)){}
DemoHttpServer::~DemoHttpServer(){stop();}
bool DemoHttpServer::start(std::uint16_t port){stopping_=false;return server_.start([this](const LocalHttpRequest& r,LocalHttpResponse& o){serve(r,o);},port);}
void DemoHttpServer::stop(){stopping_=true;server_.stop();}
std::uint16_t DemoHttpServer::port()const{return server_.port();}
void DemoHttpServer::serve(const LocalHttpRequest& request,LocalHttpResponse& response) {
    if(request.method!="GET"&&request.method!="HEAD"){response.headers(405,0,"text/plain");return;}
    const auto& path=request.path;
    bool allowed=path=="/stream.flv"||path=="/hls/index.m3u8";
    if(path.size()==18 && path.rfind("/hls/segment-",0)==0 && path.substr(15)==".ts")
        allowed=path[13]=='0' && path[14]>='0' && path[14]<='7';
    if(!allowed){response.headers(404,0,"text/plain");return;}
    const auto file=root_/std::filesystem::u8path(path.substr(1));
    std::ifstream input(file,std::ios::binary);if(!input){response.headers(404,0,"text/plain");return;}
    input.seekg(0,std::ios::end);const auto length=static_cast<std::uint64_t>(input.tellg());if(!length)return;
    std::uint64_t first=0,last=length-1;
    try {
        if(!request.range.empty()) {
            if(request.range.rfind("bytes=",0)!=0 || request.range.find(',')!=std::string::npos)throw 0;
            const auto dash=request.range.find('-');if(dash==std::string::npos)throw 0;
            first=std::stoull(request.range.substr(6,dash-6));
            if(dash+1<request.range.size())last=std::min(last,std::stoull(request.range.substr(dash+1)));
            if(first>last)throw 0;
        }
    }catch(...){response.headers(416,0,"text/plain","Content-Range: bytes */"+std::to_string(length)+"\r\n");return;}
    const bool partial=!request.range.empty(),playlist=path=="/hls/index.m3u8";
    const std::string validator="\"demo-"+std::to_string(length)+"-"+
        std::to_string(std::filesystem::last_write_time(file).time_since_epoch().count())+"\"";
    if(!response.headers(partial?206:200,last-first+1,playlist?"application/vnd.apple.mpegurl":path=="/stream.flv"?"video/x-flv":"video/mp2t",
        "ETag: "+validator+"\r\nAccept-Ranges: bytes\r\n"+(partial?"Content-Range: bytes "+std::to_string(first)+"-"+std::to_string(last)+"/"+std::to_string(length)+"\r\n":""))||request.method=="HEAD")return;
    input.seekg(static_cast<std::streamoff>(first));char data[8192];
    for(auto at=first;at<=last&&!stopping_;) {
        const auto count=std::min<std::uint64_t>(sizeof(data),last-at+1);input.read(data,static_cast<std::streamsize>(count));
        if(input.gcount()!=count || !response.write(data,static_cast<std::size_t>(count)))break;
        at+=count;if(!playlist&&last>first)std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}
