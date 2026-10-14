#include "progressive-cache.h"
#include "loopback-http.h"
#include <Windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlwapi.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <cctype>

namespace {
using json=nlohmann::json;namespace fs=std::filesystem;
constexpr std::uint64_t blockSize=512*1024;
std::wstring wide(const std::string& s) {if(s.empty())return {};int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);std::wstring r(n,0);if(n)MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),r.data(),n);return r;}
std::string narrow(const std::wstring& s) {int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);std::string r(n,0);if(n)WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),r.data(),n,nullptr,nullptr);return r;}
struct Internet {HINTERNET h=nullptr;explicit Internet(HINTERNET v):h(v){}~Internet(){if(h)WinHttpCloseHandle(h);}operator HINTERNET()const{return h;}};
struct Info {std::uint64_t length=0;std::string etag,modified,finalUrl;bool range=false;};
std::string header(HINTERNET r,DWORD key) {DWORD n=0;WinHttpQueryHeaders(r,key,nullptr,nullptr,&n,nullptr);if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER)return {};std::wstring s(n/sizeof(wchar_t),0);if(!WinHttpQueryHeaders(r,key,nullptr,s.data(),&n,nullptr))return {};while(!s.empty()&&!s.back())s.pop_back();return narrow(s);}
bool http(const std::string& url,const std::string& range,const std::string& validator,Info& info,const std::function<bool(const unsigned char*,std::size_t)>& sink,const std::atomic<bool>& stop,std::string& error,std::uint64_t expectedTotal=0) {
    auto u=wide(url);URL_COMPONENTS c{sizeof(c)};c.dwHostNameLength=c.dwUrlPathLength=c.dwExtraInfoLength=DWORD(-1);
    if(!WinHttpCrackUrl(u.c_str(),0,0,&c)||(c.nScheme!=INTERNET_SCHEME_HTTP&&c.nScheme!=INTERNET_SCHEME_HTTPS)){error="请输入 HTTP/HTTPS FLV 或 HLS 地址。";return false;}
    Internet session(WinHttpOpen(L"NativePlayerCache/2",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0));if(!session){error="创建 HTTP 会话失败。";return false;}WinHttpSetTimeouts(session,3000,3000,3000,3000);
    std::wstring host(c.lpszHostName,c.dwHostNameLength),path(c.lpszUrlPath,c.dwUrlPathLength);if(c.dwExtraInfoLength)path.append(c.lpszExtraInfo,c.dwExtraInfoLength);
    Internet con(WinHttpConnect(session,host.c_str(),c.nPort,0));Internet request(WinHttpOpenRequest(con,L"GET",path.c_str(),nullptr,nullptr,nullptr,c.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0));
    if(!request){error="创建 HTTP 请求失败。";return false;}
    auto headers=wide((range.empty()?"":"Range: bytes="+range+"\r\n")+(validator.empty()?"":"If-Range: "+validator+"\r\n"));
    if(stop||!WinHttpSendRequest(request,headers.c_str(),DWORD(headers.size()),nullptr,0,0,0)||!WinHttpReceiveResponse(request,nullptr)){error="网络连接中断，等待重试。";return false;}
    DWORD status=0,n=sizeof(status);WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&n,nullptr);
    info={};info.etag=header(request,WINHTTP_QUERY_ETAG);info.modified=header(request,WINHTTP_QUERY_LAST_MODIFIED);auto len=header(request,WINHTTP_QUERY_CONTENT_LENGTH);if(!len.empty())try{info.length=std::stoull(len);}catch(...){}
    if(status<200||status>=300){info.length=0;error="HTTP 状态 "+std::to_string(status);return false;}
    auto cr=header(request,WINHTTP_QUERY_CONTENT_RANGE);
    if(!range.empty()) {
        try {
            const auto dash=range.find('-'); const auto expectedFirst=std::stoull(range.substr(0,dash));
            const auto expectedLast=std::stoull(range.substr(dash+1));
            const auto responseDash=cr.find('-'), slash=cr.find('/');
            if(status!=206 || cr.rfind("bytes ",0)!=0 || responseDash==std::string::npos || slash==std::string::npos)
                throw std::runtime_error("range");
            const auto first=std::stoull(cr.substr(6,responseDash-6));
            const auto last=std::stoull(cr.substr(responseDash+1,slash-responseDash-1));
            const auto total=std::stoull(cr.substr(slash+1));
            if(first!=expectedFirst || last!=expectedLast || last<first || last>=total || info.length!=last-first+1)
                throw std::runtime_error("range");
            if(expectedTotal && total!=expectedTotal) {error="源文件在下载中发生变化。";return false;}
            info.length=total;info.range=true;
        } catch(...) {error="源服务器不支持可靠的 Range 续传，请使用直接网络播放。";return false;}
        if(!validator.empty() && (validator.front()=='"'?validator!=info.etag:validator!=info.modified)) {
            error="源文件在下载中发生变化。";return false;
        }
    }
    DWORD bytes=0;WinHttpQueryOption(request,WINHTTP_OPTION_URL,nullptr,&bytes);if(bytes){std::wstring final(bytes/2+1,0);if(WinHttpQueryOption(request,WINHTTP_OPTION_URL,final.data(),&bytes)){final.resize(wcslen(final.c_str()));info.finalUrl=narrow(final);}}
    if(!sink)return true;
    unsigned char buffer[65536];while(!stop){DWORD got=0;if(!WinHttpReadData(request,buffer,sizeof(buffer),&got)){error="HTTP 读取中断。";return false;}if(!got)return true;if(!sink(buffer,got))return false;}return false;
}
std::string hash(const std::string& s){BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE h=nullptr;DWORD size=0,n=0;std::vector<unsigned char> object,digest(32);std::string out;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA256 init");BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&n,0);object.resize(size);
    if(BCryptCreateHash(alg,&h,object.data(),size,nullptr,0,0)>=0&&BCryptHashData(h,reinterpret_cast<PUCHAR>(const_cast<char*>(s.data())),ULONG(s.size()),0)>=0&&BCryptFinishHash(h,digest.data(),32,0)>=0){const char* hex="0123456789abcdef";for(auto b:digest){out+=hex[b>>4];out+=hex[b&15];}}if(h)BCryptDestroyHash(h);BCryptCloseAlgorithmProvider(alg,0);if(out.empty())throw std::runtime_error("SHA256 failed");return out;}
std::string resolve(const std::string& base,const std::string& path){auto b=wide(base),p=wide(path);std::wstring result(32768,0);DWORD n=DWORD(result.size());if(FAILED(UrlCombineW(b.c_str(),p.c_str(),result.data(),&n,0)))throw std::runtime_error("HLS URL resolve");result.resize(wcslen(result.c_str()));return narrow(result);}
struct Block {std::uint64_t begin=0,end=0,next=0,durable=0;bool active=false;};
struct Resource {std::string url,etag,modified;fs::path file;std::uint64_t length=0;std::vector<Block> blocks;};
}
struct ProgressiveCache::Impl {
    mutable std::mutex mutex;std::condition_variable changed;std::atomic<bool> quitting{false},abortRequested{false};bool paused=false,cancelled=false;
    CacheSnapshot state;std::vector<Resource> resources;fs::path directory;std::string playlist;bool hls=false;
    HANDLE directoryLock=INVALID_HANDLE_VALUE;
    LoopbackHttp server;std::vector<std::thread> workers;std::pair<std::size_t,std::uint64_t> priority{0,0};std::size_t waiters=0;
    std::chrono::steady_clock::time_point started;std::uint64_t initial=0;
    std::uint64_t downloaded()const {std::uint64_t n=0;for(auto& r:resources)for(auto& b:r.blocks)n+=b.next-b.begin;return n;}
    void update() {state.downloaded=downloaded();state.waitingForData=waiters>0;state.segmentsComplete=0;state.ranges.clear();for(std::size_t i=0;i<resources.size();++i){bool complete=true;for(auto& b:resources[i].blocks){complete &= b.next==b.end;if(!hls&&b.next>b.begin){if(!state.ranges.empty()&&state.ranges.back().second==b.begin)state.ranges.back().second=b.next;else state.ranges.emplace_back(b.begin,b.next);}}state.segmentsComplete+=complete;}
        const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();state.bytesPerSecond=elapsed>0?(state.downloaded-initial)/elapsed:0;
        bool durable=state.total>0;
        for(const auto& r:resources) for(const auto& b:r.blocks) durable &= b.durable==b.end;
        if(durable && state.state!=CacheState::Error && !cancelled) {state.state=CacheState::Complete;state.error.clear();}
    }
    bool save() {
        json root{{"version",1},{"source",state.source},{"resources",json::array()}};
        for(const auto& r:resources){json item{{"url",r.url},{"etag",r.etag},{"modified",r.modified},{"length",r.length},{"blocks",json::array()}};
            for(const auto& b:r.blocks)item["blocks"].push_back(b.durable);root["resources"].push_back(item);}
        const auto bytes=root.dump();const auto tmp=directory/L"state.tmp";
        HANDLE file=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return false;
        DWORD written=0;const bool durable=bytes.size()<=MAXDWORD && WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr) &&
            written==bytes.size() && FlushFileBuffers(file);CloseHandle(file);if(!durable)return false;
        for(int attempt=0;attempt<50;++attempt) {
            if(MoveFileExW(tmp.c_str(),(directory/L"state.json").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return true;
            const auto error=GetLastError();if(error!=ERROR_SHARING_VIOLATION&&error!=ERROR_ACCESS_DENIED)return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
    void fail(const std::string& error){std::lock_guard<std::mutex> lock(mutex);state.error=error;state.state=CacheState::Error;quitting=true;changed.notify_all();}
    void work() {try {while(!quitting) {
        std::size_t ri=0,bi=0;bool found=false;
        {std::unique_lock<std::mutex> lock(mutex);changed.wait_for(lock,std::chrono::milliseconds(30),[&]{return quitting||!paused;});if(quitting)break;if(paused)continue;std::uint64_t scoreBest=UINT64_MAX;
         for(std::size_t i=0;i<resources.size();++i)for(std::size_t j=0;j<resources[i].blocks.size();++j){auto& b=resources[i].blocks[j];if(b.active||b.next==b.end)continue;const bool requestedResource=i==priority.first&&b.end>priority.second;const std::uint64_t score=requestedResource?b.begin/ blockSize:i*1000000ULL+j+1000000000ULL;if(score<scoreBest){scoreBest=score;ri=i;bi=j;found=true;}}
         if(!found){update();if(state.state==CacheState::Complete)break;changed.wait_for(lock,std::chrono::milliseconds(30));continue;}resources[ri].blocks[bi].active=true;}
        auto& r=resources[ri];auto& b=r.blocks[bi];HANDLE file=CreateFileW(r.file.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE){fail("打开缓存文件失败。");break;}
        auto offset=b.next;bool success=false,localError=false;std::string error;auto until=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(!quitting){
            {std::unique_lock<std::mutex> lock(mutex);if(paused){changed.wait(lock,[&]{return !paused||quitting;});until=std::chrono::steady_clock::now()+std::chrono::seconds(30);}}
            if(quitting||std::chrono::steady_clock::now()>=until)break;
            Info info;const auto requested=offset;
            success=http(r.url,std::to_string(offset)+"-"+std::to_string(b.end-1),r.etag.empty()?r.modified:r.etag,info,[&](const unsigned char* data,std::size_t n){
                {std::unique_lock<std::mutex> lock(mutex);changed.wait(lock,[&]{return !paused||quitting;});if(quitting)return false;}
                if(n>b.end-offset){error="HTTP 数据范围超出分块。";localError=true;return false;}LARGE_INTEGER at;at.QuadPart=offset;DWORD written=0;if(!SetFilePointerEx(file,at,nullptr,FILE_BEGIN)||!WriteFile(file,data,DWORD(n),&written,nullptr)||written!=n){error="缓存写入失败。";localError=true;return false;}offset+=n;
                until=std::chrono::steady_clock::now()+std::chrono::seconds(30);
                {std::lock_guard<std::mutex> lock(mutex);b.next=offset;update();changed.notify_all();}return !quitting;
            },quitting,error,r.length);
            // A response may close immediately after its exact final bytes. The
            // validated length and received offset establish block completion.
            if(offset==b.end && !localError){success=true;break;}
            if(info.length&&info.length!=r.length){error="源文件在下载中发生变化。";localError=true;success=false;}
            if(success&&offset==b.end)break;if(success&&offset!=b.end){success=false;error="分块响应提前结束。";}
            if(quitting||localError)break;
            {std::lock_guard<std::mutex> lock(mutex);state.error=error;}
            std::this_thread::sleep_for(std::chrono::milliseconds(offset>requested?50:250));
        }
        bool persisted=FlushFileBuffers(file)!=FALSE;CloseHandle(file);
        {std::lock_guard<std::mutex> lock(mutex);if(persisted){const auto previous=b.durable;b.durable=offset;persisted=save();if(!persisted)b.durable=previous;}b.active=false;update();changed.notify_all();}
        if(!persisted){fail("缓存进度落盘失败。");break;}if(!success&&!quitting){fail(error.empty()?"下载重试超时。":error);break;}
    }}catch(const std::exception& e){fail(e.what());}}
    void serve(const LocalHttpRequest& req,LocalHttpResponse& out) {
        if(req.method!="GET"&&req.method!="HEAD"){out.headers(405,0,"text/plain");return;}
        if(req.path=="/index.m3u8"&&hls){out.headers(200,playlist.size(),"application/vnd.apple.mpegurl");if(req.method=="GET")out.write(playlist.data(),playlist.size());return;}
        std::size_t index=0;try{if(hls){if(req.path.rfind("/segment/",0)!=0)throw std::runtime_error("path");std::size_t used=0;index=std::stoull(req.path.substr(9),&used);if(used!=req.path.size()-9)throw std::runtime_error("path");}else if(req.path!="/video.flv")throw std::runtime_error("path");}catch(...){out.headers(404,0,"text/plain");return;}
        if(index>=resources.size()){out.headers(404,0,"text/plain");return;}auto& r=resources[index];std::uint64_t first=0,last=r.length-1;
        try {if(!req.range.empty()){
            if(req.range.rfind("bytes=",0)!=0||req.range.find(',')!=std::string::npos)throw std::runtime_error("range");
            const auto dash=req.range.find('-');if(dash==std::string::npos)throw std::runtime_error("range");
            auto number=[](const std::string& s){if(s.empty()||s.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("range");return std::stoull(s);};
            if(dash==6){const auto suffix=number(req.range.substr(7));if(!suffix)throw std::runtime_error("range");first=suffix>=r.length?0:r.length-suffix;}
            else {first=number(req.range.substr(6,dash-6));if(dash+1<req.range.size())last=std::min<std::uint64_t>(number(req.range.substr(dash+1)),r.length-1);}
            if(first>last)throw std::runtime_error("range");
        }}catch(...){out.headers(416,0,"text/plain","Content-Range: bytes */"+std::to_string(r.length)+"\r\n");return;}
        const bool partial=!req.range.empty();if(!out.headers(partial?206:200,last-first+1,hls?"video/mp2t":"video/x-flv","Accept-Ranges: bytes\r\n"+(partial?"Content-Range: bytes "+std::to_string(first)+"-"+std::to_string(last)+"/"+std::to_string(r.length)+"\r\n":""))||req.method=="HEAD")return;
        HANDLE file=CreateFileW(r.file.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return;
        unsigned char buffer[65536];for(auto pos=first;pos<=last&&!quitting;){std::uint64_t available=0;
            {std::unique_lock<std::mutex> lock(mutex);auto& b=r.blocks[std::size_t(pos/blockSize)];priority={index,pos};changed.notify_all();
                if(b.next<=pos){++waiters;update();while(!quitting&&b.next<=pos&&out.connected())changed.wait_for(lock,std::chrono::milliseconds(100));--waiters;update();}
                if(quitting||b.next<=pos)break;available=std::min<std::uint64_t>({b.next-pos,last-pos+1,sizeof(buffer)});}
            LARGE_INTEGER at;at.QuadPart=pos;DWORD got=0;if(!SetFilePointerEx(file,at,nullptr,FILE_BEGIN)||!ReadFile(file,buffer,DWORD(available),&got,nullptr)||!got||!out.write(buffer,got))break;pos+=got;
        }CloseHandle(file);
    }
};
ProgressiveCache::ProgressiveCache():impl_(new Impl){} ProgressiveCache::~ProgressiveCache(){stop();}
bool ProgressiveCache::start(const std::string& url,const std::string& root) {
    stop();auto& p=*impl_;p.quitting=p.abortRequested.load();p.paused=p.cancelled=false;p.waiters=0;p.priority={0,0};p.state={};p.state.source=url;p.state.state=CacheState::Preparing;p.resources.clear();p.playlist.clear();p.started=std::chrono::steady_clock::now();
    try {
        p.directory=fs::u8path(root)/hash(url);fs::create_directories(p.directory);
        p.directoryLock=CreateFileW((p.directory/L"task.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(p.directoryLock==INVALID_HANDLE_VALUE)throw std::runtime_error("相同缓存正在被另一个播放器使用。");
        json saved=json::object();try{std::ifstream f(p.directory/L"state.json");if(f)f>>saved;}catch(...){saved=json::object();}if(!saved.is_object())saved=json::object();
        auto clean=url.substr(0,url.find_first_of("?#"));std::transform(clean.begin(),clean.end(),clean.begin(),[](unsigned char c){return char(std::tolower(c));});p.hls=clean.size()>=5&&clean.substr(clean.size()-5)==".m3u8";
        std::vector<std::string> urls;std::string error;Info info;
        if(p.hls){std::string playlistUrl=url,text;for(int depth=0;depth<4;++depth){text.clear();if(!http(playlistUrl,{}, {},info,[&](const unsigned char* b,std::size_t n){if(text.size()+n>8*1024*1024)return false;text.append(reinterpret_cast<const char*>(b),n);return true;},p.quitting,error))throw std::runtime_error(error);
            std::istringstream lines(text);std::string line,output,variant;bool nextVariant=false,end=false;urls.clear();while(std::getline(lines,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.rfind("#EXT-X-STREAM-INF",0)==0)nextVariant=true;else if(!line.empty()&&line.front()!='#'){auto resolved=resolve(info.finalUrl.empty()?playlistUrl:info.finalUrl,line);if(nextVariant){if(variant.empty())variant=resolved;nextVariant=false;}else {output+="/segment/"+std::to_string(urls.size())+"\n";urls.push_back(resolved);}}else {if(line.rfind("#EXT-X-KEY",0)==0&&line.find("METHOD=NONE")==std::string::npos)throw std::runtime_error("加密 HLS 请使用直接播放。");if(line.rfind("#EXT-X-MAP",0)==0||line.rfind("#EXT-X-BYTERANGE",0)==0||line.rfind("#EXT-X-MEDIA:",0)==0)throw std::runtime_error("缓存演示支持 MPEG-TS HLS 单音轨点播。");end|=line=="#EXT-X-ENDLIST";output+=line+"\n";}}
            if(!variant.empty()){playlistUrl=variant;continue;}if(text.rfind("#EXTM3U",0)!=0||!end||urls.empty())throw std::runtime_error("缓存需要有限的 HLS 点播列表。");p.playlist=std::move(output);break;}if(p.playlist.empty())throw std::runtime_error("HLS 变体嵌套过深。");
        }else{if(clean.size()<4||clean.substr(clean.size()-4)!=".flv")throw std::runtime_error("缓存模式支持 FLV / HLS 地址。");urls.push_back(url);}
        for(std::size_t i=0;i<urls.size();++i){Info metadata;if(!http(urls[i],"0-0",{},metadata,{},p.quitting,error)||!metadata.length)throw std::runtime_error(error.empty()?"无法确认资源长度。":error);
            if(metadata.etag.rfind("W/",0)==0)metadata.etag.clear();
            if(metadata.etag.empty()&&metadata.modified.empty())throw std::runtime_error("源服务器未提供可靠的文件校验标识，请选择直接播放。");
            Resource r;r.url=urls[i];r.etag=metadata.etag;r.modified=metadata.modified;r.length=metadata.length;r.file=p.directory/(std::to_string(i)+(p.hls?".ts":".flv"));
            bool resume=saved.value("source",std::string())==url&&saved.contains("resources")&&saved["resources"].size()>i;json old;if(resume){old=saved["resources"][i];resume=old.value("url",std::string())==r.url&&old.value("length",0ULL)==r.length&&old.value("etag",std::string())==r.etag&&old.value("modified",std::string())==r.modified&&(!r.etag.empty()||!r.modified.empty())&&fs::exists(r.file)&&fs::file_size(r.file)==r.length;}
            HANDLE f=CreateFileW(r.file.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,resume?OPEN_EXISTING:CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE)throw std::runtime_error("创建缓存文件失败。");LARGE_INTEGER length;length.QuadPart=r.length;bool ok=SetFilePointerEx(f,length,nullptr,FILE_BEGIN)&&SetEndOfFile(f);CloseHandle(f);if(!ok)throw std::runtime_error("分配缓存文件失败。");
            for(std::uint64_t first=0,j=0;first<r.length;first+=blockSize,++j){auto end=std::min(first+blockSize,r.length),next=first;if(resume&&old.contains("blocks")&&old["blocks"].size()>j)next=std::clamp(old["blocks"][j].get<std::uint64_t>(),first,end);r.blocks.push_back({first,end,next,next,false});}p.state.total+=r.length;p.resources.push_back(std::move(r));}
        p.state.segmentsTotal=p.hls?p.resources.size():0;p.initial=p.downloaded();p.update();if(!p.save())throw std::runtime_error("保存下载元数据失败。");
        if(!p.server.start([&p](const LocalHttpRequest& r,LocalHttpResponse& out){p.serve(r,out);}))throw std::runtime_error("启动缓存读取服务失败。");p.state.playbackUrl="http://127.0.0.1:"+std::to_string(p.server.port())+(p.hls?"/index.m3u8":"/video.flv");p.state.state=p.state.downloaded==p.state.total?CacheState::Complete:CacheState::Downloading;
        for(int i=0;i<4;++i)p.workers.emplace_back([&p]{p.work();});return true;
    }catch(const std::exception& e){p.fail(e.what());return false;}
}
void ProgressiveCache::pause(){auto& p=*impl_;std::lock_guard<std::mutex> lock(p.mutex);if(p.state.state==CacheState::Downloading){p.paused=true;p.state.state=CacheState::Paused;}}
void ProgressiveCache::resume(){auto& p=*impl_;std::lock_guard<std::mutex> lock(p.mutex);if(p.state.state==CacheState::Paused){p.paused=false;p.state.state=CacheState::Downloading;p.changed.notify_all();}}
void ProgressiveCache::cancel(){auto& p=*impl_;{std::lock_guard<std::mutex> lock(p.mutex);p.cancelled=true;p.quitting=true;if(p.state.state!=CacheState::Complete)p.state.state=CacheState::Cancelled;}p.changed.notify_all();}
void ProgressiveCache::requestAbort(){impl_->abortRequested=true;impl_->quitting=true;impl_->changed.notify_all();}
void ProgressiveCache::stop(){auto& p=*impl_;p.quitting=true;p.changed.notify_all();for(auto& t:p.workers)if(t.joinable())t.join();p.workers.clear();p.server.stop();
    if(p.directoryLock!=INVALID_HANDLE_VALUE){CloseHandle(p.directoryLock);p.directoryLock=INVALID_HANDLE_VALUE;}}
CacheSnapshot ProgressiveCache::snapshot()const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->state;}
std::string ProgressiveCache::playbackUrl()const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->state.playbackUrl;}
