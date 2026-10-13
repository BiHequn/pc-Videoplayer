#include <winsock2.h>
#include <Windows.h>
#include <winhttp.h>
#include "media/progressive-cache.h"
#include "media/loopback-http.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Real HTTP/filesystem regressions from the independent cache review.
// Usage: cache-robustness-test [artifact-root]. Root CMake is parent-owned.
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::size_t fixtureSize = 2 * 1024 * 1024;
constexpr std::size_t blockSize = 512 * 1024;

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
template<class Predicate>
void until(Predicate predicate, std::chrono::milliseconds timeout, const char* message) {
    const auto deadline = Clock::now() + timeout;
    while (!predicate()) {
        require(Clock::now() < deadline, message);
        std::this_thread::sleep_for(5ms);
    }
}

struct Internet {
    HINTERNET value = nullptr;
    explicit Internet(HINTERNET handle) : value(handle) {
        require(value != nullptr, "WinHTTP allocation failed");
    }
    ~Internet() { WinHttpCloseHandle(value); }
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
};
std::string header(HINTERNET request, DWORD key) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, key, nullptr, nullptr, &size, nullptr);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return {};
    std::wstring text(size / sizeof(wchar_t), 0);
    require(WinHttpQueryHeaders(request, key, nullptr, text.data(), &size, nullptr) != FALSE,
        "HTTP response header read failed");
    while (!text.empty() && text.back() == 0) text.pop_back();
    std::string ascii;
    ascii.reserve(text.size());
    for (const auto character : text) {
        require(character >= 0 && character <= 0x7f, "non-ASCII Content-Range response header");
        ascii.push_back(static_cast<char>(character));
    }
    return ascii;
}
struct Reply {
    DWORD status = 0;
    std::string contentRange;
    std::vector<unsigned char> bytes;
    bool readFailed = false;
};
Reply get(const std::string& url, const std::string& range = {}, bool head = false,
          std::size_t limit = fixtureSize) {
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = DWORD(-1);
    require(WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) != FALSE, "parse test URL");
    Internet session(WinHttpOpen(L"cache-robustness-test", WINHTTP_ACCESS_TYPE_NO_PROXY,
        nullptr, nullptr, 0));
    WinHttpSetTimeouts(session.value, 2000, 2000, 2000, 1500);
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    Internet connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
    Internet request(WinHttpOpenRequest(connection.value, head ? L"HEAD" : L"GET", path.c_str(),
        nullptr, nullptr, nullptr, 0));
    const std::string headers = range.empty() ? "" : "Range: bytes=" + range + "\r\n";
    const std::wstring wh(headers.begin(), headers.end());
    require(WinHttpSendRequest(request.value, wh.c_str(), DWORD(wh.size()), nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(request.value, nullptr), "test HTTP request failed");
    Reply reply;
    DWORD size = sizeof(reply.status);
    require(WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        nullptr, &reply.status, &size, nullptr) != FALSE, "HTTP status missing");
    reply.contentRange = header(request.value, WINHTTP_QUERY_CONTENT_RANGE);
    if (!head) {
        unsigned char bytes[8192];
        while (reply.bytes.size() < limit) {
            DWORD count = 0;
            if (!WinHttpReadData(request.value, bytes,
                DWORD(std::min(sizeof(bytes), limit - reply.bytes.size())), &count)) {
                reply.readFailed = true;
                break;
            }
            if (!count) break;
            reply.bytes.insert(reply.bytes.end(), bytes, bytes + count);
        }
    }
    return reply;
}

// Raw clients keep the handle test independent of WinHTTP's connection caches,
// and allow disconnecting a reader after it is known to be parked on a hole.
struct Socket {
    SOCKET value = INVALID_SOCKET;
    ~Socket() { close(); }
    Socket() = default;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    void close() {
        if (value != INVALID_SOCKET) {
            shutdown(value, SD_BOTH);
            closesocket(value);
            value = INVALID_SOCKET;
        }
    }
    void connectTo(std::uint16_t port) {
        value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(value != INVALID_SOCKET, "raw socket allocation");
        DWORD timeout = 2000;
        setsockopt(value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        require(connect(value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "raw HTTP connect");
    }
    void sendRequest(const std::string& path, const std::string& range = {}) {
        const std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n" +
            (range.empty() ? "" : "Range: bytes=" + range + "\r\n") + "Connection: close\r\n\r\n";
        std::size_t offset = 0;
        while (offset < request.size()) {
            const int count = send(value, request.data() + offset, int(request.size() - offset), 0);
            require(count > 0, "raw HTTP send");
            offset += count;
        }
    }
    std::string readHeaders() {
        std::string text;
        while (text.size() < 16384 && text.find("\r\n\r\n") == std::string::npos) {
            char byte = 0;
            require(recv(value, &byte, 1, 0) == 1, "raw response headers");
            text += byte;
        }
        require(text.find("\r\n\r\n") != std::string::npos, "incomplete raw HTTP headers");
        return text;
    }
};

enum class OriginMode { Normal, ChangedTotal, ChangedEtag, Transient503, NoValidator, WeakValidator };
class Origin {
public:
    explicit Origin(OriginMode selected = OriginMode::Normal, bool gated = false)
        : mode(selected), releaseBody(!gated), data(fixtureSize) {
        for (std::size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<unsigned char>((i * 13 + 7) % 251);
        require(server.start([this](const LocalHttpRequest& request, LocalHttpResponse& response) {
            serve(request, response);
        }), "fixture origin start");
    }
    ~Origin() {
        stopping = true;
        releaseBody = true;
        server.stop();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(server.port()) + "/video.flv"; }
    OriginMode mode;
    std::atomic<bool> releaseBody;
    std::atomic<unsigned int> bodyRequests{0}, unavailableResponses{0};
    std::vector<unsigned char> data;
private:
    std::atomic<bool> stopping{false};
    LoopbackHttp server;
    void serve(const LocalHttpRequest& request, LocalHttpResponse& response) {
        std::uint64_t first = 0, last = data.size() - 1;
        try {
            const auto dash = request.range.find('-');
            require(request.range.rfind("bytes=", 0) == 0 && dash != std::string::npos,
                "fixture expects an explicit Range");
            first = std::stoull(request.range.substr(6, dash - 6));
            last = std::stoull(request.range.substr(dash + 1));
            require(first <= last && last < data.size(), "fixture Range bounds");
        } catch (...) {
            response.headers(416, 0, "text/plain");
            return;
        }
        const bool probe = first == 0 && last == 0;
        if (!probe) {
            const auto ordinal = ++bodyRequests;
            // Delay headers until the loopback reader is waiting, so source rejection
            // cannot happen before the test establishes the consumer boundary.
            while (!releaseBody && !stopping && response.connected())
                std::this_thread::sleep_for(2ms);
            if (stopping || !response.connected()) return;
            if (mode == OriginMode::Transient503 && ordinal == 1) {
                ++unavailableResponses;
                if (response.headers(503, 4, "text/plain")) response.write("busy", 4);
                return;
            }
        }
        std::string etag = "\"strong-cache-fixture-v1\"";
        if (!probe && mode == OriginMode::ChangedEtag) etag = "\"strong-cache-fixture-v2\"";
        if (mode == OriginMode::WeakValidator) etag = "W/\"weak-fixture\"";
        if (mode == OriginMode::NoValidator) etag.clear();
        const std::uint64_t total = data.size() + (!probe && mode == OriginMode::ChangedTotal ? 1 : 0);
        const std::string extra = (etag.empty() ? "" : "ETag: " + etag + "\r\n") +
            "Content-Range: bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" +
            std::to_string(total) + "\r\n";
        if (!response.headers(206, last - first + 1, "video/x-flv", extra)) return;
        for (std::uint64_t position = first; position <= last && !stopping;) {
            const auto count = std::min<std::uint64_t>(65536, last - position + 1);
            if (!response.write(data.data() + position, static_cast<std::size_t>(count))) return;
            position += count;
            // The former bug publishes these bytes before validating total/ETag.
            // A gap gives a real waiting reader time to observe that wrong publication.
            if (!probe && (mode == OriginMode::ChangedTotal || mode == OriginMode::ChangedEtag))
                std::this_thread::sleep_for(10ms);
        }
    }
};

void complete(ProgressiveCache& cache) {
    until([&] {
        const auto state = cache.snapshot();
        require(state.state != CacheState::Error, "cache error: " + state.error);
        return state.state == CacheState::Complete;
    }, 8s, "cache did not complete");
}
void exactFile(ProgressiveCache& cache, const Origin& origin) {
    const auto reply = get(cache.playbackUrl());
    require(reply.status == 200 && !reply.readFailed && reply.bytes == origin.data,
        "complete cache is not byte-exact");
}
std::uint16_t cachePort(const std::string& url) {
    const auto colon = url.find(':', url.find("://") + 3);
    return static_cast<std::uint16_t>(std::stoul(url.substr(colon + 1)));
}

void changedSourceLeaksNothing(const fs::path& root, OriginMode mode) {
    Origin origin(mode, true);
    ProgressiveCache cache;
    require(cache.start(origin.url(), root.u8string()), "changed-source start");
    Socket reader;
    reader.connectTo(cachePort(cache.playbackUrl()));
    reader.sendRequest("/video.flv", "0-65535");
    require(reader.readHeaders().rfind("HTTP/1.1 206 ", 0) == 0, "range response missing");
    until([&] { return cache.snapshot().waitingForData && origin.bodyRequests > 0; },
        2s, "changed-source reader did not wait for a hole");
    origin.releaseBody = true;
    char bytes[65536];
    const int leaked = recv(reader.value, bytes, sizeof(bytes), 0);
    // EOF or a receive timeout is acceptable while rejection is being reported;
    // returning even one payload byte from the changed source is not.
    const auto state = cache.snapshot();
    reader.close();
    cache.cancel();
    cache.stop();
    require(leaked <= 0 && state.downloaded == 0,
        "changed source published payload before header validation");
    std::cout << "  rejected changed " << (mode == OriginMode::ChangedTotal ? "total" : "ETag")
              << ": leaked=0, published=0\n";
}

void transient503Retries(const fs::path& root) {
    Origin origin(OriginMode::Transient503);
    ProgressiveCache cache;
    require(cache.start(origin.url(), root.u8string()), "503 start");
    complete(cache);
    require(origin.unavailableResponses == 1 && origin.bodyRequests > fixtureSize / blockSize,
        "fixture did not exercise a retry after its single 503");
    exactFile(cache, origin);
    require(cache.snapshot().error.empty(), "successful retry retained an error");
}

void shortMetadataReaderIsTolerated(const fs::path& root) {
    Origin origin(OriginMode::Normal, true);
    ProgressiveCache cache;
    require(cache.start(origin.url(), root.u8string()), "metadata reader start");
    fs::path checkpoint;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.path().filename() == L"state.json") checkpoint = entry.path();
    require(!checkpoint.empty(), "initial checkpoint missing");
    struct File {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~File() { close(); }
        void close() { if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); value = INVALID_HANDLE_VALUE; } }
    } reader;
    reader.value = CreateFileW(checkpoint.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(reader.value != INVALID_HANDLE_VALUE, "open ordinary metadata reader without DELETE sharing");
    origin.releaseBody = true;
    // Initial save renamed away state.tmp; its reappearance proves a worker is
    // checkpointing while this real read handle blocks replacement.
    until([&] { return fs::exists(checkpoint.parent_path() / L"state.tmp"); },
        3s, "worker never attempted a checkpoint under the read lock");
    std::this_thread::sleep_for(150ms);
    reader.close();
    complete(cache);
    exactFile(cache, origin);
}

void suffixAndInvalidRanges(const fs::path& root) {
    Origin origin;
    ProgressiveCache cache;
    require(cache.start(origin.url(), root.u8string()), "range start");
    complete(cache);
    const auto suffix = get(cache.playbackUrl(), "-16");
    const std::vector<unsigned char> tail(origin.data.end() - 16, origin.data.end());
    require(suffix.status == 206 && !suffix.readFailed && suffix.bytes == tail,
        "suffix Range did not return the exact final 16 bytes");
    require(suffix.contentRange == "bytes 2097136-2097151/2097152", "suffix Content-Range incorrect");
    const auto open = get(cache.playbackUrl(), "2097136-");
    require(open.status == 206 && open.bytes == tail, "open-ended Range tail incorrect");
    for (const auto* range : {"0-7,12-15", "-0", "0-7garbage", "2097152-"}) {
        const auto reply = get(cache.playbackUrl(), range);
        require(reply.status == 416 && reply.bytes.empty() && reply.contentRange == "bytes */2097152",
            std::string("invalid or unsupported Range accepted: ") + range);
    }
}

void abandonedWaitClears(const fs::path& root) {
    Origin origin(OriginMode::Normal, true);
    ProgressiveCache cache;
    require(cache.start(origin.url(), root.u8string()), "abandoned reader start");
    cache.pause();
    Socket reader;
    reader.connectTo(cachePort(cache.playbackUrl()));
    reader.sendRequest("/video.flv", "524288-589823");
    require(reader.readHeaders().rfind("HTTP/1.1 206 ", 0) == 0, "waiting response headers");
    until([&] { return cache.snapshot().waitingForData; }, 2s, "reader did not park on an unwritten range");
    reader.close();
    until([&] { return !cache.snapshot().waitingForData; }, 1500ms,
        "disconnected reader remains parked on a missing range");
    const auto before = Clock::now();
    cache.cancel();
    origin.releaseBody = true;
    cache.stop();
    require(Clock::now() - before < 3s, "cancel/stop stalled after an abandoned reader");
    require(cache.snapshot().state == CacheState::Cancelled, "cancel state lost");
}

void completedConnectionsAreReaped(const fs::path&) {
    LoopbackHttp server;
    require(server.start([](const LocalHttpRequest&, LocalHttpResponse& response) {
        const char payload = 'x';
        if (response.headers(200, 1, "text/plain")) response.write(&payload, 1);
    }), "handle fixture server start");
    const auto request = [&] {
        Socket client;
        client.connectTo(server.port());
        client.sendRequest("/");
        require(client.readHeaders().rfind("HTTP/1.1 200 ", 0) == 0, "handle fixture response");
        char payload = 0;
        require(recv(client.value, &payload, 1, 0) == 1 && payload == 'x', "handle fixture body");
        require(recv(client.value, &payload, 1, 0) == 0, "handle fixture did not close");
    };
    // Warm up Winsock and CRT before measuring only retained completed clients.
    for (int i = 0; i < 20; ++i) request();
    std::this_thread::sleep_for(250ms);
    DWORD before = 0, after = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "initial handle count");
    for (int i = 0; i < 200; ++i) request();
    until([&] {
        require(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE, "final handle count");
        return after <= before + 32;
    }, 2s, "200 completed HTTP requests retained unbounded native handles");
    // Measure before server.stop(), otherwise joining everything hides the leak.
    std::cout << "  200 raw connections: handles before=" << before << ", after=" << after << '\n';
}

void sameTaskCannotBeOpenedTwice(const fs::path& root) {
    Origin origin(OriginMode::Normal, true);
    ProgressiveCache first, second;
    require(first.start(origin.url(), root.u8string()), "first cache start");
    require(!second.start(origin.url(), root.u8string()), "second cache acquired the same task directory");
    const auto rejected = second.snapshot();
    require(rejected.state == CacheState::Error && rejected.downloaded == 0 && rejected.playbackUrl.empty(),
        "rejected duplicate cache exposed data or a playback endpoint");
    second.stop();
    origin.releaseBody = true;
    complete(first);
    exactFile(first, origin); // A rejected second opener must not truncate the live first cache.
    first.stop();
    ProgressiveCache reopened;
    require(reopened.start(origin.url(), root.u8string()), "task lock not released by stop");
    require(reopened.snapshot().state == CacheState::Complete && reopened.snapshot().downloaded == fixtureSize,
        "reopening a durable same-task cache discarded completed bytes");
    exactFile(reopened, origin);
}

void validatorlessSourcesAreRejected(const fs::path& root) {
    for (const auto mode : {OriginMode::NoValidator, OriginMode::WeakValidator}) {
        Origin origin(mode);
        ProgressiveCache cache;
        const auto directory = root / (mode == OriginMode::NoValidator ? "absent" : "weak");
        require(!cache.start(origin.url(), directory.u8string()), "source without a reliable validator accepted");
        const auto state = cache.snapshot();
        require(state.state == CacheState::Error && !state.error.empty() && state.downloaded == 0 &&
            state.playbackUrl.empty() && origin.bodyRequests == 0,
            "validatorless rejection started body downloads or exposed a cache endpoint");
    }
}
}

int main(int argc, char** argv) {
    try {
        require(argc <= 2, "usage: cache-robustness-test [artifact-root]");
        const fs::path base = argc == 2 ? fs::absolute(fs::u8path(argv[1]))
            : fs::temp_directory_path() / L"native-player-cache-robustness";
        const auto root = base / (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        fs::create_directories(root);
        std::cout << "Cache robustness artifacts: " << root.u8string() << '\n';
        const std::vector<std::pair<const char*, std::function<void(const fs::path&)>>> tests = {
            {"changed-total-zero-leak", [](const fs::path& path) { changedSourceLeaksNothing(path, OriginMode::ChangedTotal); }},
            {"changed-etag-zero-leak", [](const fs::path& path) { changedSourceLeaksNothing(path, OriginMode::ChangedEtag); }},
            {"transient-503-retry", transient503Retries},
            {"metadata-reader-150ms", shortMetadataReaderIsTolerated},
            {"suffix-and-invalid-ranges", suffixAndInvalidRanges},
            {"abandoned-reader-clears", abandonedWaitClears},
            {"completed-connections-reaped", completedConnectionsAreReaped},
            {"same-task-lock", sameTaskCannotBeOpenedTwice},
            {"validatorless-source-rejected", validatorlessSourcesAreRejected}
        };
        unsigned int failures = 0;
        for (const auto& test : tests) {
            try {
                test.second(root / test.first);
                std::cout << "PASS " << test.first << '\n';
            } catch (const std::exception& error) {
                ++failures;
                std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
            }
        }
        std::cout << tests.size() - failures << '/' << tests.size() << " cache robustness scenarios passed\n";
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
