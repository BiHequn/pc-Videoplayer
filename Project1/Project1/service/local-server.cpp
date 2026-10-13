#include "service/local-server.h"
#include "service/crypto.h"
#include <Windows.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using service::Json;
namespace fs = std::filesystem;
namespace {
// A share-denied OS handle protects all records, across ports and processes.
// The file remains on disk; closing the handle releases ownership after a crash.
struct DirectoryLease {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit DirectoryLease(const fs::path& directory) {
        const auto path = directory / ".service.lock";
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Service data directory is already in use or inaccessible");
    }
    ~DirectoryLease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    DirectoryLease(const DirectoryLease&) = delete;
    DirectoryLease& operator=(const DirectoryLease&) = delete;
};
Json reply(const Json& request, bool ok, std::string code, Json body = Json::object()) {
    return {{"version", 1}, {"id", request.value("id", std::string())},
            {"op", request.value("op", std::string())}, {"ok", ok}, {"code", code}, {"body", body}};
}
bool validCredentials(const std::string& username, const std::string& password) {
    return !username.empty() && username.size() <= 64 && !password.empty() && password.size() <= 1024 &&
        std::all_of(username.begin(), username.end(), [](unsigned char c) { return c >= 32 && c != 127; });
}
bool validHash(const std::string& hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool unsignedNumber(const Json& value) {
    return value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0);
}
std::string safeFilename(std::string name) {
    const auto last = name.find_last_of("/\\");
    if (last != std::string::npos) name.erase(0, last + 1);
    for (auto& c : name) if (static_cast<unsigned char>(c) < 32 || std::string("<>:\"/\\|?*").find(c) != std::string::npos) c = '_';
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty() || name == "." || name == "..") name = "upload.bin";
    // Only display metadata uses this value; every disk path uses generated hex identifiers.
    return name;
}
void durableBlock(const fs::path& path, std::uint64_t offset, const std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open upload partial");
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD written = 0;
    const bool ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) throw std::runtime_error("Cannot durably write upload block");
}
}
struct LocalServer::Impl {
    fs::path root;
    std::uint16_t requestedPort;
    std::atomic<std::uint16_t> boundPort{0};
    std::atomic<bool> stopping{true};
    std::shared_ptr<service::Socket> listener;
    std::unique_ptr<DirectoryLease> directoryLease;
    std::thread acceptThread;
    std::mutex peersMutex, recordsMutex;
    struct Peer {
        std::shared_ptr<service::Socket> socket;
        std::thread thread;
        std::atomic<bool> done{false};
    };
    std::vector<std::shared_ptr<Peer>> peers;
    Json accounts = Json::object(), sessions = Json::object();
    struct Replay { std::string key; Json result; };
    std::deque<Replay> replay;
    Impl(std::string directory, std::uint16_t port) : root(fs::u8path(directory)), requestedPort(port) {}
    Json uploadRequest(const Json& request, const std::string& user) {
        const auto& body = request.at("body");
        const auto op = request.at("op").get<std::string>();
        const auto userId = service::sha256(user);
        const auto directory = root / "uploads" / userId;
        std::string id;
        if (op == "upload.init") {
            const auto hash = body.value("sha256", std::string());
            if (!body.contains("size") || !unsignedNumber(body.at("size")) ||
                body.at("size").get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX) ||
                !validHash(hash) || !body.contains("filename") || !body["filename"].is_string() ||
                body["filename"].get_ref<const std::string&>().size() > 1024)
                return reply(request, false, "INVALID_UPLOAD");
            const auto size = body.at("size").get<std::uint64_t>();
            id = service::sha256(hash + ":" + std::to_string(size));
            fs::create_directories(directory);
            if (!fs::exists(directory / (id + ".json"))) {
                Json task{{"uploadId", id}, {"sha256", hash}, {"size", size}, {"offset", 0},
                          {"filename", safeFilename(body.at("filename"))}, {"state", "uploading"}};
                durableBlock(directory / (id + ".part"), 0, "");
                service::atomicWrite(directory / (id + ".json"), task.dump(2));
            }
        } else {
            id = body.value("uploadId", std::string());
            if (!validHash(id)) return reply(request, false, "NOT_FOUND");
        }
        const auto metadata = directory / (id + ".json");
        if (!fs::exists(metadata)) return reply(request, false, "NOT_FOUND");
        auto task = service::loadRecord(metadata);
        const auto partial = directory / (id + ".part");
        const fs::path relative = fs::path("files") / userId / (id + ".bin");
        const auto published = root / relative;
        // Recover the window after atomic publication but before completed metadata was saved.
        if (task.at("state") == "uploading" && !fs::exists(partial) && fs::exists(published) &&
            fs::file_size(published) == task.at("size").get<std::uint64_t>() &&
            service::sha256File(published, &stopping) == task.at("sha256")) {
            task["state"] = "completed"; task["offset"] = task.at("size");
            service::atomicWrite(metadata, task.dump(2));
        }
        const auto offset = task.at("offset").get<std::uint64_t>();
        const auto total = task.at("size").get<std::uint64_t>();
        auto resultBody = [&] {
            Json result{{"uploadId", id}, {"offset", task.at("offset")}, {"size", total},
                        {"sha256", task.at("sha256")}, {"filename", task.at("filename")}, {"state", task.at("state")}};
            if (task.at("state") == "completed") result["path"] = relative.generic_u8string();
            return result;
        };
        if (op == "upload.cancel") {
            if (task.at("state") != "completed") {
                task["state"] = "canceled";
                service::atomicWrite(metadata, task.dump(2));
                std::error_code ignored; fs::remove(partial, ignored);
            }
            return reply(request, true, "OK", resultBody());
        }
        if (task.at("state") == "canceled") {
            if (op != "upload.init") return reply(request, false, "CANCELED", resultBody());
            task["state"] = "uploading"; task["offset"] = 0;
            durableBlock(partial, 0, "");
            service::atomicWrite(metadata, task.dump(2));
            return reply(request, true, "OK", resultBody());
        }
        if (task.at("state") == "uploading") {
            if (!fs::exists(partial) || fs::file_size(partial) < offset) throw std::runtime_error("Upload checkpoint exceeds durable bytes");
            if (fs::file_size(partial) > offset) fs::resize_file(partial, offset);
        }
        if (op == "upload.init" || op == "upload.status") return reply(request, true, "OK", resultBody());
        if (op == "upload.chunk") {
            if (!body.contains("offset") || !unsignedNumber(body.at("offset"))) return reply(request, false, "INVALID_CHUNK");
            const auto at = body.at("offset").get<std::uint64_t>();
            const auto dataHex = body.value("data", std::string());
            const auto hash = body.value("sha256", std::string());
            if (dataHex.empty() || dataHex.size() > service::UploadChunkBytes * 2 || dataHex.size() % 2 || !validHash(hash))
                return reply(request, false, "INVALID_CHUNK");
            std::string bytes;
            try { bytes = service::hexDecode(dataHex); } catch (...) { return reply(request, false, "INVALID_CHUNK"); }
            if (service::sha256(bytes) != hash) return reply(request, false, "HASH_MISMATCH");
            if (at > total || bytes.size() > total - at) return reply(request, false, "INVALID_CHUNK");
            if (at < offset) {
                if (bytes.size() > offset - at) return reply(request, false, "OFFSET_MISMATCH", resultBody());
                std::ifstream input(task.at("state") == "completed" ? published : partial, std::ios::binary);
                input.seekg(static_cast<std::streamoff>(at));
                std::string existing(bytes.size(), '\0'); input.read(existing.data(), existing.size());
                if (!input || existing != bytes) return reply(request, false, "CHUNK_CONFLICT", resultBody());
                return reply(request, true, "OK", resultBody());
            }
            if (at != offset) return reply(request, false, "OFFSET_MISMATCH", resultBody());
            if (task.at("state") == "completed") return reply(request, false, "ALREADY_COMPLETED", resultBody());
            durableBlock(partial, offset, bytes);
            task["offset"] = offset + bytes.size();
            service::atomicWrite(metadata, task.dump(2));
            return reply(request, true, "OK", resultBody());
        }
        if (op == "upload.finish") {
            if (task.at("state") == "completed") return reply(request, true, "OK", resultBody());
            if (offset != total) return reply(request, false, "INCOMPLETE", resultBody());
            if (service::sha256File(partial, &stopping) != task.at("sha256")) return reply(request, false, "HASH_MISMATCH", resultBody());
            fs::create_directories(published.parent_path());
            if (!MoveFileExW(partial.c_str(), published.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot publish completed upload");
            task["state"] = "completed";
            service::atomicWrite(metadata, task.dump(2));
            return reply(request, true, "OK", resultBody());
        }
        return reply(request, false, "UNKNOWN_OP");
    }
    Json dispatch(const Json& request) {
        if (!request.is_object() || !request.contains("version") || !request["version"].is_number_integer() ||
            !request.contains("id") || !request["id"].is_string() || request["id"].get_ref<const std::string&>().empty() ||
            request["id"].get_ref<const std::string&>().size() > 128 || !request.contains("op") || !request["op"].is_string() ||
            !request.contains("body") || !request["body"].is_object())
            return {{"version", 1}, {"id", ""}, {"op", ""}, {"ok", false}, {"code", "BAD_REQUEST"}, {"body", Json::object()}};
        if (request["version"] != 1) return reply(request, false, "BAD_VERSION");
        const auto op = request.at("op").get<std::string>();
        const auto& body = request.at("body");
        if (op == "heartbeat") return reply(request, true, "OK");
        if (op == "register" || op == "login") {
            const auto user = body.value("username", std::string());
            const auto password = body.value("password", std::string());
            if (!validCredentials(user, password)) return reply(request, false, "INVALID_CREDENTIALS");
            const auto key = service::sha256(request.dump());
            {
                std::lock_guard<std::mutex> lock(recordsMutex);
                for (const auto& entry : replay) if (entry.key == key) return entry.result;
            }
            Json account;
            {
                std::lock_guard<std::mutex> lock(recordsMutex);
                if (accounts.contains(user)) account = accounts.at(user);
            }
            if (op == "register") {
                if (!account.is_null()) {
                    if (account.value("requestId", std::string()) == request.at("id") &&
                        service::constantEqual(service::passwordHash(password, account.at("salt")), account.at("hash")))
                        return reply(request, true, "OK", {{"username", user}});
                    return reply(request, false, "ACCOUNT_EXISTS");
                }
                const auto salt = service::randomHex(16);
                const auto hash = service::passwordHash(password, salt);
                std::lock_guard<std::mutex> lock(recordsMutex);
                if (accounts.contains(user)) return reply(request, false, "ACCOUNT_EXISTS");
                auto updated = accounts;
                updated[user] = {{"salt", salt}, {"hash", hash}, {"iterations", service::PasswordIterations}, {"requestId", request.at("id")}};
                service::atomicWrite(root / "accounts.json", updated.dump(2)); accounts = std::move(updated);
                auto result = reply(request, true, "OK", {{"username", user}});
                replay.push_back({key, result}); if (replay.size() > 128) replay.pop_front();
                return result;
            }
            // Unknown users do the same expensive derivation as known users.
            const auto salt = account.is_null() ? std::string(32, '0') : account.at("salt").get<std::string>();
            const auto hash = service::passwordHash(password, salt);
            if (account.is_null() || !service::constantEqual(hash, account.at("hash"))) return reply(request, false, "BAD_CREDENTIALS");
            const auto token = service::randomHex(32);
            std::lock_guard<std::mutex> lock(recordsMutex);
            for (auto session = sessions.begin(); session != sessions.end(); ++session) {
                if (session.value().is_object() && session.value().value("username", std::string()) == user &&
                    session.value().value("requestId", std::string()) == request.at("id"))
                    return reply(request, true, "OK", {{"username", user}, {"token", session.key()}});
            }
            auto updated = sessions; updated[token] = {{"username", user}, {"requestId", request.at("id")}};
            service::atomicWrite(root / "sessions.json", updated.dump(2)); sessions = std::move(updated);
            auto result = reply(request, true, "OK", {{"username", user}, {"token", token}});
            replay.push_back({key, result}); if (replay.size() > 128) replay.pop_front();
            return result;
        }
        if (op == "session.restore" || op == "logout" || op.rfind("upload.", 0) == 0) {
            std::lock_guard<std::mutex> lock(recordsMutex);
            const auto token = body.value("token", std::string());
            if (!sessions.contains(token)) return reply(request, false, "UNAUTHORIZED");
            const auto& session = sessions.at(token);
            const auto user = session.is_string() ? session.get<std::string>() : session.at("username").get<std::string>();
            if (op == "session.restore") return reply(request, true, "OK", {{"username", user}});
            if (op == "logout") {
                auto updated = sessions; updated.erase(token);
                service::atomicWrite(root / "sessions.json", updated.dump(2)); sessions = std::move(updated);
                replay.clear();
                return reply(request, true, "OK");
            }
            return uploadRequest(request, user);
        }
        return reply(request, false, "UNKNOWN_OP");
    }
    void serve(const std::shared_ptr<Peer>& peer) {
        while (!stopping) {
            try {
                auto request = service::readFrame(*peer->socket, std::chrono::steady_clock::now() + std::chrono::seconds(5));
                Json response;
                try { response = dispatch(request); }
                catch (const Json::exception&) { response = reply(request, false, "BAD_REQUEST"); }
                catch (const std::exception& e) { response = reply(request, false, "IO_ERROR", {{"message", e.what()}}); }
                service::writeFrame(*peer->socket, response, std::chrono::steady_clock::now() + std::chrono::seconds(5));
            } catch (...) { break; }
        }
        peer->socket->interrupt(); peer->done = true;
    }
    void acceptLoop() {
        while (!stopping) {
            fd_set readable; FD_ZERO(&readable); FD_SET(listener->native(), &readable);
            timeval timeout{0, 100000};
            const auto selected = select(0, &readable, nullptr, nullptr, &timeout);
            std::lock_guard<std::mutex> lock(peersMutex);
            for (auto it = peers.begin(); it != peers.end();) {
                if ((*it)->done) { if ((*it)->thread.joinable()) (*it)->thread.join(); it = peers.erase(it); }
                else ++it;
            }
            if (selected <= 0 || stopping) continue;
            SOCKET accepted = ::accept(listener->native(), nullptr, nullptr);
            if (accepted == INVALID_SOCKET) continue;
            if (peers.size() >= 64) { closesocket(accepted); continue; }
            auto peer = std::make_shared<Peer>();
            peer->socket = std::make_shared<service::Socket>(accepted);
            peer->thread = std::thread([this, peer] { serve(peer); });
            peers.push_back(std::move(peer));
        }
    }
    bool start() {
        if (!stopping) return true;
        try {
            service::initializeSockets();
            fs::create_directories(root);
            root = fs::weakly_canonical(fs::absolute(root));
            auto lease = std::make_unique<DirectoryLease>(root);
            accounts = service::loadRecord(root / "accounts.json");
            sessions = service::loadRecord(root / "sessions.json");
            listener = std::make_shared<service::Socket>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (listener->native() == INVALID_SOCKET) return false;
            BOOL exclusive = TRUE;
            setsockopt(listener->native(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive), sizeof(exclusive));
            sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(requestedPort);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::bind(listener->native(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) || ::listen(listener->native(), SOMAXCONN)) {
                listener.reset(); return false;
            }
            int length = sizeof(address);
            if (getsockname(listener->native(), reinterpret_cast<sockaddr*>(&address), &length)) { listener.reset(); return false; }
            boundPort = ntohs(address.sin_port); stopping = false;
            acceptThread = std::thread([this] { acceptLoop(); });
            directoryLease = std::move(lease);
            return true;
        } catch (...) { stopping = true; listener.reset(); boundPort = 0; return false; }
    }
    void stop() {
        if (stopping.exchange(true)) return;
        listener->interrupt();
        { std::lock_guard<std::mutex> lock(peersMutex); for (auto& peer : peers) peer->socket->interrupt(); }
        if (acceptThread.joinable()) acceptThread.join();
        for (auto& peer : peers) if (peer->thread.joinable()) peer->thread.join();
        peers.clear(); listener.reset(); boundPort = 0;
        std::lock_guard<std::mutex> lock(recordsMutex); replay.clear(); directoryLease.reset();
    }
};
LocalServer::LocalServer(std::string directory, std::uint16_t port) : impl_(new Impl(std::move(directory), port)) {}
LocalServer::~LocalServer() { impl_->stop(); }
bool LocalServer::start() { return impl_->start(); }
void LocalServer::stop() { impl_->stop(); }
std::uint16_t LocalServer::port() const { return impl_->boundPort; }
