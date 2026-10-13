#pragma once
#include "media/loopback-http.h"
#include <atomic>
#include <filesystem>

// Serves only generated demo media under a fixed directory on loopback.
class DemoHttpServer {
public:
    explicit DemoHttpServer(std::filesystem::path root);
    ~DemoHttpServer();
    bool start(std::uint16_t port=0);
    void stop();
    std::uint16_t port()const;
private:
    void serve(const LocalHttpRequest&,LocalHttpResponse&);
    std::filesystem::path root_;
    std::atomic<bool> stopping_{false};
    LoopbackHttp server_;
};
