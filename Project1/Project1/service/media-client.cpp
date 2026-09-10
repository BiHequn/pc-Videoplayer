#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include "media-client.h"
#include "media-protocol.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace {
std::wstring utf8ToWide(const std::string &value) {
    if (value.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), length);
    return result;
}

std::string wideToUtf8(const std::wstring &value) {
    if (value.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0,
                                     nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

bool sendAll(SOCKET socket, const char *data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        int result = send(socket, data + sent,
                          static_cast<int>((std::min<std::size_t>)(size - sent, INT_MAX)), 0);
        if (result <= 0) return false;
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

bool receiveAll(SOCKET socket, char *data, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
        int result = recv(socket, data + received,
                          static_cast<int>((std::min<std::size_t>)(size - received, INT_MAX)), 0);
        if (result <= 0) return false;
        received += static_cast<std::size_t>(result);
    }
    return true;
}

std::string md5File(const fs::path &path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD resultSize = 0;
    DWORD digestSize = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                          &resultSize, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&digestSize), sizeof(digestSize),
                          &resultSize, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<unsigned char> object(objectSize);
    std::vector<unsigned char> digest(digestSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), buffer.size());
        std::streamsize count = input.gcount();
        if (count > 0)
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                           static_cast<ULONG>(count), 0);
    }
    BCryptFinishHash(hash, digest.data(), digestSize, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    static const char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (unsigned char byte : digest) {
        text.push_back(hex[byte >> 4]);
        text.push_back(hex[byte & 15]);
    }
    return text;
}

fs::path cacheRoot() {
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                                   SHGFP_TYPE_CURRENT, path)))
        return fs::path(path) / L"VideoPlayer1" / L"Cache" / L"Videos";
    return fs::temp_directory_path() / L"VideoPlayer1" / L"Cache" / L"Videos";
}

MediaResult failure(const std::wstring &message) {
    MediaResult result;
    result.error = message;
    return result;
}

bool isMd5(const std::string &value) {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
               (value >= 'A' && value <= 'F');
    });
}

class CacheMutex {
public:
    CacheMutex(const std::wstring &key, const std::atomic_bool &cancelled) {
        handle_ = CreateMutexW(nullptr, FALSE, (L"Local\\VideoPlayer1-" + key).c_str());
        if (!handle_) return;
        while (!cancelled.load()) {
            DWORD wait = WaitForSingleObject(handle_, 100);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
                locked_ = true;
                break;
            }
            if (wait != WAIT_TIMEOUT) break;
        }
    }
    ~CacheMutex() {
        if (locked_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }
    bool locked() const { return locked_; }
private:
    HANDLE handle_ = nullptr;
    bool locked_ = false;
};
}

MediaClient::MediaClient() {
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
}

MediaClient::~MediaClient() {
    close();
    WSACleanup();
}

MediaResult MediaClient::connectAndAuthenticate(const ServerSettings &settings,
                                                const std::string &accessToken) {
    close();
    std::string host = wideToUtf8(settings.host);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    std::string port = std::to_string(settings.mediaPort);
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0)
        return failure(L"无法解析媒体服务地址。");
    SOCKET connected = INVALID_SOCKET;
    for (addrinfo *address = addresses; address; address = address->ai_next) {
        SOCKET candidate = socket(address->ai_family, address->ai_socktype,
                                  address->ai_protocol);
        if (candidate == INVALID_SOCKET) continue;
        DWORD timeout = 10000;
        setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        if (::connect(candidate, address->ai_addr,
                      static_cast<int>(address->ai_addrlen)) == 0) {
            connected = candidate;
            break;
        }
        closesocket(candidate);
    }
    freeaddrinfo(addresses);
    if (connected == INVALID_SOCKET) return failure(L"无法连接媒体服务。");
    socket_ = static_cast<std::uintptr_t>(connected);
    accessToken_ = accessToken;
    nlohmann::json response;
    std::string responseText;
    MediaResult requestResult = request(
        nlohmann::json{{"cmd", 6}, {"access_token", accessToken_}}.dump(),
        1006,
        &responseText);
    if (!requestResult.success) return requestResult;
    try { response = nlohmann::json::parse(responseText); }
    catch (...) { return failure(L"媒体服务认证响应无效。"); }
    if (response.value("result", "") != "ok") {
        close();
        return failure(utf8ToWide(response.value("msg", "媒体服务认证失败")));
    }
    MediaResult result;
    result.success = true;
    return result;
}

MediaResult MediaClient::request(const std::string &jsonRequest, int expectedCommand,
                                 std::string *jsonResponse) {
    SOCKET socket = static_cast<SOCKET>(socket_);
    if (socket == INVALID_SOCKET) return failure(L"媒体服务未连接。");
    std::string payload = encodeMediaProtocol(jsonRequest);
    std::uint32_t size = htonl(static_cast<std::uint32_t>(payload.size()));
    if (!sendAll(socket, reinterpret_cast<const char *>(&size), sizeof(size)) ||
        !sendAll(socket, payload.data(), payload.size())) {
        close();
        return failure(L"发送媒体服务请求失败。");
    }
    std::uint32_t networkSize = 0;
    if (!receiveAll(socket, reinterpret_cast<char *>(&networkSize), sizeof(networkSize))) {
        close();
        return failure(L"读取媒体服务响应失败。");
    }
    std::uint32_t responseSize = ntohl(networkSize);
    if (responseSize == 0 || responseSize > 10 * 1024 * 1024) {
        close();
        return failure(L"媒体服务响应长度无效。");
    }
    std::string response(responseSize, '\0');
    if (!receiveAll(socket, response.data(), response.size())) {
        close();
        return failure(L"媒体服务响应不完整。");
    }
    *jsonResponse = decodeMediaProtocol(response);
    try {
        const nlohmann::json decoded = nlohmann::json::parse(*jsonResponse);
        if (!decoded.contains("cmd") || !decoded["cmd"].is_number_integer() ||
            decoded["cmd"].get<int>() != expectedCommand) {
            close();
            return failure(L"媒体服务响应命令不匹配。");
        }
    } catch (...) {
        close();
        return failure(L"媒体服务响应无效。");
    }
    MediaResult result;
    result.success = true;
    return result;
}

MediaResult MediaClient::listVideos(std::vector<RemoteVideoInfo> *videos) {
    if (!videos) return failure(L"视频列表输出参数无效。");
    std::string text;
    MediaResult result = request(nlohmann::json{{"cmd", 30},
                                                {"access_token", accessToken_}}.dump(), 1030, &text);
    if (!result.success) return result;
    try {
        nlohmann::json response = nlohmann::json::parse(text);
        if (response.value("result", "") != "ok")
            return failure(utf8ToWide(response.value("msg", "获取视频列表失败")));
        videos->clear();
        for (const auto &item : response["videos"]) {
            RemoteVideoInfo video;
            video.id = item.value("id", 0);
            video.filename = item.value("filename", "");
            video.filesize = item.value("filesize", static_cast<std::uint64_t>(0));
            video.md5 = item.value("file_md5", "");
            video.playCount = item.value("play_count", 0);
            video.likeCount = item.value("like_count", 0);
            videos->push_back(std::move(video));
        }
    } catch (...) { return failure(L"无法解析视频列表。"); }
    return result;
}

MediaResult MediaClient::downloadVideo(int videoId,
                                       const std::function<void(int)> &progress,
                                       const std::atomic_bool &cancelled,
                                       std::string *localPath) {
    try {
    if (!localPath || videoId <= 0) return failure(L"下载参数无效。");
    std::string text;
    MediaResult result = request(nlohmann::json{{"cmd", 23}, {"video_id", videoId},
                                                {"access_token", accessToken_}}.dump(), 1023, &text);
    if (!result.success) return result;
    nlohmann::json info;
    try { info = nlohmann::json::parse(text); }
    catch (...) { return failure(L"下载初始化响应无效。"); }
    if (info.value("result", "") != "ok")
        return failure(utf8ToWide(info.value("msg", "下载初始化失败")));
    if (!info.contains("filename") || !info["filename"].is_string() ||
        !info.contains("filesize") || !info["filesize"].is_number_unsigned() ||
        !info.contains("file_md5") || !info["file_md5"].is_string() ||
        !info.contains("chunk_size") || !info["chunk_size"].is_number_integer())
        return failure(L"下载初始化信息类型无效。");
    std::string filename = fs::path(utf8ToWide(info["filename"].get<std::string>())).filename().u8string();
    std::uint64_t fileSize = info["filesize"].get<std::uint64_t>();
    std::string expectedMd5 = info["file_md5"].get<std::string>();
    int chunkSize = info["chunk_size"].get<int>();
    if (filename.empty() || !fileSize || !isMd5(expectedMd5) || chunkSize <= 0 || chunkSize > 1024 * 1024)
        return failure(L"下载初始化信息不完整。");
    fs::path directory = cacheRoot();
    fs::create_directories(directory);
    fs::path extension = fs::path(utf8ToWide(filename)).extension();
    fs::path finalPath = directory /
        (std::to_wstring(videoId) + L"_" + utf8ToWide(expectedMd5) + extension.wstring());
    CacheMutex cacheMutex(std::to_wstring(videoId) + L"-" + utf8ToWide(expectedMd5), cancelled);
    if (!cacheMutex.locked())
        return failure(cancelled.load() ? L"下载已取消。" : L"无法锁定视频缓存。");
    std::error_code fileError;
    if (fs::is_regular_file(finalPath, fileError) &&
        fs::file_size(finalPath, fileError) == fileSize &&
        md5File(finalPath) == expectedMd5) {
        if (progress) progress(100);
        *localPath = finalPath.u8string();
        result.success = true;
        return result;
    }
    fs::path partialPath = finalPath;
    partialPath += L".part";
    fs::remove(finalPath, fileError);
    fs::remove(partialPath, fileError);
    std::ofstream output(partialPath, std::ios::binary | std::ios::trunc);
    if (!output) return failure(L"无法创建视频缓存文件。");
    std::uint64_t offset = 0;
    while (offset < fileSize && !cancelled.load()) {
        int size = static_cast<int>((std::min<std::uint64_t>)(chunkSize, fileSize - offset));
        result = request(nlohmann::json{{"cmd", 24}, {"video_id", videoId},
                                        {"offset", std::to_string(offset)}, {"size", size},
                                        {"access_token", accessToken_}}.dump(), 1024, &text);
        if (!result.success) break;
        nlohmann::json response;
        try { response = nlohmann::json::parse(text); }
        catch (...) { result = failure(L"下载分片响应无效。"); break; }
        if (response.value("result", "") != "ok") {
            result = failure(utf8ToWide(response.value("msg", "下载分片失败")));
            break;
        }
        if (response.value("video_id", 0) != videoId ||
            response.value("offset", static_cast<std::uint64_t>(-1)) != offset) {
            result = failure(L"下载分片顺序无效。");
            break;
        }
        std::string decodedBytes = decodeMediaBase64(response.value("data", ""));
        std::vector<unsigned char> bytes(decodedBytes.begin(), decodedBytes.end());
        if (bytes.empty() || offset + bytes.size() > fileSize) {
            result = failure(L"下载分片内容无效。");
            break;
        }
        output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        if (!output) { result = failure(L"写入视频缓存失败。"); break; }
        offset += bytes.size();
        if (progress) progress(static_cast<int>(offset * 100 / fileSize));
    }
    output.close();
    if (cancelled.load()) result = failure(L"下载已取消。");
    const std::string actualMd5 = md5File(partialPath);
    if (!result.success || offset != fileSize || actualMd5 != expectedMd5) {
        fs::remove(partialPath, fileError);
        if (result.success) result = failure(L"视频文件校验失败。");
        return result;
    }
    fs::rename(partialPath, finalPath, fileError);
    if (fileError) return failure(L"无法发布视频缓存文件。");
    *localPath = finalPath.u8string();
    result.success = true;
    return result;
    } catch (const std::exception &) {
        return failure(L"下载视频时发生本地文件或响应错误。");
    } catch (...) {
        return failure(L"下载视频时发生未知错误。");
    }
}

MediaResult MediaClient::reportPlay(int videoId) {
    std::string text;
    MediaResult result = request(
        nlohmann::json{{"cmd", 32}, {"video_id", videoId},
                       {"access_token", accessToken_}}.dump(), 1032, &text);
    if (!result.success) return result;
    try {
        nlohmann::json response = nlohmann::json::parse(text);
        if (response.value("result", "") != "ok")
            return failure(utf8ToWide(response.value("msg", "播放统计上报失败")));
    } catch (...) {
        return failure(L"播放统计响应无效。");
    }
    return result;
}

void MediaClient::close() {
    SOCKET socket = static_cast<SOCKET>(socket_);
    if (socket != INVALID_SOCKET) closesocket(socket);
    socket_ = static_cast<std::uintptr_t>(-1);
    accessToken_.clear();
}
