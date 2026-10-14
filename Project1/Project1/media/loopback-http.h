#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct LocalHttpRequest { std::string method, path, range; };
class LocalHttpResponse {
public:
    explicit LocalHttpResponse(std::uintptr_t socket): socket_(socket) {}
    bool headers(int status, std::uint64_t length, const std::string& type, const std::string& extra = {});
    bool write(const void* data, std::size_t size);
    bool connected() const;
private: std::uintptr_t socket_;bool headersSent_=false;
};
class LoopbackHttp {
public:
    using Handler = std::function<void(const LocalHttpRequest&, LocalHttpResponse&)>;
    LoopbackHttp(); ~LoopbackHttp();
    bool start(Handler handler, std::uint16_t port = 0);
    void stop();
    std::uint16_t port() const;
private: struct Impl; std::unique_ptr<Impl> impl_;
};
