#pragma once
#include <winsock2.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>

namespace service {
using Json = nlohmann::json;
constexpr std::uint32_t MaxFrameBytes = 2 * 1024 * 1024;
constexpr std::uint32_t UploadChunkBytes = 256 * 1024;
using Deadline = std::chrono::steady_clock::time_point;
void initializeSockets();
class Socket {
public:
    explicit Socket(SOCKET socket = INVALID_SOCKET);
    ~Socket();
    Socket(const Socket&) = delete;
    SOCKET native() const { return socket_; }
    bool interrupted() const { return interrupted_.load(); }
    void interrupt();
private:
    SOCKET socket_;
    std::atomic<bool> interrupted_{false};
};
std::shared_ptr<Socket> connectTcp(const std::string& host, std::uint16_t port,
                                 Deadline deadline);
void writeFrame(Socket& socket, const Json& message, Deadline deadline);
Json readFrame(Socket& socket, Deadline deadline);
std::string hexEncode(const unsigned char* bytes, std::size_t size);
std::string hexDecode(const std::string& hex);
}
