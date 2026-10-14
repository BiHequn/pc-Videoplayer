#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
enum class CacheState { Idle, Preparing, Downloading, Paused, Complete, Cancelled, Error };
struct CacheSnapshot {
    CacheState state = CacheState::Idle;
    std::string source, playbackUrl, error;
    std::uint64_t downloaded = 0, total = 0;
    double bytesPerSecond = 0;
    std::size_t segmentsComplete = 0, segmentsTotal = 0;
    bool waitingForData = false;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> ranges;
};
class ProgressiveCache {
public:
    ProgressiveCache(); ~ProgressiveCache();
    // Lifecycle calls are serialized by the owner; start probes synchronously.
    // Read snapshots only after start returns. requestAbort may interrupt start.
    bool start(const std::string& url, const std::string& root);
    void pause(); void resume(); void cancel(); void stop();
    // May be called while start() is probing resources on a background thread.
    void requestAbort();
    CacheSnapshot snapshot() const;
    std::string playbackUrl() const;
private: struct Impl; std::unique_ptr<Impl> impl_;
};
