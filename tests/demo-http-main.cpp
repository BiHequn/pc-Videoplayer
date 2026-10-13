#include "app/demo-http-server.h"
#include <windows.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
namespace {std::atomic<bool> stopped{false};BOOL WINAPI signalHandler(DWORD){stopped=true;return TRUE;}}
int main(int argc,char** argv) {
    if(argc<2){std::cerr<<"native-demo-http fixtures [port] [request-log]\n";return 2;}
    try {
        std::ofstream requestLog;std::mutex logMutex;
        if(argc>3){requestLog.open(std::filesystem::u8path(argv[3]),std::ios::app);if(!requestLog)throw std::runtime_error("Cannot create request log");}
        DemoHttpServer server(std::filesystem::u8path(argv[1]),[&](const LocalHttpRequest& request){if(argc>3){std::lock_guard<std::mutex> lock(logMutex);requestLog<<nlohmann::json{{"path",request.path},{"method",request.method},{"range",request.range}}.dump()<<'\n';requestLog.flush();}});
        const auto port=argc>2?std::stoul(argv[2]):18761;
        if(port>65535||!server.start(static_cast<std::uint16_t>(port)))return 1;
        SetConsoleCtrlHandler(signalHandler,TRUE);
        std::cout<<"Demo FLV: http://127.0.0.1:"<<server.port()<<"/stream.flv\nHLS: http://127.0.0.1:"<<server.port()<<"/hls/index.m3u8\n"<<std::flush;
        while(!stopped)std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server.stop();return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
