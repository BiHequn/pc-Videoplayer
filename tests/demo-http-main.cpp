#include "app/demo-http-server.h"
#include <windows.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
namespace {std::atomic<bool> stopped{false};BOOL WINAPI signalHandler(DWORD){stopped=true;return TRUE;}}
int main(int argc,char** argv) {
    if(argc<2){std::cerr<<"native-demo-http fixtures [port]\n";return 2;}
    try {
        DemoHttpServer server(std::filesystem::u8path(argv[1]));const auto port=argc>2?std::stoul(argv[2]):18761;
        if(port>65535||!server.start(static_cast<std::uint16_t>(port)))return 1;
        SetConsoleCtrlHandler(signalHandler,TRUE);
        std::cout<<"Demo FLV: http://127.0.0.1:"<<server.port()<<"/stream.flv\nHLS: http://127.0.0.1:"<<server.port()<<"/hls/index.m3u8\n"<<std::flush;
        while(!stopped)std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server.stop();return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
