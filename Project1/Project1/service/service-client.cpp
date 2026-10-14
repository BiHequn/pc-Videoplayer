#include "service/service-client.h"
#include "service/crypto.h"
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>

using service::Json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
struct ServiceClient::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> stopping{false}, hashCanceled{false};
    std::thread worker;
    ServiceSnapshot status;
    UploadSnapshot transfer;
    std::shared_ptr<service::Socket> socket;
    bool desired = false, paused = false, cancelPending = false, initialized = false;
    std::uint64_t generation = 0, uploadGeneration = 0;
    std::string token;
    struct Command { Json request; std::uint64_t generation; };
    std::deque<Command> commands;
    std::optional<Command> pending;
    struct UploadRequest { Json request; std::uint64_t generation; std::uint64_t uploadGeneration; };
    std::optional<UploadRequest> pendingUpload;
    Clock::time_point heartbeat = Clock::now(), retry = Clock::now(), rateStart = Clock::now();
    unsigned retryStep = 0;
    std::uint64_t rateBytes = 0;
    Impl() { worker = std::thread([this] { run(); }); }
    ~Impl() {
        stopping = true; hashCanceled = true;
        { std::lock_guard<std::mutex> lock(mutex); if (socket) socket->interrupt(); }
        wake.notify_all(); if (worker.joinable()) worker.join();
    }
    Json request(const std::string& op, Json body = Json::object()) {
        return {{"version", 1}, {"id", service::randomHex(16)}, {"op", op}, {"body", std::move(body)}};
    }
    Json exchange(const Json& requestValue, std::uint64_t expectedGeneration) {
        std::shared_ptr<service::Socket> current;
        { std::lock_guard<std::mutex> lock(mutex);
          if (expectedGeneration != generation || !desired || !socket) throw std::runtime_error("Connection changed");
          current = socket; }
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        service::writeFrame(*current, requestValue, deadline);
        auto result = service::readFrame(*current, deadline);
        if (!result.is_object() || result.value("version", 0) != 1 || result.at("id") != requestValue.at("id") ||
            result.at("op") != requestValue.at("op") || !result.at("ok").is_boolean() ||
            !result.at("code").is_string() || !result.at("body").is_object())
            throw std::runtime_error("Invalid correlated service response");
        return result;
    }
    void lost(const std::string& message, std::uint64_t expectedGeneration) {
        std::lock_guard<std::mutex> lock(mutex);
        if (expectedGeneration != generation) return;
        if (socket) socket->interrupt(); socket.reset(); initialized = false;
        status.connected = false;
        status.state = desired ? ServiceState::Reconnecting : ServiceState::Disconnected;
        status.code = "TRANSPORT_ERROR"; status.error = message;
        if (transfer.state == UploadState::Uploading || transfer.state == UploadState::Hashing || transfer.state == UploadState::Reconnecting)
            transfer.state = paused ? UploadState::Paused : UploadState::Reconnecting;
        retry = Clock::now() + std::chrono::seconds(1u << std::min(retryStep, 2u));
        if (retryStep < 2) ++retryStep;
    }
    void restoreConnection(std::uint64_t epoch, const std::string& host, std::uint16_t port) {
        auto opened = service::connectTcp(host, port, Clock::now() + std::chrono::seconds(5));
        std::string sessionToken;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping || epoch != generation || !desired) { opened->interrupt(); return; }
            socket = opened; sessionToken = token;
        }
        if (!sessionToken.empty()) {
            auto result = exchange(request("session.restore", {{"token", sessionToken}}), epoch);
            std::lock_guard<std::mutex> lock(mutex);
            if (epoch != generation) return;
            if (result.at("ok") == true) {
                status.authenticated = true; status.username = result.at("body").at("username");
            } else {
                token.clear(); status.authenticated = false; status.username.clear();
                status.code = result.at("code"); status.error = status.code;
                if (transfer.state == UploadState::Reconnecting || transfer.state == UploadState::Paused) {
                    transfer.state = UploadState::Failed; transfer.code = "UNAUTHORIZED"; transfer.error = "Session expired";
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (epoch != generation || !desired) return;
            status.connected = true; status.state = ServiceState::Connected;
            if (sessionToken.empty() || status.authenticated) { status.error.clear(); status.code = "OK"; }
            retryStep = 0; heartbeat = Clock::now(); initialized = false;
            if (transfer.state == UploadState::Reconnecting) transfer.state = paused ? UploadState::Paused : UploadState::Uploading;
        }
    }
    void enqueue(const std::string& op, Json body) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!desired) { status.code = "DISCONNECTED"; status.error = "Connect to the service first"; return; }
        if (commands.size() >= 64) { status.code = "BUSY"; status.error = "Service command queue is full"; return; }
        commands.push_back({request(op, std::move(body)), generation});
        status.busy = true; status.error.clear(); status.code.clear(); wake.notify_all();
    }
    void handleCommand(const Command& command) {
        auto wire = command.request;
        if (wire.at("op") == "logout") {
            std::lock_guard<std::mutex> lock(mutex); wire["body"]["token"] = token;
        }
        auto result = exchange(wire, command.generation);
        std::lock_guard<std::mutex> lock(mutex);
        if (command.generation != generation) return;
        status.code = result.at("code"); status.error = result.at("ok") == true ? "" : status.code;
        if (wire.at("op") == "login" && result.at("ok") == true) {
            token = result.at("body").at("token"); status.username = result.at("body").at("username"); status.authenticated = true;
        }
        if (wire.at("op") == "logout" && (result.at("ok") == true || result.at("code") == "UNAUTHORIZED")) {
            token.clear(); status.username.clear(); status.authenticated = false;
            if (transfer.state == UploadState::Uploading || transfer.state == UploadState::Paused || transfer.state == UploadState::Hashing) {
                transfer.state = UploadState::Failed; transfer.code = "UNAUTHORIZED"; transfer.error = "Logged out";
            }
        }
        pending.reset(); status.busy = !commands.empty();
    }
    void failUpload(const std::string& code, const std::string& message, std::uint64_t uploadEpoch) {
        std::lock_guard<std::mutex> lock(mutex);
        if (uploadEpoch != uploadGeneration) return;
        transfer.state = UploadState::Failed; transfer.code = code; transfer.error = message;
        initialized = false; cancelPending = false;
    }
    bool applyUpload(const Json& result, std::uint64_t epoch, std::uint64_t uploadEpoch) {
        std::lock_guard<std::mutex> lock(mutex);
        if (epoch != generation || uploadEpoch != uploadGeneration) return false;
        if (result.at("ok") != true) {
            const auto code = result.at("code").get<std::string>();
            if (code == "OFFSET_MISMATCH" && result.at("body").contains("offset")) {
                transfer.uploadedBytes = result.at("body").at("offset"); return true;
            }
            const auto message = result.at("body").value("message", code);
            if (code == "UNAUTHORIZED") {
                token.clear(); status.authenticated = false; status.username.clear();
                status.code = code; status.error = message;
                initialized = false; pendingUpload.reset();
            }
            transfer.state = UploadState::Failed; transfer.code = code; transfer.error = message; cancelPending = false; return false;
        }
        const auto& body = result.at("body");
        transfer.uploadId = body.at("uploadId"); transfer.uploadedBytes = body.at("offset");
        transfer.progress = transfer.totalBytes ? static_cast<double>(transfer.uploadedBytes) / static_cast<double>(transfer.totalBytes) : 0;
        if (body.at("state") == "completed") {
            transfer.state = UploadState::Completed; transfer.progress = 1; transfer.serverPath = body.at("path"); cancelPending = false;
        } else if (body.at("state") == "canceled") { transfer.state = UploadState::Canceled; cancelPending = false; }
        else transfer.state = paused ? UploadState::Paused : (cancelPending ? UploadState::Canceled : UploadState::Uploading);
        transfer.code = "OK"; transfer.error.clear();
        return true;
    }
    void uploadStep(std::uint64_t epoch, std::uint64_t uploadEpoch) {
        UploadSnapshot current;
        std::string sessionToken;
        bool needsInit, cancellation;
        {
            std::lock_guard<std::mutex> lock(mutex);
            current = transfer; sessionToken = token; needsInit = !initialized; cancellation = cancelPending;
            if (!status.authenticated) {
                transfer.state = UploadState::Failed; transfer.code = "UNAUTHORIZED"; transfer.error = "Login before uploading";
                cancelPending = false; return;
            }
        }
        auto exchangeUpload = [&](Json next) {
            Json retained;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (pendingUpload && (pendingUpload->generation != epoch || pendingUpload->uploadGeneration != uploadEpoch)) pendingUpload.reset();
                if (!pendingUpload) pendingUpload = UploadRequest{std::move(next), epoch, uploadEpoch};
                retained = pendingUpload->request;
            }
            auto response = exchange(retained, epoch);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (pendingUpload && pendingUpload->generation == epoch && pendingUpload->uploadGeneration == uploadEpoch) pendingUpload.reset();
            }
            return response;
        };
        if (cancellation) {
            // Cancellation supersedes any previously interrupted chunk/init request.
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (pendingUpload && pendingUpload->request.at("op") != "upload.cancel") pendingUpload.reset();
            }
            if (!current.uploadId.empty()) {
                auto result = exchangeUpload(request("upload.cancel", {{"token", sessionToken}, {"uploadId", current.uploadId}}));
                applyUpload(result, epoch, uploadEpoch);
                return;
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (epoch == generation && uploadEpoch == uploadGeneration) { cancelPending = false; transfer.state = UploadState::Canceled; }
            return;
        }
        if (current.sha256.empty()) {
            try {
                const auto path = fs::u8path(current.localPath);
                if (!fs::is_regular_file(path)) throw std::runtime_error("Upload source is not a regular file");
                const auto total = fs::file_size(path);
                const auto hash = service::sha256File(path, &hashCanceled);
                std::lock_guard<std::mutex> lock(mutex);
                if (uploadEpoch != uploadGeneration || epoch != generation || cancelPending) return;
                transfer.sha256 = hash; transfer.totalBytes = total;
                transfer.state = paused ? UploadState::Paused : UploadState::Uploading;
                rateStart = Clock::now(); rateBytes = 0;
            } catch (const std::exception& error) {
                if (!hashCanceled) failUpload("FILE_ERROR", error.what(), uploadEpoch);
            }
            return;
        }
        if (needsInit) {
            // This query runs before retrying an interrupted block and uses the server's durable offset.
            auto result = exchange(request("upload.init", {{"token", sessionToken}, {"filename", current.filename},
                {"size", current.totalBytes}, {"sha256", current.sha256}}), epoch);
            if (applyUpload(result, epoch, uploadEpoch)) {
                std::lock_guard<std::mutex> lock(mutex);
                if (epoch == generation && uploadEpoch == uploadGeneration) {
                    initialized = true; rateStart = Clock::now(); rateBytes = transfer.uploadedBytes;
                }
            }
            return;
        }
        std::optional<UploadRequest> retryUpload;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (pendingUpload && pendingUpload->generation == epoch && pendingUpload->uploadGeneration == uploadEpoch) retryUpload = pendingUpload;
        }
        if (retryUpload) {
            applyUpload(exchangeUpload(retryUpload->request), epoch, uploadEpoch);
            return;
        }
        if (current.uploadedBytes == current.totalBytes) {
            applyUpload(exchangeUpload(request("upload.finish", {{"token", sessionToken}, {"uploadId", current.uploadId}})), epoch, uploadEpoch);
            return;
        }
        if (current.uploadedBytes > current.totalBytes) { failUpload("BAD_OFFSET", "Server offset exceeds source size", uploadEpoch); return; }
        std::ifstream input(fs::u8path(current.localPath), std::ios::binary);
        input.seekg(static_cast<std::streamoff>(current.uploadedBytes));
        std::string bytes(static_cast<std::size_t>(std::min<std::uint64_t>(service::UploadChunkBytes, current.totalBytes - current.uploadedBytes)), '\0');
        input.read(bytes.data(), bytes.size());
        std::error_code fileError;
        const auto sourceSize = fs::file_size(fs::u8path(current.localPath), fileError);
        if (!input || fileError || sourceSize != current.totalBytes) {
            failUpload("FILE_CHANGED", "Upload source changed or cannot be read", uploadEpoch); return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (paused || cancelPending || epoch != generation || uploadEpoch != uploadGeneration) return;
        }
        auto result = exchangeUpload(request("upload.chunk", {{"token", sessionToken}, {"uploadId", current.uploadId},
            {"offset", current.uploadedBytes}, {"sha256", service::sha256(bytes)},
            {"data", service::hexEncode(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size())}}));
        if (applyUpload(result, epoch, uploadEpoch)) {
            std::lock_guard<std::mutex> lock(mutex);
            const auto seconds = std::chrono::duration<double>(Clock::now() - rateStart).count();
            transfer.bytesPerSecond = seconds > 0 && transfer.uploadedBytes >= rateBytes ?
                static_cast<double>(transfer.uploadedBytes - rateBytes) / seconds : 0;
        }
    }
    void run() {
        while (!stopping) {
            std::uint64_t epoch, uploadEpoch;
            bool connected, wantConnection, uploadWork;
            std::string host; std::uint16_t port;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (pending && pending->generation != generation) pending.reset();
                if (pendingUpload && (pendingUpload->generation != generation || pendingUpload->uploadGeneration != uploadGeneration)) pendingUpload.reset();
                if (!pending && !commands.empty()) { pending = std::move(commands.front()); commands.pop_front(); }
                if (paused && transfer.state == UploadState::Uploading) transfer.state = UploadState::Paused;
                epoch = generation; uploadEpoch = uploadGeneration;
                wantConnection = desired; connected = status.connected;
                host = status.host; port = status.port;
                uploadWork = cancelPending || (!paused && (transfer.state == UploadState::Hashing || transfer.state == UploadState::Uploading || transfer.state == UploadState::Reconnecting));
                if (!wantConnection || (!connected && Clock::now() < retry) ||
                    (connected && !pending && !uploadWork && Clock::now() - heartbeat < std::chrono::seconds(2))) {
                    wake.wait_for(lock, std::chrono::milliseconds(100)); continue;
                }
                if (!connected && status.state == ServiceState::Reconnecting) ++status.reconnectAttempts;
            }
            try {
                if (!connected) { restoreConnection(epoch, host, port); continue; }
                std::optional<Command> command;
                { std::lock_guard<std::mutex> lock(mutex); command = pending; }
                if (command) { handleCommand(*command); continue; }
                if (Clock::now() - heartbeat >= std::chrono::seconds(2)) {
                    exchange(request("heartbeat"), epoch); heartbeat = Clock::now(); continue;
                }
                if (uploadWork) uploadStep(epoch, uploadEpoch);
            } catch (const std::exception& error) { lost(error.what(), epoch); }
            catch (...) { lost("Service worker failed", epoch); }
        }
    }
};
ServiceClient::ServiceClient() : impl_(new Impl) {}
ServiceClient::~ServiceClient() = default;
void ServiceClient::connect(const std::string& host, std::uint16_t port) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->socket) impl_->socket->interrupt(); impl_->socket.reset();
    impl_->hashCanceled = true; ++impl_->generation; impl_->commands.clear();
    impl_->status = {}; impl_->status.host = host; impl_->status.port = port; impl_->status.state = ServiceState::Connecting;
    impl_->token.clear(); impl_->desired = true; impl_->retry = Clock::now(); impl_->retryStep = 0;
    impl_->initialized = false;
    if (impl_->transfer.state == UploadState::Uploading || impl_->transfer.state == UploadState::Hashing) {
        impl_->paused = true; impl_->transfer.state = UploadState::Paused;
    }
    impl_->wake.notify_all();
}
void ServiceClient::disconnect() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->socket) impl_->socket->interrupt(); impl_->socket.reset();
    ++impl_->generation; impl_->commands.clear(); impl_->desired = false; impl_->hashCanceled = true;
    impl_->status.connected = false; impl_->status.state = ServiceState::Disconnected; impl_->status.busy = false;
    impl_->status.authenticated = false; impl_->status.username.clear(); impl_->token.clear();
    // Explicit disconnect leaves resumable disk state but requires a fresh login after connect().
    if (impl_->transfer.state == UploadState::Uploading || impl_->transfer.state == UploadState::Hashing || impl_->transfer.state == UploadState::Reconnecting) {
        impl_->paused = true; impl_->transfer.state = UploadState::Paused;
    }
    impl_->wake.notify_all();
}
void ServiceClient::registerAccount(const std::string& username, const std::string& password) { impl_->enqueue("register", {{"username", username}, {"password", password}}); }
void ServiceClient::login(const std::string& username, const std::string& password) { impl_->enqueue("login", {{"username", username}, {"password", password}}); }
void ServiceClient::logout() { impl_->enqueue("logout", Json::object()); }
void ServiceClient::upload(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto state = impl_->transfer.state;
    if (state == UploadState::Uploading || state == UploadState::Hashing || state == UploadState::Paused || state == UploadState::Reconnecting || impl_->cancelPending) {
        impl_->transfer.code = "BUSY"; impl_->transfer.error = "Finish or cancel the current upload first"; return;
    }
    ++impl_->uploadGeneration; impl_->transfer = {}; impl_->transfer.localPath = path;
    try { impl_->transfer.filename = fs::u8path(path).filename().u8string(); }
    catch (...) { impl_->transfer.state = UploadState::Failed; impl_->transfer.code = "FILE_ERROR"; return; }
    impl_->hashCanceled = false; impl_->paused = false; impl_->cancelPending = false; impl_->initialized = false;
    impl_->transfer.state = UploadState::Hashing; impl_->wake.notify_all();
}
void ServiceClient::pauseUpload() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->transfer.state == UploadState::Uploading || impl_->transfer.state == UploadState::Hashing || impl_->transfer.state == UploadState::Reconnecting) {
        impl_->paused = true;
        // The worker sets Paused only after the outstanding block's durable acknowledgement.
        if (impl_->transfer.state != UploadState::Uploading) impl_->transfer.state = UploadState::Paused;
        impl_->wake.notify_all();
    }
}
void ServiceClient::resumeUpload() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->transfer.state == UploadState::Paused) {
        impl_->paused = false; impl_->hashCanceled = false; impl_->initialized = false;
        impl_->transfer.state = impl_->status.connected ? (impl_->transfer.sha256.empty() ? UploadState::Hashing : UploadState::Uploading) : UploadState::Reconnecting;
        impl_->wake.notify_all();
    }
}
void ServiceClient::cancelUpload() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->transfer.state == UploadState::Completed || impl_->transfer.state == UploadState::Idle) return;
    impl_->hashCanceled = true; impl_->paused = false; impl_->cancelPending = true;
    impl_->transfer.state = UploadState::Canceled; impl_->wake.notify_all();
}
ServiceSnapshot ServiceClient::snapshot() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->status; }
UploadSnapshot ServiceClient::uploadSnapshot() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->transfer; }
