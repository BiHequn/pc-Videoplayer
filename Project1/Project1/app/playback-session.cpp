#include "playback-session.h"
#include "media/ffmpeg-decoder.h"
#include "media/ffmpeg-runtime.h"
#include "media/video-frame-copy.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

PlaybackSession::PlaybackSession() { worker_ = std::thread(&PlaybackSession::run, this); }
PlaybackSession::~PlaybackSession() {
    { std::lock_guard<std::mutex> lock(mutex_); quitting_ = true; }
    requestFfmpegAbort();
    condition_.notify_one();
    worker_.join();
}
void PlaybackSession::enqueue(Command command) {
    { std::lock_guard<std::mutex> lock(mutex_); commands_.push_back(std::move(command)); }
    condition_.notify_one();
}
void PlaybackSession::open(std::string source, VideoDecoderBackend backend, bool nativeYuv) {
    enqueue({Action::Open, std::move(source), backend, 0, nativeYuv});
}
void PlaybackSession::pause(bool paused) { enqueue({Action::Pause, {}, VideoDecoderBackend::Auto, paused ? 1.0 : 0.0}); }
void PlaybackSession::stop() { enqueue({Action::Stop}); }
void PlaybackSession::seek(double seconds) { if(std::isfinite(seconds)) enqueue({Action::Seek, {}, VideoDecoderBackend::Auto, seconds}); }
void PlaybackSession::setRate(double rate) { enqueue({Action::Rate, {}, VideoDecoderBackend::Auto, rate}); }
void PlaybackSession::setVolume(float volume) { enqueue({Action::Volume, {}, VideoDecoderBackend::Auto, std::clamp(volume, 0.0f, 1.0f)}); }
PlaybackSnapshot PlaybackSession::snapshot() const { std::lock_guard<std::mutex> lock(mutex_); return state_; }
std::shared_ptr<const PlaybackFrame> PlaybackSession::frame() const { std::lock_guard<std::mutex> lock(mutex_); return frame_; }
bool PlaybackSession::interrupted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if(quitting_) return true;
    for(const auto& command : commands_) if(command.action == Action::Open || command.action == Action::Stop) return true;
    return false;
}
void PlaybackSession::fail(const std::string& message) {
    closeFfmpegVideo();
    std::lock_guard<std::mutex> lock(mutex_);
    state_.state = PlaybackState::Error; state_.error = message;
}
void PlaybackSession::publishFrame(const DecodedVideoFrame& source, double pts) {
    QueuedVideoFrame copy;if(!copyVideoFrame(source,&copy))throw std::runtime_error("invalid decoded frame");
    auto output = std::make_shared<PlaybackFrame>();
    output->width = copy.width; output->height = copy.height; output->stride = copy.stride; output->serial = ++frameSerial_;
    output->pixels=std::move(copy.pixels);output->pixelFormat=copy.pixelFormat;output->planeOffsets=copy.planeOffsets;output->planeStrides=copy.planeStrides;output->colorSpace=copy.colorSpace;output->colorRange=copy.colorRange;
    std::lock_guard<std::mutex> lock(mutex_);
    frame_ = std::move(output); state_.width = source.width; state_.height = source.height;
    if(pts >= 0) state_.position = pts;
}
bool PlaybackSession::openMedia(const Command& command) {
    closeFfmpegVideo();
    setFfmpegNativeYuvOutput(command.nativeYuv);
    { std::lock_guard<std::mutex> lock(mutex_);
      const float volume = state_.volume; state_ = {}; state_.volume = volume;
      state_.source = command.source; state_.state = PlaybackState::Opening; frame_.reset(); }
    if(command.source.empty()) { fail("请选择视频文件。"); return false; }
    auto backend = command.backend;
    bool fallback = false;
    for(;;) {
        if(interrupted()) { closeFfmpegVideo(); return false; }
        auto decoder = createVideoDecoder(backend);
        bool opened = decoder && openFfmpegVideo(command.source.c_str(), std::move(decoder), backend);
        if(!opened) {
            if(command.backend == VideoDecoderBackend::Auto && !fallback && !interrupted()) {
                closeFfmpegVideo(); backend = VideoDecoderBackend::Ffmpeg; fallback = true; continue;
            }
            if(!interrupted()) fail("无法打开视频。请检查文件路径、文件格式或解码器设置。");
            return false;
        }
        DecodedVideoFrame first{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while(!decodeNextFfmpegFrame(&first)) {
            if(interrupted()) { closeFfmpegVideo(); return false; }
            if(isFfmpegVideoFinished() || std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if(!first.data) {
            if(command.backend == VideoDecoderBackend::Auto && !fallback) {
                closeFfmpegVideo(); backend = VideoDecoderBackend::Ffmpeg; fallback = true; continue;
            }
            fail("未能解码首帧。文件可能已损坏，或当前解码器不支持该视频。"); return false;
        }
        anchorFfmpegVideoClock(first.ptsSeconds);
        publishFrame(first, first.ptsSeconds);
        const float volume = snapshot().volume;
        setFfmpegVolume(volume); startFfmpegPlayback();
        { std::lock_guard<std::mutex> lock(mutex_); state_.duration = getFfmpegDurationSeconds();
          state_.backend = getFfmpegEffectiveBackend(); state_.state = PlaybackState::Playing; }
        return true;
    }
}
void PlaybackSession::run() {
    bool initialized = initializeFfmpeg();
    setFfmpegCancellationProbe([](void* session) { return static_cast<PlaybackSession*>(session)->interrupted(); }, this);
    DecodedVideoFrame pending{};
    bool hasPending = false;
    auto lastVideoReady = std::chrono::steady_clock::now();
    try {
      for(;;) {
        Command command{Action::Stop}; bool haveCommand = false;
        { std::unique_lock<std::mutex> lock(mutex_);
          if(commands_.empty() && !quitting_) condition_.wait_for(lock, std::chrono::milliseconds(5));
          if(quitting_) break;
          if(!commands_.empty()) { command = std::move(commands_.front()); commands_.pop_front(); haveCommand = true; } }
        if(haveCommand) {
            lastVideoReady = std::chrono::steady_clock::now();
            const auto before = snapshot();
            const bool active = before.state == PlaybackState::Playing || before.state == PlaybackState::Paused || before.state==PlaybackState::Buffering;
            if(command.action == Action::Open) {
                hasPending = false; pending = {};
                if(initialized) openMedia(command); else fail("FFmpeg 初始化失败。");
            } else if(command.action == Action::Stop) {
                closeFfmpegVideo(); hasPending = false; pending = {};
                std::lock_guard<std::mutex> lock(mutex_); state_.state = PlaybackState::Idle; state_.error.clear(); state_.position = 0; frame_.reset();
            } else if(command.action == Action::Pause && active) {
                setFfmpegBuffering(false);advanceFfmpegPlaybackClock(); setFfmpegAudioPaused(command.value != 0);
                std::lock_guard<std::mutex> lock(mutex_); state_.state = command.value != 0 ? PlaybackState::Paused : PlaybackState::Playing;
            } else if(command.action == Action::Seek && active) {
                const double target = before.duration > 0 ? std::clamp(command.value, 0.0, std::max(0.0, before.duration - .01)) : std::max(0.0, command.value);
                hasPending = false; pending = {};
                setFfmpegBuffering(false);
                if(before.state == PlaybackState::Paused) {
                    DecodedVideoFrame preview{};
                    if(!previewFfmpegAtPosition(target, &preview)) { if(!interrupted()) fail("无法预览指定位置，请重新打开文件。"); }
                    else publishFrame(preview,std::max(target,preview.ptsSeconds));
                } else if(!seekFfmpegToPosition(target)) { if(!interrupted()) fail("无法跳转到指定位置，请重新打开文件。"); }
                else { std::lock_guard<std::mutex> lock(mutex_); state_.position = target; }
            } else if(command.action == Action::Rate && active) {
                hasPending = false; pending = {}; setFfmpegPlaybackRate(command.value);
                setFfmpegAudioPaused(before.state == PlaybackState::Paused);
                std::lock_guard<std::mutex> lock(mutex_); state_.rate = getFfmpegPlaybackRate();
            } else if(command.action == Action::Volume) {
                setFfmpegVolume(static_cast<float>(command.value));
                std::lock_guard<std::mutex> lock(mutex_); state_.volume = static_cast<float>(command.value);
            }
        }
        const auto current=snapshot();
        if(current.state != PlaybackState::Playing && current.state!=PlaybackState::Buffering) continue;
        const auto terminalReason = getFfmpegPlaybackEndReason();
        if(terminalReason == PlaybackEndReason::DecodeError || terminalReason == PlaybackEndReason::DemuxError) {
            hasPending = false; pending = {};
            if(!interrupted()) fail(terminalReason == PlaybackEndReason::DemuxError ? "媒体读取失败，播放已停止。" : "视频解码失败，播放已停止。");
            continue;
        }
        advanceFfmpegPlaybackClock();
        // After video drains, let buffered tail audio finish even if the last
        // network starvation entered Buffering and no further video frame arrives.
        if(current.state==PlaybackState::Buffering && isFfmpegVideoFinished()) {
            setFfmpegBuffering(false);
            std::lock_guard<std::mutex> lock(mutex_);state_.state=PlaybackState::Playing;
        }
        const double clock = getFfmpegPlaybackClockSeconds();
        if(clock >= 0) { std::lock_guard<std::mutex> lock(mutex_); state_.position = state_.duration > 0 ? std::min(clock, state_.duration) : clock; }
        for(int count = 0; count < 12; ++count) {
            if(!hasPending) hasPending = decodeNextFfmpegFrame(&pending);
            if(hasPending) lastVideoReady = std::chrono::steady_clock::now();
            if(hasPending && current.state==PlaybackState::Buffering){setFfmpegBuffering(false);std::lock_guard<std::mutex> lock(mutex_);state_.state=PlaybackState::Playing;}
            if(!hasPending) {
                if(isFfmpegPlaybackFinished() || (getFfmpegPlaybackRate() != 1.0 && isFfmpegVideoFinished())) {
                    const auto reason = getFfmpegPlaybackEndReason();
                    if(reason == PlaybackEndReason::Completed) {
                        closeFfmpegVideo(); std::lock_guard<std::mutex> lock(mutex_);
                        state_.state = PlaybackState::Ended; if(state_.duration > 0) state_.position = state_.duration;
                    } else fail(reason == PlaybackEndReason::DemuxError ? "媒体读取失败，播放已停止。" : "视频解码失败，播放已停止。");
                }
                else if(std::strstr(current.source.c_str(),"://") && !isFfmpegVideoFinished() &&
                    std::chrono::steady_clock::now()-lastVideoReady > std::chrono::milliseconds(150) &&
                    getFfmpegPlaybackEndReason()==PlaybackEndReason::None) {
                    setFfmpegBuffering(true); std::lock_guard<std::mutex> lock(mutex_); state_.state=PlaybackState::Buffering;
                }
                break;
            }
            const double diff = pending.ptsSeconds >= 0 && clock >= 0 ? pending.ptsSeconds - clock : 0;
            { std::lock_guard<std::mutex> lock(mutex_); state_.syncDifference = diff; }
            if(diff > .025) break;
            hasPending = false;
            if(diff < -.15) { std::lock_guard<std::mutex> lock(mutex_); ++state_.droppedFrames; continue; }
            publishFrame(pending, pending.ptsSeconds);
            break;
        }
      }
    } catch(const std::exception&) { fail("播放线程异常。请关闭窗口后重新启动播放器。"); }
    closeFfmpegVideo(); setFfmpegCancellationProbe(nullptr, nullptr); if(initialized) shutdownFfmpeg();
}
