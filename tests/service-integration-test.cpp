#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include "service/local-server.h"
#include "service/service-client.h"
#include "service/crypto.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using Json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct Wire {
    SOCKET socket = INVALID_SOCKET;
    explicit Wire(std::uint16_t port) {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        DWORD timeout = 5000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
        require(::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "TCP connect failed");
    }
    ~Wire() { if (socket != INVALID_SOCKET) closesocket(socket); }
    Wire(const Wire&) = delete;
    void sendAll(const char* bytes, std::size_t count) {
        while (count) {
            const int sent = ::send(socket, bytes, static_cast<int>(count), 0);
            require(sent > 0, "TCP send failed");
            bytes += sent; count -= sent;
        }
    }
    void receiveAll(char* bytes, std::size_t count) {
        while (count) {
            const int got = ::recv(socket, bytes, static_cast<int>(count), 0);
            require(got > 0, "TCP receive failed");
            bytes += got; count -= got;
        }
    }
    Json receive() {
        std::uint32_t length;
        receiveAll(reinterpret_cast<char*>(&length), 4);
        length = ntohl(length);
        require(length > 0 && length <= 2 * 1024 * 1024, "invalid response frame size");
        std::string bytes(length, '\0'); receiveAll(bytes.data(), bytes.size());
        return Json::parse(bytes);
    }
    void send(const Json& request, bool fragment = false) {
        const auto bytes = request.dump();
        const auto length = htonl(static_cast<std::uint32_t>(bytes.size()));
        std::string frame(reinterpret_cast<const char*>(&length), 4); frame += bytes;
        if (fragment) {
            for (char byte : frame) sendAll(&byte, 1);
        } else sendAll(frame.data(), frame.size());
    }
    Json call(const std::string& op, Json body = Json::object(), bool fragment = false,
              std::string id = "") {
        static std::uint64_t sequence = 0;
        if (id.empty()) id = "test-" + std::to_string(++sequence);
        send(Json{{"version", 1}, {"id", id}, {"op", op}, {"body", body}}, fragment);
        auto response = receive();
        require(response.at("id") == id && response.at("version") == 1 && response.at("op") == op,
                "response correlation lost");
        return response;
    }
};
Json readJson(const fs::path& path) { std::ifstream in(path); Json value; in >> value; return value; }
void waitFor(const std::function<bool()>& predicate, const std::string& message, int seconds = 12,
             const std::function<std::string()>& diagnostics = {}) {
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    while (Clock::now() < deadline) {
        if (predicate()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error(message + (diagnostics ? " | " + diagnostics() : ""));
}
std::string clientDiagnostics(const ServiceClient& client) {
    const auto service = client.snapshot(); const auto upload = client.uploadSnapshot();
    std::ostringstream out;
    out << "service.state=" << static_cast<int>(service.state) << " connected=" << service.connected
        << " authenticated=" << service.authenticated << " busy=" << service.busy
        << " reconnects=" << service.reconnectAttempts << " code=" << service.code << " error=" << service.error
        << " upload.state=" << static_cast<int>(upload.state) << " offset=" << upload.uploadedBytes << '/' << upload.totalBytes
        << " bytes/s=" << upload.bytesPerSecond << " code=" << upload.code << " error=" << upload.error;
    return out.str();
}
void waitForUploadCompletion(const ServiceClient& client, const std::string& message) {
    // A transfer deadline must account for Debug JSON cost and durable disk throughput.
    // Detect a stalled state independently instead of treating slow forward progress as lost reconnect.
    const auto begin = Clock::now(); const auto deadline = begin + std::chrono::seconds(75);
    auto lastProgress = begin; auto nextReport = begin + std::chrono::seconds(10);
    auto confirmed = client.uploadSnapshot().uploadedBytes;
    while (Clock::now() < deadline) {
        const auto state = client.uploadSnapshot();
        if (state.state == UploadState::Completed) return;
        if (state.state == UploadState::Failed || state.state == UploadState::Canceled)
            throw std::runtime_error(message + " | terminal failure | " + clientDiagnostics(client));
        const auto now = Clock::now();
        if (state.uploadedBytes > confirmed) { confirmed = state.uploadedBytes; lastProgress = now; }
        if (now - lastProgress >= std::chrono::seconds(20))
            throw std::runtime_error(message + " | no acknowledged progress for 20s | " + clientDiagnostics(client));
        if (now >= nextReport) {
            std::cout << "INFO reconnect waiting elapsed=" << std::chrono::duration<double>(now-begin).count()
                      << "s " << clientDiagnostics(client) << std::endl;
            nextReport = now + std::chrono::seconds(10);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error(message + " | 75s total transfer deadline | " + clientDiagnostics(client));
}
std::string loginWire(Wire& wire, const std::string& user = "uploader", const std::string& password = "upload-password") {
    wire.call("register", {{"username", user}, {"password", password}});
    auto response = wire.call("login", {{"username", user}, {"password", password}});
    require(response.at("ok") == true, "upload test login failed");
    return response.at("body").at("token");
}
std::string fixture(std::size_t size) {
    std::string bytes(size, '\0');
    for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<char>((i * 37 + i / 251) % 256);
    return bytes;
}
Json chunkBody(const std::string& token, const std::string& id, std::uint64_t offset, const std::string& bytes) {
    return {{"token", token}, {"uploadId", id}, {"offset", offset},
            {"sha256", service::sha256(bytes)},
            {"data", service::hexEncode(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size())}};
}
void uploadWireTests(const fs::path& root) {
    LocalServer server(root.u8string(), 0); require(server.start(), "upload server start failed");
    const auto port = server.port();
    const auto bytes = fixture(700 * 1024 + 19);
    const auto digest = service::sha256(bytes);
    require(service::sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 known vector differs");
    std::string token, id;
    const std::string hostile = "../../outside\\..\\CON:evil.mp4";
    {
        Wire wire(port); token = loginWire(wire);
        const Json initBody{{"token", token}, {"filename", hostile}, {"size", bytes.size()}, {"sha256", digest}};
        auto init = wire.call("upload.init", initBody);
        require(init.at("ok") == true, "authenticated upload initialization failed");
        id = init.at("body").at("uploadId");
        require(init.at("body").at("offset") == 0, "new upload did not start at zero");
        const auto first = bytes.substr(0, service::UploadChunkBytes);
        auto chunk = chunkBody(token, id, 0, first);
        auto bad = chunk; bad["sha256"] = std::string(64, '0');
        require(wire.call("upload.chunk", bad).at("code") == "HASH_MISMATCH", "corrupt chunk accepted");
        bad = chunk; bad["offset"] = service::UploadChunkBytes;
        require(wire.call("upload.chunk", bad).at("code") == "OFFSET_MISMATCH", "out-of-order chunk accepted");
        bad = chunkBody(token, id, 0, fixture(service::UploadChunkBytes + 1));
        require(wire.call("upload.chunk", bad).at("code") == "INVALID_CHUNK", "oversize chunk accepted");
        auto result = wire.call("upload.chunk", chunk, false, "lost-ack");
        require(result.at("body").at("offset") == service::UploadChunkBytes, "first chunk offset not acknowledged");
        require(wire.call("upload.chunk", chunk, false, "lost-ack").at("body").at("offset") == service::UploadChunkBytes, "duplicate chunk was not idempotent");
        auto different = first; different[0] ^= 1;
        require(wire.call("upload.chunk", chunkBody(token, id, 0, different)).at("code") == "CHUNK_CONFLICT", "conflicting duplicate accepted");
        require(wire.call("upload.finish", {{"token", token}, {"uploadId", id}}).at("code") == "INCOMPLETE", "partial upload was published");
        const auto otherToken = loginWire(wire, "other-user");
        require(wire.call("upload.status", {{"token", otherToken}, {"uploadId", id}}).at("code") == "NOT_FOUND", "user isolation failed");
    }
    server.stop();
    // Simulate durable bytes written after the last committed metadata checkpoint.
    {
        const auto partial = root / "uploads" / service::sha256("uploader") / (id + ".part");
        std::ofstream unacknowledged(partial, std::ios::binary | std::ios::app);
        unacknowledged << "unacknowledged-tail";
    }
    LocalServer restarted(root.u8string(), port); require(restarted.start(), "upload recovery server failed");
    {
        Wire wire(port);
        auto status = wire.call("upload.status", {{"token", token}, {"uploadId", id}});
        require(status.at("body").at("offset") == service::UploadChunkBytes, "durable offset did not survive server restart");
        require(fs::file_size(root / "uploads" / service::sha256("uploader") / (id + ".part")) == service::UploadChunkBytes,
                "uncommitted tail was not truncated to confirmed offset");
        require(wire.call("upload.init", {{"token", token}, {"filename", hostile}, {"size", bytes.size()}, {"sha256", digest}}).at("body").at("uploadId") == id, "upload init did not reuse content identity");
        for (std::uint64_t offset = service::UploadChunkBytes; offset < bytes.size(); offset += service::UploadChunkBytes) {
            auto block = bytes.substr(static_cast<std::size_t>(offset), service::UploadChunkBytes);
            require(wire.call("upload.chunk", chunkBody(token, id, offset, block)).at("body").at("offset") == offset + block.size(), "resumed chunk acknowledgement wrong");
        }
        auto complete = wire.call("upload.finish", {{"token", token}, {"uploadId", id}});
        require(complete.at("ok") == true, "full upload failed final verification");
        const auto path = fs::u8path(complete.at("body").at("path").get<std::string>());
        require(path.is_relative(), "server publication path must be relative");
        const auto published = fs::weakly_canonical(root / path);
        const auto relative = published.lexically_relative(fs::weakly_canonical(root));
        require(!relative.empty() && *relative.begin() != "..", "hostile filename escaped server root");
        require(service::sha256File(published) == digest, "published file bytes differ");
        require(wire.call("upload.finish", {{"token", token}, {"uploadId", id}}).at("ok") == true, "finalization retry not idempotent");
        auto wrongInit = wire.call("upload.init", {{"token", token}, {"filename", "wrong.bin"}, {"size", 3}, {"sha256", std::string(64, '0')}});
        const auto wrongId = wrongInit.at("body").at("uploadId").get<std::string>();
        wire.call("upload.chunk", chunkBody(token, wrongId, 0, "abc"));
        require(wire.call("upload.finish", {{"token", token}, {"uploadId", wrongId}}).at("code") == "HASH_MISMATCH", "whole-file corruption was published");
        require(wire.call("upload.cancel", {{"token", token}, {"uploadId", wrongId}}).at("body").at("state") == "canceled", "cancel failed");
        require(wire.call("upload.chunk", chunkBody(token, wrongId, 0, "abc")).at("code") == "CANCELED", "canceled upload accepted a block");
        auto empty = wire.call("upload.init", {{"token", token}, {"filename", "empty.bin"}, {"size", 0},
            {"sha256", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}});
        require(empty.at("ok") == true, "empty upload initialization failed");
        const auto emptyId = empty.at("body").at("uploadId").get<std::string>();
        require(wire.call("upload.finish", {{"token", token}, {"uploadId", emptyId}}).at("ok") == true, "empty file did not finalize");
        require(wire.call("upload.init", {{"token", token}, {"filename", "negative.bin"}, {"size", -1}, {"sha256", digest}}).at("code") == "INVALID_UPLOAD", "negative upload size accepted");
        require(wire.call("upload.status", {{"token", token}, {"uploadId", "../../outside"}}).at("code") == "NOT_FOUND", "hostile upload ID was used as a path");
    }
    std::cout << "PASS chunk hashes, durable offset recovery, duplicate chunks, user isolation, hostile filename, final hash\n";
}
void clientTests(const fs::path& root) {
    fs::create_directories(root);
    LocalServer server((root / "server").u8string(), 0); require(server.start(), "client test server start failed");
    const auto port = server.port();
    ServiceClient client;
    client.connect("127.0.0.1", port);
    waitFor([&] { return client.snapshot().connected; }, "asynchronous client never connected");
    client.upload((root / "missing.bin").u8string());
    waitFor([&] { return client.uploadSnapshot().state == UploadState::Failed; }, "unauthenticated upload not rejected");
    require(client.uploadSnapshot().code == "UNAUTHORIZED", "wrong unauthenticated upload error");
    client.registerAccount("client-user", "client-password");
    waitFor([&] { const auto s = client.snapshot(); return !s.busy && s.code == "OK"; }, "client registration failed");
    client.login("client-user", "wrong");
    waitFor([&] { return client.snapshot().code == "BAD_CREDENTIALS"; }, "client wrong-password result missing");
    client.login("client-user", "client-password");
    waitFor([&] { return client.snapshot().authenticated; }, "client login failed");
    const auto path = root / "large.bin";
    const auto bytes = fixture(24 * 1024 * 1024 + 41);
    { std::ofstream file(path, std::ios::binary); file.write(bytes.data(), bytes.size()); }
    client.upload(path.u8string());
    waitFor([&] { auto s = client.uploadSnapshot(); return s.state == UploadState::Uploading && s.uploadedBytes > 0; }, "client upload did not begin", 20);
    client.pauseUpload();
    waitFor([&] { return client.uploadSnapshot().state == UploadState::Paused; }, "client pause not acknowledged");
    const auto paused = client.uploadSnapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    require(client.uploadSnapshot().uploadedBytes == paused.uploadedBytes, "paused upload kept advancing");
    require(paused.uploadedBytes > 0 && paused.uploadedBytes < paused.totalBytes, "pause did not preserve partial transfer");
    server.stop();
    waitFor([&] { return client.snapshot().state == ServiceState::Reconnecting; }, "heartbeat did not detect socket loss", 10);
    LocalServer restarted((root / "server").u8string(), port); require(restarted.start(), "client recovery server start failed");
    waitFor([&] { auto s = client.snapshot(); return s.connected && s.authenticated && s.reconnectAttempts > 0; }, "session not restored automatically", 15);
    require(client.uploadSnapshot().state == UploadState::Paused, "reconnect ignored paused state");
    client.resumeUpload();
    waitFor([&] { return client.uploadSnapshot().uploadedBytes > paused.uploadedBytes; }, "resumed client did not advance", 15);
    restarted.stop();
    waitFor([&] { return client.snapshot().state == ServiceState::Reconnecting; }, "active-upload lost socket not detected", 10);
    LocalServer again((root / "server").u8string(), port); require(again.start(), "active upload recovery server start failed");
    const auto reconnectBegin = Clock::now();
    waitForUploadCompletion(client, "active upload did not resume after reconnect");
    std::cout << "INFO active reconnect transfer elapsed=" << std::chrono::duration<double>(Clock::now()-reconnectBegin).count()
              << "s " << clientDiagnostics(client) << std::endl;
    const auto finished = client.uploadSnapshot();
    require(finished.uploadedBytes == bytes.size() && finished.progress == 1, "completed client progress incorrect");
    require(service::sha256File(root / "server" / fs::u8path(finished.serverPath)) == service::sha256(bytes), "client transfer final bytes differ");
    const auto canceledPath = root / "cancel.bin";
    auto cancelBytes = bytes; cancelBytes[0] ^= 3;
    { std::ofstream file(canceledPath, std::ios::binary); file.write(cancelBytes.data(), cancelBytes.size()); }
    client.upload(canceledPath.u8string());
    waitFor([&] { return client.uploadSnapshot().uploadedBytes > 0; }, "cancel fixture never began");
    for (int cycle = 0; cycle < 12; ++cycle) {
        client.pauseUpload();
        waitFor([&] { return client.uploadSnapshot().state == UploadState::Paused; }, "pause at block boundary was not published", 3);
        const auto at = client.uploadSnapshot().uploadedBytes;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        require(client.uploadSnapshot().uploadedBytes == at, "block boundary pause advanced");
        client.resumeUpload();
        waitFor([&] { return client.uploadSnapshot().uploadedBytes > at; }, "block boundary resume failed", 3);
    }
    client.cancelUpload();
    waitFor([&] { return client.uploadSnapshot().state == UploadState::Canceled; }, "client cancellation failed");
    const auto canceledId = client.uploadSnapshot().uploadId;
    {
        Wire wire(port); const auto token = loginWire(wire, "client-user", "client-password");
        waitFor([&] { auto s = wire.call("upload.status", {{"token", token}, {"uploadId", canceledId}});
                      return s.at("body").value("state", "") == "canceled"; }, "server cancellation never arrived");
    }
    client.upload(path.u8string());
    waitFor([&] { return client.uploadSnapshot().state == UploadState::Completed; }, "repeat completed upload failed");
    client.logout();
    waitFor([&] { return !client.snapshot().authenticated && !client.snapshot().busy; }, "client logout failed");
    client.disconnect();
    waitFor([&] { return client.snapshot().state == ServiceState::Disconnected; }, "client disconnect failed");
    const auto restartPath = root / "restart.bin";
    auto restartBytes = bytes; restartBytes[0] ^= 7;
    { std::ofstream file(restartPath, std::ios::binary); file.write(restartBytes.data(), restartBytes.size()); }
    std::uint64_t durableOffset = 0;
    {
        ServiceClient exiting; exiting.connect("127.0.0.1", port);
        waitFor([&] { return exiting.snapshot().connected; }, "exiting client connect failed");
        exiting.login("client-user", "client-password");
        waitFor([&] { return exiting.snapshot().authenticated; }, "exiting client login failed");
        exiting.upload(restartPath.u8string());
        waitFor([&] { return exiting.uploadSnapshot().uploadedBytes > 0; }, "exiting client did not upload");
        exiting.pauseUpload();
        waitFor([&] { return exiting.uploadSnapshot().state == UploadState::Paused; }, "exiting client pause failed");
        durableOffset = exiting.uploadSnapshot().uploadedBytes;
    }
    {
        ServiceClient fresh; fresh.connect("127.0.0.1", port);
        waitFor([&] { return fresh.snapshot().connected; }, "fresh client connect failed");
        fresh.login("client-user", "client-password");
        waitFor([&] { return fresh.snapshot().authenticated; }, "fresh client login failed");
        fresh.upload(restartPath.u8string());
        waitFor([&] { return fresh.uploadSnapshot().uploadedBytes >= durableOffset; }, "fresh client failed to recover durable upload offset");
        waitFor([&] { return fresh.uploadSnapshot().state == UploadState::Completed; }, "fresh client failed to complete recovered task", 30);
    }
    std::cout << "PASS asynchronous client auth, pause/resume, heartbeat, session and active upload reconnect\n";
}
void hostileFrameTests(const fs::path& root) {
    LocalServer server(root.u8string(), 0); require(server.start(), "hostile frame listener failed");
    const auto port = server.port();
    for (const auto size : {0u, service::MaxFrameBytes + 1}) {
        Wire wire(port); auto header = htonl(size);
        wire.sendAll(reinterpret_cast<char*>(&header), 4);
        char byte; require(recv(wire.socket, &byte, 1, 0) <= 0, "invalid frame length connection remained open");
    }
    {
        Wire wire(port);
        const auto malformed = htonl(1u); wire.sendAll(reinterpret_cast<const char*>(&malformed), 4); wire.sendAll("{", 1);
        char byte; require(recv(wire.socket, &byte, 1, 0) <= 0, "malformed JSON connection remained open");
    }
    {
        Wire wire(port);
        wire.send({{"version", 1}, {"id", 42}, {"op", "heartbeat"}, {"body", Json::object()}});
        require(wire.receive().at("code") == "BAD_REQUEST", "nonstring request ID accepted");
    }
    std::vector<std::unique_ptr<Wire>> idle;
    for (int i = 0; i < 32; ++i) {
        auto wire = std::make_unique<Wire>(port); const char partialHeader = 0;
        wire->sendAll(&partialHeader, 1); idle.push_back(std::move(wire));
    }
    const auto begin = Clock::now(); server.stop();
    require(Clock::now() - begin < std::chrono::seconds(3), "server shutdown scaled linearly with idle connection timeouts");
    {
        const auto beginClient = Clock::now();
        { ServiceClient client; client.connect("192.0.2.1", 59999); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        require(Clock::now() - beginClient < std::chrono::seconds(6), "client destructor exceeded one connection deadline");
    }
    std::cout << "PASS malformed/oversize frames and bounded teardown with 32 incomplete sockets\n";
}
void directoryOwnershipTests(const fs::path& root) {
    LocalServer first(root.u8string(), 0); require(first.start(), "first directory owner failed to start");
    LocalServer second((root / ".").u8string(), 0);
    require(!second.start(), "second server acquired the same data directory on another port");
    require(second.port() == 0, "rejected directory owner reported a bound port");
    Wire wire(first.port()); const auto token = loginWire(wire, "directory-owner");
    require(wire.call("session.restore", {{"token", token}}).at("ok") == true, "original directory owner was affected");
    first.stop();
    require(second.start(), "data directory lease was not released after stop");
    Wire reopened(second.port());
    require(reopened.call("session.restore", {{"token", token}}).at("ok") == true, "persisted session missing after lease handoff");
    second.stop();
    LocalServer blocker((root / "other").u8string(), 0); require(blocker.start(), "port blocker failed");
    LocalServer failed(root.u8string(), blocker.port()); require(!failed.start(), "occupied port unexpectedly bound");
    require(second.start(), "bind failure retained the data directory lease");
    std::cout << "PASS exclusive data directory ownership, lease handoff and failed-start release\n";
}
void clientRegressionTests(const fs::path& root, const std::string& scenario) {
    fs::create_directories(root);
    LocalServer server((root / "server").u8string(), 0); require(server.start(), "client regression server failed");
    const auto port = server.port();
    ServiceClient client; client.connect("127.0.0.1", port);
    waitFor([&] { return client.snapshot().connected; }, "regression client connect failed");
    client.registerAccount("regression", "regression-password");
    waitFor([&] { return !client.snapshot().busy && client.snapshot().code == "OK"; }, "regression registration failed");
    client.login("regression", "regression-password");
    waitFor([&] { return client.snapshot().authenticated; }, "regression login failed");
    const auto path = root / "fixture.bin";
    const auto bytes = fixture(8 * 1024 * 1024 + 41);
    { std::ofstream file(path, std::ios::binary); file.write(bytes.data(), bytes.size()); }
    client.upload(path.u8string());
    waitFor([&] { auto upload = client.uploadSnapshot(); return upload.uploadedBytes > 0; }, "regression upload did not start", 20,
        [&] { return clientDiagnostics(client); });
    client.pauseUpload();
    waitFor([&] { return client.uploadSnapshot().state == UploadState::Paused; }, "regression pause failed", 5,
        [&] { return clientDiagnostics(client); });
    const auto paused = client.uploadSnapshot();
    require(paused.uploadedBytes < bytes.size(), "regression fixture already completed");
    const auto sessions = readJson(root / "server" / "sessions.json");
    std::string token;
    for (auto it=sessions.begin(); it!=sessions.end(); ++it)
        if (it.value().is_object() && it.value().value("username", "") == "regression") token = it.key();
    require(!token.empty(), "cannot locate actual client token for revocation test");
    if (scenario == "revoked") {
        Wire wire(port); require(wire.call("logout", {{"token", token}}).at("ok") == true, "external token revocation failed");
        client.resumeUpload();
        waitFor([&] { return client.uploadSnapshot().code == "UNAUTHORIZED"; }, "upload did not observe revoked token", 5,
            [&] { return clientDiagnostics(client); });
        const auto state = client.snapshot();
        require(!state.authenticated && state.username.empty(), "upload UNAUTHORIZED left client authenticated with stale username/token");
        require(state.connected && state.code == "UNAUTHORIZED", "revocation did not report account failure on existing connection");
    } else if (scenario == "cancel-completed") {
        Wire wire(port);
        for (std::uint64_t offset=paused.uploadedBytes; offset<bytes.size(); offset+=service::UploadChunkBytes) {
            auto block=bytes.substr(static_cast<std::size_t>(offset),service::UploadChunkBytes);
            require(wire.call("upload.chunk", chunkBody(token,paused.uploadId,offset,block)).at("ok") == true, "external completion block failed");
        }
        require(wire.call("upload.finish", {{"token",token},{"uploadId",paused.uploadId}}).at("ok") == true, "external completion failed");
        client.cancelUpload();
        waitFor([&] { return !client.uploadSnapshot().serverPath.empty(); }, "cancel did not receive published file response", 5,
            [&] { return clientDiagnostics(client); });
        require(client.uploadSnapshot().state == UploadState::Completed, "cancellation overwrote server Completed result");
        require(service::sha256File(root / "server" / fs::u8path(client.uploadSnapshot().serverPath)) == service::sha256(bytes), "completed cancellation lost published file");
    } else if (scenario == "cancel-error") {
        const auto checkpointTemp = root / "server" / "uploads" / service::sha256("regression") / (paused.uploadId + ".json.tmp");
        fs::create_directory(checkpointTemp); // Actual filesystem failure at the server's atomic checkpoint write.
        client.cancelUpload();
        waitFor([&] { return client.uploadSnapshot().code == "IO_ERROR"; }, "cancel persistence failure did not arrive", 5,
            [&] { return clientDiagnostics(client); });
        require(client.uploadSnapshot().state == UploadState::Failed, "cancellation overwrote IO_ERROR with Canceled");
        require(!client.uploadSnapshot().error.empty(), "cancellation hid server error text");
        fs::remove(checkpointTemp);
    }
    std::cout << "PASS client regression " << scenario << std::endl;
}
void serverProcessTests(const fs::path& root, const fs::path& executable) {
    require(fs::exists(executable), "native-service standalone executable is missing");
    LocalServer probe(root.u8string(), 0); require(probe.start(), "cannot reserve child server test port");
    const auto port = probe.port(); probe.stop();
    auto launch = [&](const std::wstring& args, DWORD expectedExit, bool connectTest) {
        std::wstring command = L"\"" + executable.wstring() + L"\" " + args;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process) != 0, "cannot start native-service child");
        struct Child {
            PROCESS_INFORMATION process;
            ~Child() { TerminateProcess(process.hProcess, 0); WaitForSingleObject(process.hProcess, 2000); CloseHandle(process.hThread); CloseHandle(process.hProcess); }
        } child{process};
        if (connectTest) {
            waitFor([&] { try { Wire wire(port); return wire.call("heartbeat").at("ok") == true; } catch (...) { return false; } }, "standalone server never listened");
        } else {
            require(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0, "invalid server arguments did not terminate");
            DWORD exitCode; require(GetExitCodeProcess(process.hProcess, &exitCode) != 0 && exitCode == expectedExit, "server argument exit code differs");
        }
    };
    launch(L"--port " + std::to_wstring(port) + L" --data \"" + root.wstring() + L"\"", 0, true);
    {
        LocalServer owner(root.u8string(), 0); require(owner.start(), "cross-process directory owner failed");
        LocalServer otherRoot((root / "different").u8string(), 0); require(otherRoot.start(), "cannot reserve competing process port");
        const auto otherPort=otherRoot.port(); otherRoot.stop();
        launch(L"--port " + std::to_wstring(otherPort) + L" --data \"" + root.wstring() + L"\"", 1, false);
    }
    launch(L"--port 65536", 2, false);
    launch(L"--bogus", 2, false);
    std::cout << "PASS standalone native-service child process and argument validation\n";
}
void accountTests(const fs::path& root) {
    LocalServer server(root.u8string(), 0);
    require(server.start(), "service must start a real loopback listener");
    const auto port = server.port();
    require(port != 0, "ephemeral port was not reported");
    std::string token;
    {
        Wire wire(port);
        auto heartbeat = wire.call("heartbeat", Json::object(), true, "fragmented");
        require(heartbeat.at("ok") == true, "fragmented frame was rejected");
        require(wire.call("upload.init", {{"filename", "x"}, {"size", 1}, {"sha256", std::string(64, '0')}}).at("code") == "UNAUTHORIZED", "upload allowed without authentication");
        require(wire.call("register", {{"username", "alice"}, {"password", "correct horse"}}, false, "persisted-register").at("ok") == true, "registration failed");
        require(wire.call("register", {{"username", "alice"}, {"password", "wrong"}}).at("code") == "ACCOUNT_EXISTS", "duplicate registration accepted");
        require(wire.call("login", {{"username", "alice"}, {"password", "wrong"}}).at("code") == "BAD_CREDENTIALS", "wrong password accepted");
        require(wire.call("login", {{"username", "nobody"}, {"password", "wrong"}}).at("code") == "BAD_CREDENTIALS", "unknown account not rejected");
        auto login = wire.call("login", {{"username", "alice"}, {"password", "correct horse"}}, false, "persisted-login");
        require(login.at("ok") == true, "valid login failed");
        token = login.at("body").at("token");
        require(token.size() == 64, "session token must contain 256 random bits");
        require(wire.call("session.restore", {{"token", token}}).at("body").at("username") == "alice", "session restore failed");
        wire.send({{"version", 7}, {"id", "bad-version"}, {"op", "heartbeat"}, {"body", Json::object()}});
        require(wire.receive().at("code") == "BAD_VERSION", "unknown version accepted");
        require(wire.call("unknown").at("code") == "UNKNOWN_OP", "unknown operation accepted");
        require(wire.call("register", {{"username", "salt-peer"}, {"password", "correct horse"}}).at("ok") == true, "second salted account registration failed");
    }
    auto accounts = readJson(root / "accounts.json");
    const auto account = accounts.at("alice");
    require(account.at("iterations") == 600000, "PBKDF2 work factor differs from approved design");
    require(account.at("salt").get<std::string>().size() >= 32, "missing random salt");
    require(account.at("hash").get<std::string>().size() == 64, "missing SHA256 password hash");
    require(accounts.dump().find("correct horse") == std::string::npos, "plaintext password persisted");
    require(accounts.at("salt-peer").at("salt") != account.at("salt"), "accounts reused password salt");
    require(accounts.at("salt-peer").at("hash") != account.at("hash"), "identical passwords shared account hashes");
    server.stop();
    LocalServer reopened(root.u8string(), port);
    require(reopened.start(), "persisted service failed to restart");
    Wire wire(reopened.port());
    require(wire.call("register", {{"username", "alice"}, {"password", "correct horse"}}, false, "persisted-register").at("ok") == true,
        "registration retry after server restart lost its request ID");
    require(wire.call("login", {{"username", "alice"}, {"password", "correct horse"}}, false, "persisted-login").at("body").at("token") == token,
        "login retry after server restart changed the session token");
    require(wire.call("login", {{"username", "alice"}, {"password", "correct horse"}}).at("ok") == true, "account did not persist across server objects");
    require(wire.call("session.restore", {{"token", token}}).at("ok") == true, "session did not persist across restart");
    require(wire.call("logout", {{"token", token}}).at("ok") == true, "logout failed");
    require(wire.call("session.restore", {{"token", token}}).at("code") == "UNAUTHORIZED", "logged-out token remained usable");
    std::cout << "PASS framing, authentication, account persistence, session restart/revocation\n";
}
int main(int argc, char** argv) {
    WSADATA winsock{}; require(WSAStartup(MAKEWORD(2, 2), &winsock) == 0, "Winsock init failed");
    const auto root = fs::temp_directory_path() / ("native-service-test-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root); fs::create_directories(root);
    try {
        char selected[64]{};
        const auto selectedLength = GetEnvironmentVariableA("NP_SERVICE_TEST_CASE", selected, sizeof(selected));
        const std::string scenario = selectedLength && selectedLength < sizeof(selected) ? selected : "all";
        if (scenario == "all" || scenario == "directory") directoryOwnershipTests(root / "directory");
        for (const std::string regression : {"revoked", "cancel-completed", "cancel-error"})
            if (scenario == "all" || scenario == regression) clientRegressionTests(root / regression, regression);
        if (scenario == "all" || scenario == "account") accountTests(root / "account");
        if (scenario == "all" || scenario == "upload") uploadWireTests(root / "upload");
        if (scenario == "all" || scenario == "client") clientTests(root / "client");
        if (scenario == "all" || scenario == "hostile") hostileFrameTests(root / "hostile");
        if (argc > 1 && (scenario == "all" || scenario == "child")) serverProcessTests(root / "child", fs::u8path(argv[1]));
        fs::remove_all(root); WSACleanup();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << "\n";
        fs::remove_all(root); WSACleanup(); return 1;
    }
}
