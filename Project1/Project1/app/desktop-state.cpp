#include "desktop-state.h"
#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <regex>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::size_t maxFile = 1024 * 1024, maxSource = 2047;
std::string sha256(const std::string& bytes);
bool validTaskKey(const std::string& key) {
    return key.size() == 64 && std::all_of(key.begin(), key.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void close() { if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); value = INVALID_HANDLE_VALUE; } }
};
[[noreturn]] void ioError(const char* action, DWORD code = GetLastError()) {
    throw std::runtime_error(std::string(action) + " (Windows error " + std::to_string(code) + ").");
}
void invalid(const char* field) { throw std::runtime_error(std::string("Invalid desktop state field: ") + field + "."); }
bool utf8Text(const std::string& s, std::size_t limit, bool empty = false) {
    if (s.size() > limit || (!empty && s.empty())) return false;
    for (unsigned char c : s) if (c < 32 || c == 127) return false;
    return s.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        s.data(), static_cast<int>(s.size()), nullptr, 0) > 0;
}
fs::path dataPath(const std::string& root) {
    if (!utf8Text(root, 32767)) throw std::runtime_error("Invalid desktop data root.");
    return fs::absolute(fs::u8path(root)).lexically_normal();
}
std::string sourceForStorage(std::string value) {
    // Strip every query, including unknown signed-URL schemes; a denylist of
    // token keys cannot guarantee credentials are absent. Local '?' filenames
    // are legal on neither of this application's supported Windows filesystems.
    const auto scheme = value.find("://");
    if (scheme == std::string::npos) return value;
    value.erase(value.find_first_of("?#") == std::string::npos ? value.size() : value.find_first_of("?#"));
    const auto start = scheme + 3;
    const auto end = value.find_first_of("/\\", start);
    const auto at = value.find_last_of('@', end == std::string::npos ? value.size() : end);
    if (at != std::string::npos && at >= start) value.erase(start, at - start + 1);
    return value;
}
std::string sanitize(const std::string& input, std::size_t limit) {
    // Bound work even for multi-megabyte error strings. A clipped URL still
    // has its query/userinfo removed before output is clipped to the line cap.
    const bool clipped = input.size() > 16384;
    std::string text = input.substr(0, 16384);
    auto schemeChar = [](unsigned char c) { return (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'; };
    std::string cleaned; std::size_t offset = 0, search = 0;
    while ((search = text.find("://", search)) != std::string::npos) {
        auto start = search;
        while (start > offset && schemeChar(static_cast<unsigned char>(text[start - 1]))) --start;
        if (start == search) { search += 3; continue; }
        const auto end = text.find_first_of(" \t\r\n<>\"'", search + 3);
        const auto stop = end == std::string::npos ? text.size() : end;
        cleaned.append(text, offset, start - offset);
        // A truncated authority may hide its '@' beyond the work cap. Never
        // output the prefix of such a URL, which could itself be credentials.
        cleaned += clipped && end == std::string::npos ? "[truncated URL]" : sourceForStorage(text.substr(start, stop - start));
        offset = search = stop;
    }
    cleaned.append(text, offset, std::string::npos);
    static const std::regex secret(
        R"re((\b(?:authorization|proxy[-_]authorization|cookie|set[-_]cookie|password|passwd|pwd|(?:access[-_]?|refresh[-_]?|id[-_]?|auth[-_]?)token|api[-_]?key|secret|credentials?|signature|auth)["']?\s*[:=]\s*)(?:"[^"\r\n]*"|'[^'\r\n]*'|[^,;\r\n}]*))re",
        std::regex::icase);
    cleaned = std::regex_replace(cleaned, secret, "$1[redacted]");
    static const std::regex bearer(R"(\b(?:Bearer|Basic)\s+[^\s,;"'}]+)", std::regex::icase);
    cleaned = std::regex_replace(cleaned, bearer, "[redacted]");
    for (char& c : cleaned) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    if (cleaned.size() > limit) cleaned.resize(limit);
    // JSON dump also checks UTF-8, but log clipping must end at a character.
    while (!cleaned.empty() && !utf8Text(cleaned, limit, true)) cleaned.pop_back();
    return cleaned;
}
int integer(const Json& j, const char* key, int fallback, int low, int high) {
    auto i = j.find(key); if (i == j.end()) return fallback;
    if (!i->is_number_integer()) invalid(key);
    if (i->is_number_unsigned()) {
        const auto n = i->get<std::uint64_t>();
        if (n < static_cast<std::uint64_t>(low) || n > static_cast<std::uint64_t>(high)) invalid(key);
        return static_cast<int>(n);
    }
    const auto n = i->get<std::int64_t>(); if (n < low || n > high) invalid(key);
    return static_cast<int>(n);
}
float real(const Json& j, const char* key, float fallback, double low, double high) {
    auto i = j.find(key); if (i == j.end()) return fallback;
    if (!i->is_number()) invalid(key);
    const auto n = i->get<double>(); if (!std::isfinite(n) || n < low || n > high) invalid(key);
    return static_cast<float>(n);
}
bool boolean(const Json& j, const char* key, bool fallback) {
    auto i = j.find(key); if (i == j.end()) return fallback;
    if (!i->is_boolean()) invalid(key); return i->get<bool>();
}
std::string text(const Json& j, const char* key, const std::string& fallback,
    std::size_t limit, bool required = false) {
    auto i = j.find(key); if (i == j.end()) { if (required) invalid(key); return fallback; }
    if (!i->is_string()) invalid(key);
    const auto s = i->get<std::string>(); if (!utf8Text(s, limit)) invalid(key); return s;
}
std::uint64_t counter(const Json& j, const char* key) {
    auto i = j.find(key); if (i == j.end() || !i->is_number_integer()) invalid(key);
    if (!i->is_number_unsigned() && i->get<std::int64_t>() < 0) invalid(key);
    return i->get<std::uint64_t>();
}
DesktopState decode(const Json& j) {
    if (!j.is_object()) invalid("record");
    DesktopState s;
    s.decoder = integer(j, "decoder", s.decoder, 0, 2);
    s.brightness = real(j, "brightness", s.brightness, .25, 2);
    s.volume = real(j, "volume", s.volume, 0, 1);
    s.rotation = integer(j, "rotation", s.rotation, 0, 270);
    if (s.rotation % 90) invalid("rotation");
    s.zoom = real(j, "zoom", s.zoom, .5, 3);
    s.filter = integer(j, "filter", s.filter, 0, 2);
    s.networkMode = integer(j, "networkMode", s.networkMode, 0, 2);
    s.showInfo = boolean(j, "showInfo", s.showInfo);
    s.host = text(j, "host", s.host, 127);
    for (unsigned char c : s.host) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':' || c == '[' || c == ']')) invalid("host");
    s.port = integer(j, "port", s.port, 1, 65535);
    if (auto i = j.find("recent"); i != j.end()) {
        if (!i->is_array() || i->size() > 10) invalid("recent");
        for (const auto& item : *i) {
            if (!item.is_string()) invalid("recent");
            const auto source = item.get<std::string>(); if (!utf8Text(source, maxSource)) invalid("recent");
            // Recent entries are directly playable strings. Unlike downloads
            // they have no requiresSource flag, so omit altered sources.
            const auto clean = sourceForStorage(source);
            if (clean == source) s.recent.push_back(source);
        }
    }
    if (auto i = j.find("downloads"); i != j.end()) {
        if (!i->is_array() || i->size() > 100) invalid("downloads");
        for (const auto& item : *i) {
            if (!item.is_object()) invalid("downloads");
            const auto original = text(item, "source", {}, maxSource, true);
            DownloadRecord d; d.source = sourceForStorage(original);
            d.total = counter(item, "total"); d.downloaded = counter(item, "downloaded");
            if (d.downloaded > d.total) invalid("downloaded");
            d.state = sanitize(text(item, "state", {}, 64, true), 64);
            const bool previouslyRedacted = boolean(item, "requiresSource", false);
            d.requiresSource = previouslyRedacted || d.source != original;
            if (auto key = item.find("taskKey"); key != item.end()) {
                if (!key->is_string()) invalid("taskKey");
                d.taskKey = key->get<std::string>();
                if (!validTaskKey(d.taskKey)) invalid("taskKey");
            }
            // An original source is known for new/unredacted records and for
            // legacy JSON which still contains private URL parts. Compute its
            // key before discarding those parts. Never invent a key for an
            // already-redacted legacy record with requiresSource=true.
            if (!previouslyRedacted || d.source != original) {
                const auto originalKey = sha256(original);
                if (!d.taskKey.empty() && d.taskKey != originalKey) invalid("taskKey");
                d.taskKey = originalKey;
            }
            s.downloads.push_back(std::move(d));
        }
    }
    return s;
}
Json encode(const DesktopState& s) {
    if (!std::isfinite(s.brightness) || !std::isfinite(s.volume) || !std::isfinite(s.zoom)) invalid("settings");
    Json j{{"decoder", s.decoder}, {"brightness", s.brightness}, {"volume", s.volume},
        {"rotation", s.rotation}, {"zoom", s.zoom}, {"filter", s.filter},
        {"networkMode", s.networkMode}, {"showInfo", s.showInfo}, {"host", s.host},
        {"port", s.port}, {"recent", s.recent}, {"downloads", Json::array()}};
    for (const auto& d : s.downloads) {
        Json record{{"source", d.source}, {"total", d.total}, {"downloaded", d.downloaded},
            {"state", d.state}, {"requiresSource", d.requiresSource}};
        // An absent key is supported solely for legacy redacted history. The
        // validator derives every known original source key on load/save.
        if (!d.taskKey.empty()) record["taskKey"] = d.taskKey;
        j["downloads"].push_back(std::move(record));
    }
    return j;
}
std::string fileBytes(HANDLE file, std::size_t limit) {
    LARGE_INTEGER size{}; if (!GetFileSizeEx(file, &size)) ioError("Cannot inspect desktop record");
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > limit)
        throw std::runtime_error("Desktop record exceeds size limit.");
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    if (!bytes.empty() && (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) || got != bytes.size()))
        ioError("Cannot read desktop record");
    return bytes;
}
std::string uniqueSuffix() {
    static std::atomic<std::uint64_t> sequence{0};
    return std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++);
}
std::string cleanupName() {
    static std::atomic<std::uint64_t> sequence{0};
    char suffix[48]{};
    sprintf_s(suffix, "%08lx-%016llx-%016llx", static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long long>(GetTickCount64()), static_cast<unsigned long long>(sequence++));
    // Fixed maximum length 57, shorter than the original 64-character task key.
    return std::string(".desktop-clean-") + suffix;
}
void atomicWrite(const fs::path& destination, const std::string& bytes) {
    auto temporary = destination; temporary += L"." + fs::u8path(uniqueSuffix()).wstring() + L".tmp";
    // CREATE_NEW ensures a stale temporary file cannot be followed/overwritten.
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) ioError("Cannot create desktop temporary record");
    DWORD written = 0;
    if (!WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
        written != bytes.size() || !FlushFileBuffers(file.value)) {
        const auto code = GetLastError(); FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition));
        ioError("Cannot flush desktop record", code);
    }
    const auto name = destination.wstring();
    const auto renameSize = offsetof(FILE_RENAME_INFO, FileName) + (name.size() + 1) * sizeof(wchar_t);
    std::vector<unsigned char> renameBytes(renameSize, 0);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(renameBytes.data());
    rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
    DWORD last = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 6; ++attempt) {
        // POSIX replacement keeps share-delete readers on the old file object
        // while new opens find the new record. MoveFileEx can return transient
        // ACCESS_DENIED under this exact read/replace pattern.
        if (SetFileInformationByHandle(file.value, FileRenameInfoEx, rename, static_cast<DWORD>(renameSize))) return;
        last = GetLastError();
        if (last != ERROR_SHARING_VIOLATION && last != ERROR_ACCESS_DENIED && last != ERROR_LOCK_VIOLATION) break;
        if (attempt != 5) Sleep(20);
    }
    FILE_DISPOSITION_INFO disposition{TRUE};
    SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition));
    ioError("Cannot replace desktop record", last);
}
std::string sha256(const std::string& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize cache task hash.");
    struct AlgorithmCloser { BCRYPT_ALG_HANDLE h; ~AlgorithmCloser() { BCryptCloseAlgorithmProvider(h, 0); } } closer{algorithm};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot create cache task hash.");
    struct HashCloser { BCRYPT_HASH_HANDLE h; ~HashCloser() { BCryptDestroyHash(h); } } hashCloser{hash};
    std::array<unsigned char, 32> digest{};
    if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) < 0 ||
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        throw std::runtime_error("Cannot compute cache task hash.");
    std::string result; constexpr char hex[] = "0123456789abcdef";
    for (auto b : digest) { result += hex[b >> 4]; result += hex[b & 15]; } return result;
}
bool contained(const fs::path& parent, const fs::path& child) {
    auto p = parent.begin(), c = child.begin();
    for (; p != parent.end(); ++p, ++c) if (c == child.end() ||
        _wcsicmp(p->c_str(), c->c_str()) != 0) return false;
    return c != child.end();
}
using Handles = std::vector<std::unique_ptr<Handle>>;
std::unique_ptr<Handle> pin(const fs::path& path, bool directory, bool deleting, bool exclusive = false) {
    auto h = std::make_unique<Handle>(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES |
        (deleting ? DELETE : 0) | (exclusive ? GENERIC_READ | GENERIC_WRITE : 0),
        exclusive ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (h->value == INVALID_HANDLE_VALUE) ioError(exclusive ? "Cache task is active or task.lock cannot be opened" : "Cannot pin cache path");
    BY_HANDLE_FILE_INFORMATION info{}; if (!GetFileInformationByHandle(h->value, &info)) ioError("Cannot inspect cache path");
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Cache cleanup rejects reparse points.");
    if (bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory)
        throw std::runtime_error("Cache cleanup encountered an unexpected path type.");
    return h;
}
// Pin every existing ancestor, even above the requested root: canonical path
// checks alone do not prevent a junction replacement after inspection.
bool pinAncestors(const fs::path& path, Handles& pins) {
    fs::path current = path.root_path();
    pins.push_back(pin(current, true, false));
    for (const auto& part : path.relative_path()) {
        current /= part;
        const DWORD attrs = GetFileAttributesW(current.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const auto code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return false;
            ioError("Cannot inspect cache ancestor", code);
        }
        pins.push_back(pin(current, true, false));
    }
    return true;
}
struct Entry { fs::path path; std::unique_ptr<Handle> handle; bool directory; };
void collect(const fs::path& parent, const fs::path& task, const fs::path& lock,
    std::vector<Entry>& entries, unsigned depth = 0) {
    if (depth > 64) throw std::runtime_error("Cache directory nesting exceeds cleanup limit.");
    for (const auto& item : fs::directory_iterator(parent)) {
        const auto path = item.path();
        if (_wcsicmp(path.c_str(), lock.c_str()) == 0) continue;
        if (entries.size() >= 10000) throw std::runtime_error("Cache entry count exceeds cleanup limit.");
        const auto attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) ioError("Cannot inspect cache entry");
        const bool dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        auto handle = pin(path, dir, true);
        if (!contained(task, fs::canonical(path))) throw std::runtime_error("Cache entry escapes task directory.");
        entries.push_back({path, std::move(handle), dir});
        if (dir) collect(path, task, lock, entries, depth + 1);
    }
}
void deleteHandle(Handle& h) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(h.value, FileDispositionInfo, &disposition, sizeof(disposition)))
        ioError("Cannot remove inactive cache entry");
    h.close();
}
void renameDirectory(Handle& handle, const fs::path& destination) {
    const auto name = destination.wstring();
    const auto size = offsetof(FILE_RENAME_INFO, FileName) + (name.size() + 1) * sizeof(wchar_t);
    std::vector<unsigned char> buffer(size, 0);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->ReplaceIfExists = FALSE; rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
    if (!SetFileInformationByHandle(handle.value, FileRenameInfo, rename, static_cast<DWORD>(size)))
        ioError("Cannot isolate inactive cache task for cleanup");
}
}

DesktopState loadDesktopState(const std::string& root, std::string& error) {
    error.clear();
    try {
        const auto path = dataPath(root) / "desktop-state.json";
        Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (file.value == INVALID_HANDLE_VALUE) {
            const auto code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                // A missing file is normal, a non-directory ancestor is not.
                auto ancestor = path.parent_path();
                while (!ancestor.empty()) {
                    const auto attrs = GetFileAttributesW(ancestor.c_str());
                    if (attrs != INVALID_FILE_ATTRIBUTES) {
                        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("Desktop data root is blocked by a file.");
                        return {};
                    }
                    if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
                        ioError("Cannot inspect desktop data root");
                    const auto next = ancestor.parent_path(); if (next == ancestor) break; ancestor = next;
                }
                return {};
            }
            ioError("Cannot open desktop state", code);
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file.value, &info)) ioError("Cannot inspect desktop state");
        if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Desktop state must be a regular file.");
        return decode(Json::parse(fileBytes(file.value, maxFile)));
    } catch (const Json::exception&) { error = "Desktop state JSON is corrupt or has invalid UTF-8; using defaults."; }
    catch (const fs::filesystem_error& e) { error = "Cannot access desktop state (system error " + std::to_string(e.code().value()) + ")."; }
    catch (const std::exception& e) { error = e.what(); }
    return {};
}
bool saveDesktopState(const std::string& root, const DesktopState& state, std::string& error) {
    error.clear();
    try {
        // Validate the original first; then serialize only sanitized known fields.
        const auto clean = decode(encode(state));
        const auto bytes = encode(clean).dump(2) + "\n";
        if (bytes.size() > maxFile) throw std::runtime_error("Desktop state exceeds size limit.");
        const auto directory = dataPath(root); fs::create_directories(directory);
        atomicWrite(directory / "desktop-state.json", bytes); return true;
    } catch (const Json::exception&) { error = "Desktop state contains invalid JSON values or UTF-8."; }
    catch (const fs::filesystem_error& e) { error = "Cannot save desktop state (system error " + std::to_string(e.code().value()) + ")."; }
    catch (const std::exception& e) { error = e.what(); }
    return false;
}
bool appendDesktopLog(const std::string& root, const std::string& event,
    const std::string& details, std::string& error) {
    error.clear();
    try {
        const auto directory = dataPath(root); fs::create_directories(directory);
        SYSTEMTIME utc{}; GetSystemTime(&utc); char timestamp[40]{};
        sprintf_s(timestamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ ", utc.wYear,
            utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
        const auto line = std::string(timestamp) + sanitize(event, 256) + " " + sanitize(details, 8192) + "\n";
        const auto path = directory / "desktop.log";
        Handle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (file.value == INVALID_HANDLE_VALUE) ioError("Cannot open desktop log");
        BY_HANDLE_FILE_INFORMATION info{}; if (!GetFileInformationByHandle(file.value, &info)) ioError("Cannot inspect desktop log");
        if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Desktop log must be a regular file.");
        LARGE_INTEGER size{}, at{}; if (!GetFileSizeEx(file.value, &size)) ioError("Cannot inspect desktop log size");
        if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) + line.size() > maxFile) {
            if (!SetFilePointerEx(file.value, at, nullptr, FILE_BEGIN) || !SetEndOfFile(file.value)) ioError("Cannot truncate desktop log");
        } else if (!SetFilePointerEx(file.value, at, nullptr, FILE_END)) ioError("Cannot seek desktop log");
        DWORD written = 0;
        if (!WriteFile(file.value, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) || written != line.size())
            ioError("Cannot append desktop log");
        return true;
    } catch (const fs::filesystem_error& e) { error = "Cannot access desktop log (system error " + std::to_string(e.code().value()) + ")."; }
    catch (const std::exception& e) { error = e.what(); }
    return false;
}
void appendDesktopLog(const std::string& root, const std::string& event, const std::string& details) {
    std::string error; appendDesktopLog(root, event, details, error);
}
namespace {
bool removeCacheTask(const std::string& root, const std::string& key) {
        if (!validTaskKey(key)) throw std::runtime_error("Invalid cache task key; expected 64 lowercase hex characters.");
        const auto cache = dataPath(root) / "cache";
        Handles ancestors;
        if (!pinAncestors(cache, ancestors)) return true;
        const auto canonicalCache = fs::canonical(cache);
        const auto task = cache / key;
        const auto attrs = GetFileAttributesW(task.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const auto code = GetLastError(); if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
            ioError("Cannot inspect cache task", code);
        }
        auto directory = pin(task, true, true);
        const auto canonicalTask = fs::canonical(task);
        if (!contained(canonicalCache, canonicalTask)) throw std::runtime_error("Cache task escapes cache root.");
        const auto lockPath = task / "task.lock";
        // OPEN_ALWAYS matches the production cache's cross-process protocol;
        // directory/task are pinned so this cannot follow a replacement path.
        Handle creation(CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
            0, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (creation.value == INVALID_HANDLE_VALUE) ioError("Cache task is active or task.lock cannot be opened");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(creation.value, &info)) ioError("Cannot inspect cache task lock");
        if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Cache cleanup rejects invalid task.lock.");
        std::vector<Entry> entries;
        collect(task, canonicalTask, lockPath, entries);
        // Windows refuses directory renames while children are open. Release
        // the inspection handles and lock before rename, keeping ancestors and
        // the directory pinned. If a cache starts in this window, its open
        // exclusive task.lock prevents the rename; no content has been deleted.
        const auto isolated = cache / fs::u8path(cleanupName());
        entries.clear(); creation.close();
        renameDirectory(*directory, isolated);
        try {
            // Revalidate after isolation; a newly created original hash folder
            // belongs to a fresh task and is never part of this cleanup.
            auto isolatedLock = pin(isolated / "task.lock", false, true, true);
            collect(isolated, fs::canonical(isolated), isolated / "task.lock", entries);
            for (auto i = entries.rbegin(); i != entries.rend(); ++i) deleteHandle(*i->handle);
            deleteHandle(*isolatedLock); deleteHandle(*directory); return true;
        } catch (...) {
            // If cleanup failed (including a reparse introduced during the
            // rename window), restore the original directory when still free.
            // Never replace a fresh task created by another instance.
            entries.clear();
            try { renameDirectory(*directory, task); } catch (...) {}
            throw;
        }
}
}
bool removeDesktopCacheByKey(const std::string& root, const std::string& key, std::string& error) {
    error.clear();
    try { return removeCacheTask(root, key); }
    catch (const fs::filesystem_error& e) { error = "Cannot clean cache task (system error " + std::to_string(e.code().value()) + ")."; }
    catch (const std::exception& e) { error = e.what(); }
    return false;
}
bool removeDesktopCache(const std::string& root, const std::string& source, std::string& error) {
    error.clear();
    try {
        if (!utf8Text(source, maxSource)) throw std::runtime_error("Invalid cache source.");
        return removeDesktopCacheByKey(root, sha256(source), error);
    } catch (const std::exception& e) { error = e.what(); }
    return false;
}
