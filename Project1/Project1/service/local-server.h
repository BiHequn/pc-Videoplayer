#pragma once
#include <cstdint>
#include <memory>
#include <string>

class LocalServer {
public:
    LocalServer(std::string dataDirectory, std::uint16_t port = 18760);
    ~LocalServer();
    LocalServer(const LocalServer&) = delete;
    LocalServer& operator=(const LocalServer&) = delete;
    bool start();
    void stop();
    std::uint16_t port() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
