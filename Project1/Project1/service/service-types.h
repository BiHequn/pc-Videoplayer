#pragma once
#include <cstdint>
#include <string>

enum class ServiceState { Disconnected, Connecting, Connected, Reconnecting };
enum class UploadState { Idle, Hashing, Uploading, Paused, Reconnecting, Completed, Canceled, Failed };

struct ServiceSnapshot {
    ServiceState state = ServiceState::Disconnected;
    bool connected = false;
    bool authenticated = false;
    bool busy = false;
    std::string host;
    std::uint16_t port = 0;
    std::string username;
    std::string error;
    std::string code;
    std::uint64_t reconnectAttempts = 0;
};
struct UploadSnapshot {
    UploadState state = UploadState::Idle;
    std::string localPath;
    std::string filename;
    std::string uploadId;
    std::string sha256;
    std::uint64_t totalBytes = 0;
    std::uint64_t uploadedBytes = 0;
    double bytesPerSecond = 0;
    double progress = 0;
    std::string error;
    std::string code;
    std::string serverPath;
};
