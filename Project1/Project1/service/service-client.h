#pragma once
#include "service/service-types.h"
#include <memory>
#include <string>

// Mutations enqueue background work; snapshots are thread-safe value copies.
class ServiceClient {
public:
    ServiceClient();
    ~ServiceClient();
    ServiceClient(const ServiceClient&) = delete;
    ServiceClient& operator=(const ServiceClient&) = delete;
    void connect(const std::string& host, std::uint16_t port);
    void disconnect();
    void registerAccount(const std::string& username, const std::string& password);
    void login(const std::string& username, const std::string& password);
    void logout();
    void upload(const std::string& path);
    void pauseUpload();
    void resumeUpload();
    void cancelUpload();
    ServiceSnapshot snapshot() const;
    UploadSnapshot uploadSnapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
