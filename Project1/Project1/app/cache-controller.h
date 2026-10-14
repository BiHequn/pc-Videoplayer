#pragma once
#include "media/progressive-cache.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// Serializes the blocking cache lifecycle away from the window thread.
class CacheController {
public:
    CacheController();
    ~CacheController();
    void start(std::string url, std::string root);
    void pause(); void resume(); void cancel();
    CacheSnapshot snapshot() const;
    std::pair<std::uint64_t,CacheSnapshot> status() const;
private:
    enum class Action { Start, Pause, Resume, Cancel };
    struct Command { Action action; std::string url, root; std::uint64_t generation=0; std::shared_ptr<ProgressiveCache> cache; };
    void enqueue(Command command);
    void run();
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Command> commands_;
    bool quitting_=false, preparing_=false;
    std::uint64_t generation_=0,cancelledThrough_=0;
    CacheSnapshot preparingState_;
    std::shared_ptr<ProgressiveCache> cache_;
    std::shared_ptr<ProgressiveCache> pendingCache_;
    std::thread worker_;
};
