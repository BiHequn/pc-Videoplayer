#define NOMINMAX

#include "vod-downloader.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlwapi.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::size_t kMaxWorkers = 4;
constexpr int kMaxAttempts = 3;
constexpr std::uint64_t kCheckpointBytes = 4ULL * 1024 * 1024;
constexpr DWORD kBufferSize = 64 * 1024;
constexpr std::size_t kMaxPlaylistBytes = 8 * 1024 * 1024;

struct RangeState {
    std::uint64_t start = 0;
    std::uint64_t end = 0; // Exclusive.
    std::uint64_t next = 0;
};

struct SegmentState {
    std::string file;
    bool complete = false;
};

struct DownloadState {
    int version = 1;
    std::string type;
    std::string urlHash;
    std::string etag;
    std::string lastModified;
    std::string playlistHash;
    std::uint64_t contentLength = 0;
    std::vector<RangeState> ranges;
    std::vector<SegmentState> segments;
    bool complete = false;
};

struct HttpResponse {
    DWORD status = 0;
    std::uint64_t contentLength = 0;
    bool supportsRange = false;
    std::uint64_t rangeStart = 0;
    std::uint64_t rangeEnd = 0; // Exclusive.
    std::string etag;
    std::string lastModified;
    std::string finalUrl;
};

struct Playlist {
    std::vector<std::string> lines;
    std::vector<std::size_t> uriLines;
    std::vector<std::string> segments;
    std::vector<std::string> variants;
    bool endList = false;
};

class HttpHandle {
  public:
    explicit HttpHandle(HINTERNET value = nullptr) : value_(value) {}
    ~HttpHandle() {
        if (value_) WinHttpCloseHandle(value_);
    }
    HttpHandle(const HttpHandle &) = delete;
    HttpHandle &operator=(const HttpHandle &) = delete;
    HINTERNET get() const {
        return value_;
    }

  private:
    HINTERNET value_ = nullptr;
};

class NamedMutex {
  public:
    ~NamedMutex() {
        if (locked_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }

    bool acquire(const std::wstring &name, const std::atomic_bool &cancelled, std::wstring *error) {
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!handle_) {
            if (error) *error = L"Cannot create cache lock.";
            return false;
        }

        while (!cancelled.load()) {
            DWORD result = WaitForSingleObject(handle_, 100);
            if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
                locked_ = true;
                return true;
            }
            if (result != WAIT_TIMEOUT) {
                if (error) *error = L"Cannot acquire cache lock.";
                return false;
            }
        }

        if (error) *error = L"Download cancelled.";
        return false;
    }

  private:
    HANDLE handle_ = nullptr;
    bool locked_ = false;
};

class ProgressReporter {
  public:
    explicit ProgressReporter(VodProgressCallback callback) : callback_(std::move(callback)) {}

    void report(std::uint64_t completed, std::uint64_t total) {
        int percent = total ? static_cast<int>(completed * 100 / total) : 0;
        percent = (std::max)(0, (std::min)(100, percent));

        std::lock_guard<std::mutex> lock(mutex_);
        if (percent <= last_) return;
        last_ = percent;
        if (callback_) callback_(percent);
    }

  private:
    VodProgressCallback callback_;
    std::mutex mutex_;
    int last_ = -1;
};

void setError(std::wstring *error, const std::wstring &value) {
    if (error) *error = value;
}

std::wstring utf8ToWide(const std::string &value) {
    if (value.empty()) return {};
    int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                               result.data(), size) == size
               ? result
               : std::wstring();
}

std::string wideToUtf8(const std::wstring &value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr,
                                   0, nullptr, nullptr);
    if (size <= 0) return {};

    std::string result(static_cast<std::size_t>(size), '\0');
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                               result.data(), size, nullptr, nullptr) == size
               ? result
               : std::string();
}

std::string trim(std::string value) {
    auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), space).base(), value.end());
    return value;
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

VodDownloadResult fail(const std::wstring &error) {
    VodDownloadResult result;
    result.error = error;
    return result;
}

VodDownloadResult succeed(const fs::path &path, bool fromCache) {
    VodDownloadResult result;
    result.success = true;
    result.fromCache = fromCache;
    result.localPath = path.u8string();
    return result;
}

fs::path cacheRoot() {
    wchar_t value[32768] = {};
    DWORD length =
        GetEnvironmentVariableW(L"LOCALAPPDATA", value, static_cast<DWORD>(sizeof(value) / sizeof(value[0])));

    if (length > 0 && length < sizeof(value) / sizeof(value[0])) return fs::path(value) / L"NativePlayer" / L"Cache";

    return fs::temp_directory_path() / L"NativePlayer" / L"Cache";
}

bool createUrlHash(const std::string &url, std::string *output) {
    if (!output || url.empty()) return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, hashSize = 0, bytes = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;
    bool ok = false;

    do {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) break;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                              sizeof(objectSize), &bytes, 0) < 0)
            break;
        if (BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize),
                              &bytes, 0) < 0)
            break;

        object.resize(objectSize);
        digest.resize(hashSize);

        if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) break;
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(url.data())),
                           static_cast<ULONG>(url.size()), 0) < 0)
            break;
        if (BCryptFinishHash(hash, digest.data(), hashSize, 0) < 0) break;

        static constexpr char digits[] = "0123456789abcdef";
        output->clear();
        output->reserve(digest.size() * 2);
        for (unsigned char value : digest) {
            output->push_back(digits[value >> 4]);
            output->push_back(digits[value & 0x0f]);
        }
        ok = true;
    } while (false);

    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

json stateToJson(const DownloadState &state) {
    json ranges = json::array();
    for (const auto &range : state.ranges)
        ranges.push_back({{"start", range.start}, {"end", range.end}, {"next", range.next}});

    json segments = json::array();
    for (const auto &segment : state.segments)
        segments.push_back({{"file", segment.file}, {"complete", segment.complete}});

    return {{"version", state.version},
            {"type", state.type},
            {"urlHash", state.urlHash},
            {"etag", state.etag},
            {"lastModified", state.lastModified},
            {"playlistHash", state.playlistHash},
            {"contentLength", state.contentLength},
            {"ranges", std::move(ranges)},
            {"segments", std::move(segments)},
            {"complete", state.complete}};
}

bool loadState(const fs::path &path, DownloadState *state) {
    if (!state) return false;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;

        json value;
        input >> value;
        DownloadState loaded;
        loaded.version = value.value("version", 0);
        loaded.type = value.value("type", "");
        loaded.urlHash = value.value("urlHash", "");
        loaded.etag = value.value("etag", "");
        loaded.lastModified = value.value("lastModified", "");
        loaded.playlistHash = value.value("playlistHash", "");
        loaded.contentLength = value.value("contentLength", std::uint64_t{0});
        loaded.complete = value.value("complete", false);

        for (const auto &item : value.value("ranges", json::array())) {
            RangeState range;
            range.start = item.value("start", std::uint64_t{0});
            range.end = item.value("end", std::uint64_t{0});
            range.next = item.value("next", range.start);
            if (range.start >= range.end || range.next < range.start || range.next > range.end) return false;
            loaded.ranges.push_back(range);
        }

        for (const auto &item : value.value("segments", json::array())) {
            SegmentState segment;
            segment.file = item.value("file", "");
            segment.complete = item.value("complete", false);
            if (segment.file.empty()) return false;
            loaded.segments.push_back(std::move(segment));
        }

        if (loaded.version != 1 || loaded.urlHash.empty()) return false;
        *state = std::move(loaded);
        return true;
    } catch (...) {
        return false;
    }
}

bool saveState(const fs::path &path, const DownloadState &state, std::wstring *error) {
    fs::path temporary = path;
    temporary += L".tmp";

    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            setError(error, L"Cannot create state.json.");
            return false;
        }
        output << stateToJson(state).dump(2);
        output.close();
        if (!output) {
            setError(error, L"Cannot write state.json.");
            return false;
        }

        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            setError(error, L"Cannot commit state.json.");
            return false;
        }
        return true;
    } catch (...) {
        setError(error, L"Cannot save state.json.");
        return false;
    }
}

bool parseUnsigned(const std::string &text, std::uint64_t *value) {
    if (!value || text.empty()) return false;
    char *end = nullptr;
    unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return false;
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool queryHeader(HINTERNET request, DWORD query, std::string *output) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &bytes,
                             WINHTTP_NO_HEADER_INDEX))
        return false;

    *output = wideToUtf8(buffer.data());
    return true;
}

bool parseContentRange(const std::string &text, std::uint64_t *start, std::uint64_t *endExclusive,
                       std::uint64_t *total) {
    if (text.rfind("bytes ", 0) != 0) return false;
    std::size_t dash = text.find('-', 6);
    std::size_t slash = text.find('/', dash);
    std::uint64_t endInclusive = 0;
    if (dash == std::string::npos || slash == std::string::npos || !parseUnsigned(text.substr(6, dash - 6), start) ||
        !parseUnsigned(text.substr(dash + 1, slash - dash - 1), &endInclusive) ||
        !parseUnsigned(text.substr(slash + 1), total) || *start > endInclusive || endInclusive >= *total)
        return false;
    *endExclusive = endInclusive + 1;
    return true;
}

bool parseHttpUrl(const std::string &url, std::wstring *host, std::wstring *path, INTERNET_PORT *port, bool *secure) {
    std::wstring wideUrl = utf8ToWide(url);
    if (wideUrl.empty()) return false;

    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS))
        return false;

    host->assign(parts.lpszHostName, parts.dwHostNameLength);
    *path = parts.dwUrlPathLength ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    if (parts.dwExtraInfoLength) path->append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    *port = parts.nPort;
    *secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return !host->empty();
}

bool httpRequest(const std::string &url, const std::wstring &headers, bool readBody, bool requireRange,
                 std::uint64_t expectedStart, std::uint64_t expectedEnd, std::uint64_t expectedTotal,
                 const std::function<bool(const unsigned char *, std::size_t)> &callback,
                 const std::atomic_bool &cancelled, HttpResponse *response, std::wstring *error) {
    if (cancelled.load()) {
        setError(error, L"Download cancelled.");
        return false;
    }

    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool secure = false;
    if (!parseHttpUrl(url, &host, &path, &port, &secure)) {
        setError(error, L"Invalid HTTP URL.");
        return false;
    }

    HttpHandle session(WinHttpOpen(L"NativePlayer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.get()) {
        setError(error, L"Cannot open WinHTTP session.");
        return false;
    }
    WinHttpSetTimeouts(session.get(), 10000, 10000, 10000, 30000);

    HttpHandle connection(WinHttpConnect(session.get(), host.c_str(), port, 0));
    if (!connection.get()) {
        setError(error, L"Cannot connect to HTTP server.");
        return false;
    }

    HttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request.get()) {
        setError(error, L"Cannot create HTTP request.");
        return false;
    }

    LPCWSTR headerData = headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str();
    DWORD headerLength = headers.empty() ? 0 : static_cast<DWORD>(-1);

    if (!WinHttpSendRequest(request.get(), headerData, headerLength, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        setError(error, L"HTTP request failed.");
        return false;
    }

    HttpResponse info;
    DWORD statusBytes = sizeof(info.status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &info.status, &statusBytes, WINHTTP_NO_HEADER_INDEX);

    std::string value;
    if (queryHeader(request.get(), WINHTTP_QUERY_CONTENT_LENGTH, &value)) parseUnsigned(value, &info.contentLength);
    queryHeader(request.get(), WINHTTP_QUERY_ETAG, &info.etag);
    queryHeader(request.get(), WINHTTP_QUERY_LAST_MODIFIED, &info.lastModified);

    DWORD finalBytes = 0;
    WinHttpQueryOption(request.get(), WINHTTP_OPTION_URL, nullptr, &finalBytes);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<wchar_t> finalUrl(finalBytes / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryOption(request.get(), WINHTTP_OPTION_URL, finalUrl.data(), &finalBytes))
            info.finalUrl = wideToUtf8(finalUrl.data());
    }

    if (queryHeader(request.get(), WINHTTP_QUERY_CONTENT_RANGE, &value) &&
        parseContentRange(value, &info.rangeStart, &info.rangeEnd, &info.contentLength))
        info.supportsRange = true;

    if (response) *response = info;

    if (info.status < 200 || info.status >= 300) {
        setError(error, L"HTTP status " + std::to_wstring(info.status));
        return false;
    }

    if (requireRange && (info.status != 206 || !info.supportsRange || info.rangeStart != expectedStart ||
                         info.rangeEnd != expectedEnd || (expectedTotal && info.contentLength != expectedTotal))) {
        setError(error, L"Server returned an unexpected byte range.");
        return false;
    }

    if (!readBody) return true;

    std::vector<unsigned char> buffer(kBufferSize);
    std::uint64_t received = 0;
    for (;;) {
        if (cancelled.load()) {
            setError(error, L"Download cancelled.");
            return false;
        }
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            setError(error, L"HTTP read failed.");
            return false;
        }
        if (read == 0) break;
        received += read;
        if (callback && !callback(buffer.data(), read)) return false;
    }

    if (requireRange && received != expectedEnd - expectedStart) {
        setError(error, L"Range response ended early.");
        return false;
    }
    if (!requireRange && info.contentLength && received != info.contentLength) {
        setError(error, L"HTTP response ended early.");
        return false;
    }
    return true;
}

bool probeResource(const std::string &url, const std::atomic_bool &cancelled, HttpResponse *response,
                   std::wstring *error) {
    return httpRequest(url, L"Range: bytes=0-0\r\n", false, false, 0, 1, 0, {}, cancelled, response, error);
}

bool getText(const std::string &url, const std::atomic_bool &cancelled, std::string *text, HttpResponse *response,
             std::wstring *error) {
    text->clear();
    bool tooLarge = false;
    bool ok = httpRequest(
        url, {}, true, false, 0, 0, 0,
        [&](const unsigned char *data, std::size_t size) {
            if (text->size() + size > kMaxPlaylistBytes) {
                tooLarge = true;
                return false;
            }
            text->append(reinterpret_cast<const char *>(data), size);
            return true;
        },
        cancelled, response, error);

    if (tooLarge) setError(error, L"HLS playlist exceeds 8 MB.");
    return ok;
}

bool resolveUrl(const std::string &base, const std::string &relative, std::string *result) {
    std::wstring wideBase = utf8ToWide(base);
    std::wstring wideRelative = utf8ToWide(relative);
    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    if (wideBase.empty() || wideRelative.empty() ||
        FAILED(UrlCombineW(wideBase.c_str(), wideRelative.c_str(), buffer.data(), &length, 0)))
        return false;
    *result = wideToUtf8(buffer.data());
    return !result->empty();
}

bool parsePlaylist(const std::string &text, const std::string &baseUrl, Playlist *playlist, std::wstring *error) {
    bool expectVariant = false;
    bool expectSegment = false;

    for (std::size_t offset = 0; offset <= text.size();) {
        std::size_t newline = text.find('\n', offset);
        std::string line = text.substr(offset, newline == std::string::npos ? std::string::npos : newline - offset);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (playlist->lines.empty() && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);

        playlist->lines.push_back(line);
        std::string normalized = upper(trim(line));

        if (normalized == "#EXT-X-ENDLIST")
            playlist->endList = true;
        else if (normalized.rfind("#EXT-X-MEDIA:", 0) == 0 && normalized.find("URI=") != std::string::npos) {
            setError(error, L"HLS external renditions are not supported.");
            return false;
        } else if (normalized.rfind("#EXT-X-STREAM-INF:", 0) == 0)
            expectVariant = true;
        else if (normalized.rfind("#EXTINF:", 0) == 0)
            expectSegment = true;
        else if (normalized.rfind("#EXT-X-BYTERANGE", 0) == 0 || normalized.rfind("#EXT-X-MAP", 0) == 0) {
            setError(error, L"Only MPEG-TS HLS is supported.");
            return false;
        } else if (normalized.rfind("#EXT-X-KEY:", 0) == 0 && normalized.find("METHOD=NONE") == std::string::npos) {
            setError(error, L"Encrypted HLS is not supported.");
            return false;
        } else if (!normalized.empty() && normalized.front() != '#') {
            std::string resolved;
            if (!resolveUrl(baseUrl, trim(line), &resolved)) {
                setError(error, L"Cannot resolve HLS URL.");
                return false;
            }
            if (expectVariant) {
                playlist->variants.push_back(std::move(resolved));
                expectVariant = false;
            } else if (expectSegment) {
                playlist->uriLines.push_back(playlist->lines.size() - 1);
                playlist->segments.push_back(std::move(resolved));
                expectSegment = false;
            }
        }

        if (newline == std::string::npos) break;
        offset = newline + 1;
    }

    if (playlist->lines.empty() || upper(trim(playlist->lines.front())) != "#EXTM3U") {
        setError(error, L"Invalid HLS playlist.");
        return false;
    }
    return true;
}

bool loadMediaPlaylist(const std::string &url, const std::atomic_bool &cancelled, Playlist *playlist,
                       std::string *playlistText, HttpResponse *response, std::wstring *error) {
    std::string rootText;
    HttpResponse rootResponse;
    if (!getText(url, cancelled, &rootText, &rootResponse, error)) return false;

    Playlist root;
    std::string rootBase = rootResponse.finalUrl.empty() ? url : rootResponse.finalUrl;
    if (!parsePlaylist(rootText, rootBase, &root, error)) return false;

    if (root.variants.empty()) {
        if (!root.endList || root.segments.empty()) {
            setError(error, L"Only HLS VOD is supported.");
            return false;
        }
        *playlist = std::move(root);
        *playlistText = std::move(rootText);
        *response = std::move(rootResponse);
        return true;
    }

    for (const auto &variant : root.variants) {
        std::string mediaText;
        HttpResponse mediaResponse;
        if (!getText(variant, cancelled, &mediaText, &mediaResponse, error)) continue;

        Playlist media;
        std::string mediaBase = mediaResponse.finalUrl.empty() ? variant : mediaResponse.finalUrl;
        if (parsePlaylist(mediaText, mediaBase, &media, error) && media.variants.empty() && media.endList &&
            !media.segments.empty()) {
            *playlist = std::move(media);
            *playlistText = std::move(mediaText);
            *response = std::move(mediaResponse);
            return true;
        }
    }

    setError(error, L"No playable HLS variant was found.");
    return false;
}

std::vector<RangeState> createRanges(std::uint64_t total) {
    std::size_t count = total < kCheckpointBytes ? 1 : total < kCheckpointBytes * 4 ? 2 : kMaxWorkers;
    std::vector<RangeState> result;
    std::uint64_t base = total / count;
    std::uint64_t remainder = total % count;
    std::uint64_t offset = 0;

    for (std::size_t index = 0; index < count; ++index) {
        std::uint64_t size = base + (index < remainder ? 1 : 0);
        result.push_back({offset, offset + size, offset});
        offset += size;
    }
    return result;
}

bool sameRangeLayout(const std::vector<RangeState> &left, const std::vector<RangeState> &right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (left[i].start != right[i].start || left[i].end != right[i].end) return false;
    return true;
}

std::uint64_t coveredBytes(const std::vector<RangeState> &ranges) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> intervals;
    for (const auto &range : ranges)
        if (range.next > range.start) intervals.emplace_back(range.start, (std::min)(range.next, range.end));

    std::sort(intervals.begin(), intervals.end());
    std::uint64_t total = 0, start = 0, end = 0;
    bool active = false;
    for (const auto &interval : intervals) {
        if (!active) {
            start = interval.first;
            end = interval.second;
            active = true;
        } else if (interval.first <= end)
            end = (std::max)(end, interval.second);
        else {
            total += end - start;
            start = interval.first;
            end = interval.second;
        }
    }
    return active ? total + end - start : 0;
}

bool prepareFile(const fs::path &path, std::uint64_t size, bool preserve, std::wstring *error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              preserve ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        setError(error, L"Cannot create cache file.");
        return false;
    }

    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(size);
    bool ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) && SetEndOfFile(file);
    CloseHandle(file);
    if (!ok) setError(error, L"Cannot resize cache file.");
    return ok;
}

bool writeAt(HANDLE file, std::uint64_t offset, const unsigned char *data, std::size_t size) {
    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD written = 0;
    return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
           WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
}

bool fileHasSize(const fs::path &path, std::uint64_t size) {
    std::error_code error;
    return fs::is_regular_file(path, error) && fs::file_size(path, error) == size && !error;
}

VodDownloadResult downloadFlv(const std::string &url, const std::string &hash, const fs::path &directory,
                              DownloadState state, const VodProgressCallback &progress,
                              const std::atomic_bool &cancelled) {
    std::wstring error;
    HttpResponse info;
    if (!probeResource(url, cancelled, &info, &error)) return fail(error);

    fs::path finalPath = directory / L"video.flv";
    fs::path partialPath = directory / L"video.flv.part";
    fs::path statePath = directory / L"state.json";

    if (!info.supportsRange || !info.contentLength) {
        ProgressReporter reporter(progress);
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            HANDLE file = CreateFileW(partialPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return fail(L"Cannot create FLV cache.");

            std::uint64_t offset = 0;
            bool ok = httpRequest(
                url, {}, true, false, 0, 0, 0,
                [&](const unsigned char *data, std::size_t size) {
                    DWORD written = 0;
                    if (!WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) || written != size)
                        return false;
                    offset += size;
                    reporter.report(offset, info.contentLength);
                    return !cancelled.load();
                },
                cancelled, nullptr, &error);
            FlushFileBuffers(file);
            CloseHandle(file);

            if (ok && offset > 0 && (!info.contentLength || offset == info.contentLength)) {
                if (!MoveFileExW(partialPath.c_str(), finalPath.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    return fail(L"Cannot publish FLV cache.");
                state = {};
                state.type = "flv";
                state.urlHash = hash;
                state.etag = info.etag;
                state.lastModified = info.lastModified;
                state.contentLength = offset;
                state.complete = true;
                if (!saveState(statePath, state, &error)) return fail(error);
                reporter.report(1, 1);
                return succeed(finalPath, false);
            }
            if (cancelled.load()) return fail(L"Download cancelled.");
            Sleep(250 * (attempt + 1));
        }
        return fail(error.empty() ? L"FLV download failed." : error);
    }

    std::vector<RangeState> layout = createRanges(info.contentLength);
    bool hasValidator = !info.etag.empty() || !info.lastModified.empty();
    bool resume = hasValidator && state.type == "flv" && state.urlHash == hash &&
                  state.contentLength == info.contentLength &&
                  state.etag == info.etag && state.lastModified == info.lastModified &&
                  sameRangeLayout(state.ranges, layout) && fileHasSize(partialPath, info.contentLength);

    if (!resume) {
        state = {};
        state.type = "flv";
        state.urlHash = hash;
        state.etag = info.etag;
        state.lastModified = info.lastModified;
        state.contentLength = info.contentLength;
        state.ranges = std::move(layout);
    }

    if (!prepareFile(partialPath, info.contentLength, resume, &error) || !saveState(statePath, state, &error))
        return fail(error);

    ProgressReporter reporter(progress);
    reporter.report(coveredBytes(state.ranges), state.contentLength);

    std::mutex stateMutex;
    std::mutex errorMutex;
    std::atomic_bool failed{false};
    std::wstring firstError;
    std::vector<std::thread> workers;
    std::string validator = !info.etag.empty() ? info.etag : info.lastModified;

    auto setFailure = [&](const std::wstring &value) {
        std::lock_guard<std::mutex> lock(errorMutex);
        if (firstError.empty()) firstError = value;
        failed.store(true);
    };

    for (std::size_t index = 0; index < state.ranges.size(); ++index) {
        if (state.ranges[index].next >= state.ranges[index].end) continue;

        workers.emplace_back([&, index] {
            HANDLE file = CreateFileW(partialPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                setFailure(L"Cannot open FLV cache range.");
                return;
            }

            for (int attempt = 0; attempt < kMaxAttempts && !cancelled.load() && !failed.load(); ++attempt) {
                std::uint64_t current;
                std::uint64_t end;
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    current = state.ranges[index].next;
                    end = state.ranges[index].end;
                }
                if (current >= end) break;

                std::uint64_t checkpoint = current;
                bool localFailure = false;
                std::wstring requestError;

                bool ok = httpRequest(
                    url,
                    L"Range: bytes=" + std::to_wstring(current) + L"-" + std::to_wstring(end - 1) + L"\r\n" +
                        (validator.empty() ? L"" : L"If-Range: " + utf8ToWide(validator) + L"\r\n"),
                    true, true, current, end, state.contentLength,
                    [&](const unsigned char *data, std::size_t size) {
                        if (!writeAt(file, current, data, size)) {
                            localFailure = true;
                            requestError = L"Cannot write FLV cache.";
                            return false;
                        }
                        current += size;

                        if (current == end || current - checkpoint >= kCheckpointBytes) {
                            if (!FlushFileBuffers(file)) {
                                localFailure = true;
                                requestError = L"Cannot flush FLV cache.";
                                return false;
                            }

                            std::uint64_t completed;
                            {
                                std::lock_guard<std::mutex> lock(stateMutex);
                                state.ranges[index].next = current;
                                if (!saveState(statePath, state, &requestError)) {
                                    localFailure = true;
                                    return false;
                                }
                                completed = coveredBytes(state.ranges);
                            }
                            reporter.report(completed, state.contentLength);
                            checkpoint = current;
                        }
                        return !cancelled.load() && !failed.load();
                    },
                    cancelled, nullptr, &requestError);

                if (ok && current == end) break;
                if (localFailure) {
                    setFailure(requestError);
                    break;
                }
                if (attempt + 1 == kMaxAttempts)
                    setFailure(requestError.empty() ? L"FLV range failed after retries." : requestError);
                else
                    Sleep(250 * (attempt + 1));
            }
            CloseHandle(file);
        });
    }

    for (auto &worker : workers)
        worker.join();
    if (cancelled.load()) return fail(L"Download cancelled.");
    if (failed.load()) return fail(firstError);
    if (coveredBytes(state.ranges) != state.contentLength) return fail(L"FLV download is incomplete.");

    if (!MoveFileExW(partialPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return fail(L"Cannot publish FLV cache.");

    state.complete = true;
    if (!saveState(statePath, state, &error)) return fail(error);
    reporter.report(1, 1);
    return succeed(finalPath, false);
}

bool writeLocalPlaylist(const fs::path &path, const Playlist &playlist, const DownloadState &state) {
    fs::path temporary = path;
    temporary += L".part";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return false;

    std::size_t segment = 0;
    for (std::size_t line = 0; line < playlist.lines.size(); ++line) {
        if (segment < playlist.uriLines.size() && playlist.uriLines[segment] == line)
            output << state.segments[segment++].file << '\n';
        else
            output << playlist.lines[line] << '\n';
    }
    output.close();
    return output && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

VodDownloadResult downloadHls(const std::string &url, const std::string &hash, const fs::path &directory,
                              DownloadState state, const VodProgressCallback &progress,
                              const std::atomic_bool &cancelled) {
    Playlist playlist;
    std::string playlistText;
    HttpResponse info;
    std::wstring error;
    if (!loadMediaPlaylist(url, cancelled, &playlist, &playlistText, &info, &error)) return fail(error);

    std::string playlistHash;
    std::string mediaUrl = info.finalUrl.empty() ? url : info.finalUrl;
    if (!createUrlHash(mediaUrl + "\n" + playlistText, &playlistHash))
        return fail(L"Cannot hash HLS playlist.");

    fs::path statePath = directory / L"state.json";
    fs::path segmentDirectory = directory / L"segments";
    fs::path indexPath = directory / L"index.m3u8";

    bool resume = state.type == "hls" && state.urlHash == hash && state.playlistHash == playlistHash &&
                  state.segments.size() == playlist.segments.size();

    if (!resume) {
        std::error_code ignored;
        fs::remove_all(segmentDirectory, ignored);
        fs::remove(indexPath, ignored);
        state = {};
        state.type = "hls";
        state.urlHash = hash;
        state.playlistHash = playlistHash;
        state.etag = info.etag;
        for (std::size_t index = 0; index < playlist.segments.size(); ++index) {
            char name[64] = {};
            sprintf_s(name, "segments/%06zu.ts", index);
            state.segments.push_back({name, false});
        }
    }

    fs::create_directories(segmentDirectory);
    std::uint64_t completeCount = 0;
    for (auto &segment : state.segments) {
        fs::path path = directory / utf8ToWide(segment.file);
        std::error_code fileError;
        segment.complete = segment.complete && fs::is_regular_file(path, fileError) &&
                           fs::file_size(path, fileError) > 0 && !fileError;
        if (segment.complete) ++completeCount;
    }

    if (!saveState(statePath, state, &error)) return fail(error);
    ProgressReporter reporter(progress);
    reporter.report(completeCount, state.segments.size());

    std::atomic_size_t next{0};
    std::atomic_bool failed{false};
    std::mutex stateMutex;
    std::mutex errorMutex;
    std::wstring firstError;

    auto setFailure = [&](const std::wstring &value) {
        std::lock_guard<std::mutex> lock(errorMutex);
        if (firstError.empty()) firstError = value;
        failed.store(true);
    };

    auto worker = [&] {
        for (;;) {
            std::size_t index = next.fetch_add(1);
            if (index >= state.segments.size() || cancelled.load() || failed.load()) return;
            if (state.segments[index].complete) continue;

            fs::path finalPath = directory / utf8ToWide(state.segments[index].file);
            fs::path temporary = finalPath;
            temporary += L".part";
            bool done = false;
            std::wstring requestError;

            for (int attempt = 0; attempt < kMaxAttempts && !done; ++attempt) {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                std::uint64_t received = 0;
                bool ok = output && httpRequest(
                                        playlist.segments[index], {}, true, false, 0, 0, 0,
                                        [&](const unsigned char *data, std::size_t size) {
                                            output.write(reinterpret_cast<const char *>(data),
                                                         static_cast<std::streamsize>(size));
                                            received += size;
                                            return output.good() && !cancelled.load();
                                        },
                                        cancelled, nullptr, &requestError);
                output.close();

                done = ok && output && received > 0 &&
                       MoveFileExW(temporary.c_str(), finalPath.c_str(),
                                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                if (!done && !cancelled.load()) Sleep(250 * (attempt + 1));
            }

            if (!done) {
                if (!cancelled.load())
                    setFailure(requestError.empty() ? L"HLS segment failed after retries." : requestError);
                return;
            }

            std::uint64_t completed = 0;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                state.segments[index].complete = true;
                for (const auto &segment : state.segments)
                    if (segment.complete) ++completed;
                if (!saveState(statePath, state, &requestError)) {
                    setFailure(requestError);
                    return;
                }
            }
            reporter.report(completed, state.segments.size());
        }
    };

    std::vector<std::thread> workers;
    std::size_t workerCount = (std::min)(kMaxWorkers, state.segments.size());
    for (std::size_t index = 0; index < workerCount; ++index)
        workers.emplace_back(worker);
    for (auto &thread : workers)
        thread.join();

    if (cancelled.load()) return fail(L"Download cancelled.");
    if (failed.load()) return fail(firstError);
    for (const auto &segment : state.segments)
        if (!segment.complete) return fail(L"HLS download is incomplete.");

    if (!writeLocalPlaylist(indexPath, playlist, state)) return fail(L"Cannot write local HLS playlist.");

    state.complete = true;
    if (!saveState(statePath, state, &error)) return fail(error);
    reporter.report(1, 1);
    return succeed(indexPath, false);
}

bool isHlsUrl(const std::string &url) {
    std::string value = upper(url.substr(0, url.find_first_of("?#")));
    return value.size() >= 5 && value.compare(value.size() - 5, 5, ".M3U8") == 0;
}

bool isFlvUrl(const std::string &url) {
    std::string value = upper(url.substr(0, url.find_first_of("?#")));
    return value.size() >= 4 && value.compare(value.size() - 4, 4, ".FLV") == 0;
}

bool validCachedHls(const fs::path &directory, const DownloadState &state) {
    if (!state.complete || !fs::exists(directory / L"index.m3u8")) return false;
    for (const auto &segment : state.segments) {
        std::error_code error;
        fs::path path = directory / utf8ToWide(segment.file);
        if (!segment.complete || !fs::is_regular_file(path, error) || fs::file_size(path, error) == 0 || error)
            return false;
    }
    return true;
}
} // namespace

VodDownloadResult downloadVod(const std::string &url, const VodProgressCallback &progress,
                              const std::atomic_bool &cancelled) {
    try {
        if (!isHlsUrl(url) && !isFlvUrl(url)) return fail(L"Only .flv and .m3u8 VOD URLs are supported.");

        std::string keyUrl = url.substr(0, url.find('#'));
        std::string hash;
        if (!createUrlHash(keyUrl, &hash)) return fail(L"Cannot create URL cache key.");

        std::wstring lockError;
        NamedMutex cacheLock;
        if (!cacheLock.acquire(L"Local\\NativePlayer.Download." + utf8ToWide(hash), cancelled, &lockError))
            return fail(lockError);

        fs::path directory = cacheRoot() / utf8ToWide(hash);
        fs::create_directories(directory);
        fs::path statePath = directory / L"state.json";

        DownloadState state;
        loadState(statePath, &state);

        if (state.urlHash == hash && state.complete) {
            if (state.type == "flv") {
                fs::path path = directory / L"video.flv";
                if (fileHasSize(path, state.contentLength)) {
                    HttpResponse remote;
                    std::wstring ignored;
                    if (probeResource(url, cancelled, &remote, &ignored)) {
                        bool sameLength = !remote.contentLength || remote.contentLength == state.contentLength;
                        bool sameEtag = state.etag.empty() || remote.etag == state.etag;
                        bool sameModified =
                            state.lastModified.empty() || remote.lastModified == state.lastModified;
                        if (sameLength && sameEtag && sameModified) {
                            if (progress) progress(100);
                            return succeed(path, true);
                        }
                    } else if (!cancelled.load()) {
                        if (progress) progress(100);
                        return succeed(path, true);
                    }
                }
            } else if (state.type == "hls" && validCachedHls(directory, state)) {
                Playlist playlist;
                std::string text;
                HttpResponse remote;
                std::wstring ignored;
                if (loadMediaPlaylist(url, cancelled, &playlist, &text, &remote, &ignored)) {
                    std::string remoteHash;
                    std::string mediaUrl = remote.finalUrl.empty() ? url : remote.finalUrl;
                    if (createUrlHash(mediaUrl + "\n" + text, &remoteHash) && remoteHash == state.playlistHash) {
                        if (progress) progress(100);
                        return succeed(directory / L"index.m3u8", true);
                    }
                } else if (!cancelled.load()) {
                    if (progress) progress(100);
                    return succeed(directory / L"index.m3u8", true);
                }
            }
        }

        if (cancelled.load()) return fail(L"Download cancelled.");
        return isHlsUrl(url) ? downloadHls(url, hash, directory, std::move(state), progress, cancelled)
                             : downloadFlv(url, hash, directory, std::move(state), progress, cancelled);
    } catch (const std::exception &exception) {
        return fail(utf8ToWide(exception.what()));
    } catch (...) {
        return fail(L"Unexpected download failure.");
    }
}
