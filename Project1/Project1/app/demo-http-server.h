#pragma once
#include "media/loopback-http.h"
#include <atomic>
#include <filesystem>
#include <functional>

// Serves only generated demo media under a fixed directory on loopback.
class DemoHttpServer {
public:
    explicit DemoHttpServer(std::filesystem::path root,std::function<void(const LocalHttpRequest&)> observer={});
    ~DemoHttpServer();
    bool start(std::uint16_t port=0);
    void stop();
    std::uint16_t port()const;
private:
    void serve(const LocalHttpRequest&,LocalHttpResponse&);
    std::filesystem::path root_;
    std::atomic<bool> stopping_{false};
    LoopbackHttp server_;
    std::function<void(const LocalHttpRequest&)> observer_;
};
