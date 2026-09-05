#pragma once

#include <atomic>
#include <functional>
#include <string>

struct VodDownloadResult {
    bool success = false;
    bool fromCache = false;
    std::string localPath;
    std::wstring error;
};

using VodProgressCallback = std::function<void(int percent)>;

VodDownloadResult downloadVod(const std::string &url, const VodProgressCallback &progress,
                              const std::atomic_bool &cancelled);