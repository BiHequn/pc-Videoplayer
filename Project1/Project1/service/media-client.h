#pragma once

#include "server-settings.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct RemoteVideoInfo {
    int id = 0;
    std::string filename;
    std::uint64_t filesize = 0;
    std::string md5;
    int playCount = 0;
    int likeCount = 0;
};

struct MediaResult {
    bool success = false;
    std::wstring error;
};

class MediaClient {
public:
    MediaClient();
    ~MediaClient();
    MediaClient(const MediaClient &) = delete;
    MediaClient &operator=(const MediaClient &) = delete;

    MediaResult connectAndAuthenticate(const ServerSettings &settings,
                                       const std::string &accessToken);
    MediaResult listVideos(std::vector<RemoteVideoInfo> *videos);
    MediaResult downloadVideo(int videoId,
                              const std::function<void(int)> &progress,
                              const std::atomic_bool &cancelled,
                              std::string *localPath);
    MediaResult reportPlay(int videoId);
    void close();

private:
    MediaResult request(const std::string &jsonRequest, int expectedCommand,
                        std::string *jsonResponse);
    std::uintptr_t socket_ = static_cast<std::uintptr_t>(-1);
    std::string accessToken_;
};
