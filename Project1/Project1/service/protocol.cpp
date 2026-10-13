#include "service/protocol.h"
#include <ws2tcpip.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace service {
void initializeSockets() {
    static const bool initialized = [] {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("Winsock initialization failed");
        return true;
    }();
    (void)initialized;
}
Socket::Socket(SOCKET socket) : socket_(socket) {
    if (socket_ != INVALID_SOCKET) {
        u_long enabled = 1;
        if (ioctlsocket(socket_, FIONBIO, &enabled)) {
            closesocket(socket_); socket_ = INVALID_SOCKET;
            throw std::runtime_error("Cannot configure nonblocking socket");
        }
        BOOL noDelay = TRUE;
        setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&noDelay), sizeof(noDelay));
    }
}
Socket::~Socket() { if (socket_ != INVALID_SOCKET) closesocket(socket_); }
void Socket::interrupt() { interrupted_ = true; if (socket_ != INVALID_SOCKET) shutdown(socket_, SD_BOTH); }
namespace {
void ready(Socket& connection, bool writing, Deadline deadline) {
    const auto socket = connection.native();
    for (;;) {
        if (connection.interrupted()) throw std::runtime_error("TCP operation interrupted");
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= Deadline::duration::zero()) throw std::runtime_error("TCP response timeout");
        const auto micros = std::min<std::int64_t>(100000, std::chrono::duration_cast<std::chrono::microseconds>(remaining).count());
        timeval timeout{static_cast<long>(micros / 1000000), static_cast<long>(micros % 1000000)};
        fd_set wanted; FD_ZERO(&wanted); FD_SET(socket, &wanted);
        const int selected = select(0, writing ? nullptr : &wanted, writing ? &wanted : nullptr, nullptr, &timeout);
        if (selected > 0) return;
        if (selected == 0) continue;
        if (WSAGetLastError() != WSAEINTR) throw std::runtime_error("TCP select failed");
    }
}
void transfer(Socket& socket, char* bytes, std::size_t count, bool writing, Deadline deadline) {
    while (count) {
        ready(socket, writing, deadline);
        const int length = static_cast<int>(std::min<std::size_t>(count, 1024 * 1024));
        const int transferred = writing ? ::send(socket.native(), bytes, length, 0)
                                        : ::recv(socket.native(), bytes, length, 0);
        if (transferred > 0) { bytes += transferred; count -= transferred; }
        else if (transferred == 0) throw std::runtime_error("TCP peer disconnected");
        else if (WSAGetLastError() != WSAEWOULDBLOCK && WSAGetLastError() != WSAEINTR)
            throw std::runtime_error("TCP transport failed");
    }
}
}
std::shared_ptr<Socket> connectTcp(const std::string& host, std::uint16_t port, Deadline deadline) {
    initializeSockets();
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    const std::string numeric = host == "localhost" ? "127.0.0.1" : host;
    if (!port || InetPtonA(AF_INET, numeric.c_str(), &address.sin_addr) != 1)
        throw std::runtime_error("Use localhost or a numeric IPv4 address and a nonzero port");
    auto socket = std::make_shared<Socket>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (socket->native() == INVALID_SOCKET) throw std::runtime_error("TCP socket creation failed");
    if (::connect(socket->native(), reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
        const auto error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) throw std::runtime_error("TCP connect failed");
        ready(*socket, true, deadline);
        int connectError = 0; int size = sizeof(connectError);
        if (getsockopt(socket->native(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&connectError), &size) || connectError)
            throw std::runtime_error("TCP connect failed");
    }
    return socket;
}
void writeFrame(Socket& socket, const Json& message, Deadline deadline) {
    auto bytes = message.dump();
    if (bytes.empty() || bytes.size() > MaxFrameBytes) throw std::runtime_error("Frame exceeds 2 MiB");
    auto size = htonl(static_cast<std::uint32_t>(bytes.size()));
    transfer(socket, reinterpret_cast<char*>(&size), 4, true, deadline);
    transfer(socket, bytes.data(), bytes.size(), true, deadline);
}
Json readFrame(Socket& socket, Deadline deadline) {
    std::uint32_t size;
    transfer(socket, reinterpret_cast<char*>(&size), 4, false, deadline);
    size = ntohl(size);
    if (!size || size > MaxFrameBytes) throw std::runtime_error("Invalid frame size");
    std::string bytes(size, '\0'); transfer(socket, bytes.data(), size, false, deadline);
    return Json::parse(bytes);
}
std::string hexEncode(const unsigned char* bytes, std::size_t size) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result(size * 2, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        result[2 * i] = alphabet[bytes[i] >> 4]; result[2 * i + 1] = alphabet[bytes[i] & 15];
    }
    return result;
}
std::string hexDecode(const std::string& hex) {
    if (hex.size() % 2) throw std::invalid_argument("Odd hex size");
    auto digit = [](char c) -> unsigned char {
        if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
        throw std::invalid_argument("Invalid hex");
    };
    std::string bytes(hex.size() / 2, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<char>((digit(hex[i * 2]) << 4) | digit(hex[i * 2 + 1]));
    return bytes;
}
}
