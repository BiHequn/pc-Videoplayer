#include "app/playback-session.h"
#include <chrono>
#include <iostream>
#include <thread>
#include <stdexcept>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> void waitFor(Predicate condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("state timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "pass a local video fixture");
        PlaybackSession player;
        require(player.snapshot().state == PlaybackState::Idle, "initial idle");
        player.open("Z:/missing/native-player-test.mp4", VideoDecoderBackend::Ffmpeg);
        waitFor([&] { return player.snapshot().state == PlaybackState::Error; });
        require(!player.snapshot().error.empty(), "error must be visible");
        player.open(argv[1], VideoDecoderBackend::Ffmpeg);
        waitFor([&] { return player.snapshot().state == PlaybackState::Playing; });
        require(player.snapshot().duration > 2.0, "duration");
        waitFor([&] { return player.frame() != nullptr; });
        require(player.frame()->width > 0, "published pixels");
        player.pause(true);
        waitFor([&] { return player.snapshot().state == PlaybackState::Paused; });
        const double position = player.snapshot().position;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        require(player.snapshot().position - position < .1, "pause advances position");
        const auto priorFrame = player.frame()->serial;
        player.seek(2.0);
        waitFor([&] { return player.snapshot().position > 1.9; });
        require(player.snapshot().state == PlaybackState::Paused, "seek loses pause");
        waitFor([&] { return player.frame() && player.frame()->serial > priorFrame; });
        require(player.snapshot().position >= 1.9 && player.snapshot().position < 2.1, "paused seek preview has wrong timestamp");
        player.pause(false);
        waitFor([&] { return player.snapshot().state == PlaybackState::Playing; });
        waitFor([&] { return player.snapshot().state == PlaybackState::Ended; });
        player.open(argv[1], VideoDecoderBackend::Ffmpeg);
        waitFor([&] { return player.snapshot().state == PlaybackState::Playing; });
        player.pause(true);
        waitFor([&] { return player.snapshot().state == PlaybackState::Paused; });
        const auto tailSerial = player.frame()->serial;
        const double tailTarget = player.snapshot().duration - .01;
        player.seek(tailTarget);
        waitFor([&] { return player.frame() && player.frame()->serial > tailSerial; });
        require(player.snapshot().state == PlaybackState::Paused, "EOF preview loses pause");
        require(player.snapshot().position >= tailTarget - .001, "EOF preview rolls time back to frame start");
        player.stop();
        waitFor([&] { return player.snapshot().state == PlaybackState::Idle; });
        require(!player.frame(), "stop keeps old frame");
        player.open(argv[1], VideoDecoderBackend::Ffmpeg);
        player.stop();
        player.open(argv[1], VideoDecoderBackend::Ffmpeg);
        waitFor([&] { return player.snapshot().state == PlaybackState::Playing; });
        player.setRate(2.0);
        waitFor([&] { return player.snapshot().rate == 2.0; });
        const auto fastStart=std::chrono::steady_clock::now();
        waitFor([&] { return player.snapshot().state == PlaybackState::Ended; });
        require(std::chrono::steady_clock::now()-fastStart < std::chrono::milliseconds(3000), "2x playback throttled by audio");
        std::cout << "PASS session: error/reopen, frame, pause, seek, resume, end, replay, stop\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
