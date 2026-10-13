#pragma once
#include "media/video-decoder-factory.h"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

enum class PlaybackState { Idle, Opening, Playing, Paused, Ended, Error };
struct PlaybackSnapshot {
    PlaybackState state = PlaybackState::Idle;
    std::string source, error;
    double duration = 0, position = 0, rate = 1, syncDifference = 0;
    int width = 0, height = 0;
    float volume = 1;
    VideoDecoderBackend backend = VideoDecoderBackend::Auto;
    std::uint64_t droppedFrames = 0;
};
struct PlaybackFrame {
    std::vector<unsigned char> pixels;
    int width = 0, height = 0, stride = 0;
    std::uint64_t serial = 0;
};
class PlaybackSession {
public:
    PlaybackSession();
    ~PlaybackSession();
    PlaybackSession(const PlaybackSession&) = delete;
    PlaybackSession& operator=(const PlaybackSession&) = delete;
    void open(std::string source, VideoDecoderBackend backend);
    void pause(bool paused);
    void stop();
    void seek(double seconds);
    void setRate(double rate);
    void setVolume(float volume);
    PlaybackSnapshot snapshot() const;
    std::shared_ptr<const PlaybackFrame> frame() const;
private:
    enum class Action { Open, Pause, Stop, Seek, Rate, Volume };
    struct Command { Action action; std::string source; VideoDecoderBackend backend = VideoDecoderBackend::Auto; double value = 0; };
    void enqueue(Command command);
    void run();
    bool openMedia(const Command& command);
    bool interrupted() const;
    void publishFrame(const unsigned char* data, int width, int height, int stride, double pts);
    void fail(const std::string& message);
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Command> commands_;
    PlaybackSnapshot state_;
    std::shared_ptr<const PlaybackFrame> frame_;
    std::thread worker_;
    bool quitting_ = false;
    std::uint64_t frameSerial_ = 0;
};
