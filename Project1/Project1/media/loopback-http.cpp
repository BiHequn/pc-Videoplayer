#include <winsock2.h>
#include <ws2tcpip.h>
#include "loopback-http.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <cctype>

bool LocalHttpResponse::write(const void* data,std::size_t size) {
    const auto* p=static_cast<const char*>(data);while(size) {const int n=send(SOCKET(socket_),p,int(std::min<std::size_t>(size,65536)),0);if(n<=0)return false;p+=n;size-=n;}return true;
}
bool LocalHttpResponse::headers(int status,std::uint64_t length,const std::string& type,const std::string& extra) {
    if(headersSent_)return false;headersSent_=true;
    const std::string text="HTTP/1.1 "+std::to_string(status)+" "+(status==200?"OK":status==206?"Partial Content":"Error")+"\r\nContent-Length: "+std::to_string(length)+"\r\nContent-Type: "+type+"\r\nConnection: close\r\n"+extra+"\r\n";return write(text.data(),text.size());
}
bool LocalHttpResponse::connected()const {
    fd_set set;FD_ZERO(&set);FD_SET(SOCKET(socket_),&set);timeval timeout{0,0};
    const auto ready=select(0,&set,nullptr,nullptr,&timeout);if(ready==SOCKET_ERROR)return false;if(!ready)return true;
    char byte;return recv(SOCKET(socket_),&byte,1,MSG_PEEK)>0;
}
struct LoopbackHttp::Impl {
    SOCKET listener=INVALID_SOCKET;std::atomic<bool> running{false};std::uint16_t port=0;
    struct Client {std::thread thread;std::shared_ptr<std::atomic<bool>> done;};
    std::thread acceptThread;std::vector<Client> clients;std::vector<SOCKET> sockets;std::mutex mutex;Handler handler;bool initialized=false;
};
LoopbackHttp::LoopbackHttp():impl_(new Impl){} LoopbackHttp::~LoopbackHttp(){stop();}
bool LoopbackHttp::start(Handler handler,std::uint16_t port) {
    stop();auto& p=*impl_;WSADATA w{};if(WSAStartup(MAKEWORD(2,2),&w))return false;p.initialized=true;
    p.listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons(port);
    if(p.listener==INVALID_SOCKET||bind(p.listener,reinterpret_cast<sockaddr*>(&a),sizeof(a))||listen(p.listener,16)){stop();return false;}
    int len=sizeof(a);getsockname(p.listener,reinterpret_cast<sockaddr*>(&a),&len);p.port=ntohs(a.sin_port);p.handler=std::move(handler);p.running=true;
    p.acceptThread=std::thread([&p,listener=p.listener] {while(p.running) {
        fd_set readable;FD_ZERO(&readable);FD_SET(listener,&readable);timeval timeoutAccept{0,100000};
        const auto ready=select(0,&readable,nullptr,nullptr,&timeoutAccept);
        {std::lock_guard<std::mutex> lock(p.mutex);for(auto it=p.clients.begin();it!=p.clients.end();) {
            if(*it->done){it->thread.join();it=p.clients.erase(it);}else ++it;
        }}
        if(!p.running)break;if(ready<=0)continue;
        SOCKET s=accept(listener,nullptr,nullptr);if(s==INVALID_SOCKET)break;DWORD timeout=3000;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
        std::lock_guard<std::mutex> lock(p.mutex);if(!p.running){closesocket(s);break;}
        if(p.clients.size()>=64){closesocket(s);continue;}p.sockets.push_back(s);
        auto done=std::make_shared<std::atomic<bool>>(false);
        std::thread client([&p,s,done] {
            std::string text;char b[1024];while(text.size()<16384&&text.find("\r\n\r\n")==std::string::npos){int n=recv(s,b,sizeof(b),0);if(n<=0)break;text.append(b,n);}
            LocalHttpResponse out(s);try {
                LocalHttpRequest r;const auto first=text.find(' '),second=text.find(' ',first+1);if(text.find("\r\n\r\n")==std::string::npos||first==std::string::npos||second==std::string::npos)out.headers(400,0,"text/plain");
                else {r.method=text.substr(0,first);r.path=text.substr(first+1,second-first-1);auto lower=text;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return char(std::tolower(c));});const auto range=lower.find("\r\nrange:");if(range!=std::string::npos){const auto start=lower.find_first_not_of(" \t",range+8);const auto end=lower.find("\r\n",start);r.range=text.substr(start,end-start);}p.handler(r,out);}
            }catch(...){out.headers(500,0,"text/plain");}
            {std::lock_guard<std::mutex> lock(p.mutex);auto it=std::find(p.sockets.begin(),p.sockets.end(),s);if(it!=p.sockets.end())p.sockets.erase(it);closesocket(s);}
            *done=true;
        });
        p.clients.push_back({std::move(client),done});
    }});return true;
}
void LoopbackHttp::stop() {
    auto& p=*impl_;p.running=false;if(p.listener!=INVALID_SOCKET){shutdown(p.listener,SD_BOTH);closesocket(p.listener);p.listener=INVALID_SOCKET;}if(p.acceptThread.joinable())p.acceptThread.join();
    {std::lock_guard<std::mutex> lock(p.mutex);for(auto s:p.sockets)shutdown(s,SD_BOTH);}
    for(auto& client:p.clients)if(client.thread.joinable())client.thread.join();p.clients.clear();p.sockets.clear();if(p.initialized){WSACleanup();p.initialized=false;}
}
std::uint16_t LoopbackHttp::port()const{return impl_->port;}
