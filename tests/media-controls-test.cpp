#include "media/ffmpeg-decoder.h"
#include "media/ffmpeg-runtime.h"

#include <chrono>
#include <atomic>
#include <cmath>
#include <limits>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <Windows.h>
#include <mmsystem.h>
#include <fcntl.h>
#include <io.h>

extern "C"
{
#include <libavformat/avformat.h>
}

namespace
{
using Clock = std::chrono::steady_clock;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct MediaScope
{
    ~MediaScope() { closeFfmpegVideo(); }
};

void openSoftware(const char* path)
{
    require(openFfmpegVideo(path,
        createVideoDecoder(VideoDecoderBackend::Ffmpeg),
        VideoDecoderBackend::Ffmpeg), "real software open failed");
}

void validateFixture(const char* path, bool expectAudio)
{
    AVFormatContext* format = nullptr;
    require(avformat_open_input(&format, path, nullptr, nullptr) >= 0,
        "cannot inspect real media fixture");
    struct FormatScope
    {
        AVFormatContext** format;
        ~FormatScope() { avformat_close_input(format); }
    } scope{&format};
    require(avformat_find_stream_info(format, nullptr) >= 0,
        "cannot inspect fixture stream information");
    require(av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) >= 0,
        "fixture has no video stream");
    const bool hasAudio = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO,
        -1, -1, nullptr, 0) >= 0;
    require(hasAudio == expectAudio,
        expectAudio ? "first fixture must contain audio" : "second fixture must be silent");
}

DecodedVideoFrame waitForFrame()
{
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    DecodedVideoFrame frame;
    while (Clock::now() < deadline)
    {
        if (decodeNextFfmpegFrame(&frame))
        {
            require(frame.data && frame.width > 0 && frame.height > 0 &&
                frame.stride >= frame.width * 4, "decoded BGRA frame is invalid");
            return frame;
        }
        require(getFfmpegPlaybackEndReason() != PlaybackEndReason::DecodeError &&
            getFfmpegPlaybackEndReason() != PlaybackEndReason::DemuxError,
            "backend failed before producing a real frame");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("timed out waiting for a real decoded frame");
}

void silentClockAdvancesAtNormalRate(const char* path)
{
    MediaScope scope;
    openSoftware(path);
    const auto frame = waitForFrame();
    require(getFfmpegPlaybackClockSeconds() < 0.0,
        "fresh video clock was already valid before its first anchor");
    anchorFfmpegVideoClock(frame.ptsSeconds);
    require(std::abs(getFfmpegPlaybackClockSeconds() - frame.ptsSeconds) < 0.01,
        "silent clock did not anchor to its first real frame");
    startFfmpegPlayback();
    require(getFfmpegAudioClockSeconds() < 0.0,
        "silent fixture unexpectedly has a usable audio clock");
    const double before = getFfmpegPlaybackClockSeconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    advanceFfmpegPlaybackClock();
    const double elapsed = getFfmpegPlaybackClockSeconds() - before;
    require(elapsed >= 0.08 && elapsed < 1.0,
        "silent 1x playback clock froze; elapsed=" + std::to_string(elapsed));
}

void durationAndSeekBounds(const char* path)
{
    MediaScope scope;
    openSoftware(path);
    const double duration = getFfmpegDurationSeconds();
    require(std::isfinite(duration) && duration > 1.0,
        "fixture must have a known duration exceeding one second");
    AVFormatContext* metadata = nullptr;
    require(avformat_open_input(&metadata, path, nullptr, nullptr) >= 0,
        "could not read fixture duration independently");
    const int metadataResult = avformat_find_stream_info(metadata, nullptr);
    const double formatDuration = static_cast<double>(metadata->duration) / AV_TIME_BASE;
    avformat_close_input(&metadata);
    require(metadataResult >= 0, "could not discover independent container duration");
    require(std::abs(duration - formatDuration) < 0.001,
        "reported duration differs from the real container duration");
    waitForFrame();
    setFfmpegAudioPaused(true);
    require(seekFfmpegToPosition(-10.0), "negative seek should clamp to zero");
    require(std::abs(getFfmpegPlaybackClockSeconds()) < 0.001,
        "negative seek did not clamp the clock to zero");
    require(seekFfmpegToPosition(duration + 100.0), "seek beyond EOF should clamp");
    require(std::abs(getFfmpegPlaybackClockSeconds() - duration) < 0.001,
        "seek beyond EOF did not clamp the clock to duration");
    require(!seekFfmpegToPosition(std::numeric_limits<double>::quiet_NaN()),
        "NaN seek must fail");
    require(!seekFfmpegToPosition(std::numeric_limits<double>::infinity()),
        "infinite seek must fail");
    require(seekFfmpegToPosition(duration * 0.5), "absolute midpoint seek failed");
    setFfmpegAudioPaused(false);
    const auto frame = waitForFrame();
    require(frame.ptsSeconds >= duration * 0.5 && frame.ptsSeconds < duration * 0.5 + 0.05,
        "accurate seek exposed keyframe preroll: " + std::to_string(frame.ptsSeconds));
    require(seekFfmpegToPosition(0.0), "seek back to beginning failed");
    require(waitForFrame().ptsSeconds < 0.2, "seek did not reset actual demux position");
}

void accurateAudioSeek(const char* path)
{
    MediaScope scope;
    openSoftware(path);
    waitForFrame();
    // 2.013 falls inside a PCM packet (50 * 1920 samples = 2 seconds).
    // The retained audio must start at sample 96624, never at the keyframe.
    constexpr double target = 2.013;
    require(seekFfmpegToPosition(target), "fractional audio seek failed");
    const auto frame = waitForFrame();
    require(frame.ptsSeconds >= target && frame.ptsSeconds < 2.08,
        "fractional seek delivered pre-target video: " + std::to_string(frame.ptsSeconds));
    startFfmpegPlayback();
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    double audioClock = -1.0;
    while (Clock::now() < deadline && (audioClock = getFfmpegAudioClockSeconds()) < 0.0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(audioClock >= target && audioClock < target + 0.15,
        "PCM/audio clock retained pre-target samples: " + std::to_string(audioClock));
    require(getFfmpegPlaybackClockSeconds() >= target,
        "audio master clock pulled accurate seek before its target");
}

struct CancellationScope
{
    std::atomic<bool> interrupted{false};
    CancellationScope()
    {
        setFfmpegCancellationProbe([](void* user)
        {
            return static_cast<CancellationScope*>(user)->interrupted.load();
        }, this);
    }
    ~CancellationScope()
    {
        closeFfmpegVideo();
        setFfmpegCancellationProbe(nullptr, nullptr);
    }
};

void cancellationSurvivesOpenReset(const char* path)
{
    CancellationScope scope;
    scope.interrupted.store(true);
    require(!openFfmpegVideo(path, createVideoDecoder(VideoDecoderBackend::Ffmpeg),
        VideoDecoderBackend::Ffmpeg), "open reset erased external cancellation");
    require(scope.interrupted.load(), "backend modified external cancellation state");
    scope.interrupted.store(false);
    openSoftware(path);
    waitForFrame();
    scope.interrupted.store(true);
    require(!seekFfmpegToPosition(2.0), "seek restarted with external cancellation pending");
    closeFfmpegVideo();
    require(!openFfmpegVideo(path, createVideoDecoder(VideoDecoderBackend::Ffmpeg),
        VideoDecoderBackend::Ffmpeg), "close/reopen lost installed cancellation probe");
    scope.interrupted.store(false);
    openSoftware(path);
    waitForFrame();
}

void pausedPreviewAtTarget(const char* path, bool expectAudio)
{
    CancellationScope scope;
    openSoftware(path);
    const auto first = waitForFrame();
    anchorFfmpegVideoClock(first.ptsSeconds);
    startFfmpegPlayback();
    setFfmpegAudioPaused(true);
    DecodedVideoFrame preview;
    const auto before = Clock::now();
    require(previewFfmpegAtPosition(2.013, &preview), "paused seek preview failed");
    require(Clock::now() - before < std::chrono::seconds(5), "preview exceeded its bound");
    require(preview.data && preview.ptsSeconds >= 2.013 && preview.ptsSeconds < 2.08,
        "paused preview returned keyframe preroll: " + std::to_string(preview.ptsSeconds));
    const double position = getFfmpegPlaybackClockSeconds();
    startFfmpegPlayback();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    advanceFfmpegPlaybackClock();
    require(std::abs(getFfmpegPlaybackClockSeconds() - position) < 0.001,
        "preview did not restore paused output");
    const double audio = getFfmpegAudioClockSeconds();
    require(audio < 0.0 || (audio >= 2.013 && audio < 2.014),
        "preview played audio or retained pre-target PCM");
    setFfmpegAudioPaused(false);
    const auto next = waitForFrame();
    require(next.ptsSeconds >= preview.ptsSeconds,
        "preview resume exposed an earlier video frame");
    if (expectAudio)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(1);
        double audioClock = -1.0;
        while (Clock::now() < deadline && (audioClock = getFfmpegAudioClockSeconds()) < 0.0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(audioClock >= 2.013 && audioClock < 2.1,
            "preview resume did not trim audio to its target");
    }
    scope.interrupted.store(true);
    require(!previewFfmpegAtPosition(1.0, &preview), "preview ignored external cancellation");
}

void normalRateRestoresTargetAudio(const char* path)
{
    MediaScope scope;
    openSoftware(path);
    const auto first = waitForFrame();
    anchorFfmpegVideoClock(first.ptsSeconds);
    startFfmpegPlayback();
    // Exercise restarts with live output and a target inside a PCM packet.
    // The first valid WaveOut clock must retain the initial target samples.
    for (int attempt = 0; attempt < 12; ++attempt)
    {
        setFfmpegPlaybackRate(2.0);
        require(seekFfmpegToPosition(2.013), "2x resync seek failed");
        waitForFrame();
        // setRate advances the running 2x wall clock before choosing its seek
        // position. Anchor immediately here so decoder startup time cannot be
        // mistaken for samples discarded by the audio restart.
        anchorFfmpegVideoClock(2.013);
        const double target = getFfmpegPlaybackClockSeconds();
        setFfmpegPlaybackRate(1.0);
        const double resyncPosition = getFfmpegPlaybackClockSeconds();
        double audio = -1.0;
        const auto deadline = Clock::now() + std::chrono::seconds(1);
        while (Clock::now() < deadline && (audio = getFfmpegAudioClockSeconds()) < 0.0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(audio >= target && audio < target + 0.02,
            "1x restart discarded target PCM while still muted; target=" +
            std::to_string(target) + ", audio=" + std::to_string(audio));
        require(getFfmpegPlaybackClockSeconds() >= target,
            "restored 1x audio pulled the clock before the target");
        if (attempt == 0)
            std::cout << "1x resync: anchor=" << target << ", returned clock=" <<
                resyncPosition << ", first audio clock=" << audio << '\n';
    }
}

void pausedPreviewNearEof(const char* path)
{
    MediaScope scope;
    openSoftware(path);
    waitForFrame();
    startFfmpegPlayback();
    setFfmpegAudioPaused(true);
    const double target = getFfmpegDurationSeconds() - 0.01;
    DecodedVideoFrame preview;
    require(previewFfmpegAtPosition(target, &preview),
        "near-EOF preview discarded the final frame covering its target");
    require(preview.data && std::abs(preview.ptsSeconds - 3.96) < 0.001,
        "near-EOF preview did not return the real final frame");
    const double position = getFfmpegPlaybackClockSeconds();
    require(std::abs(position - target) < 0.001,
        "near-EOF preview pulled clock back to the last frame PTS");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    advanceFfmpegPlaybackClock();
    require(std::abs(getFfmpegPlaybackClockSeconds() - position) < 0.001,
        "near-EOF preview lost pause");
    require(getFfmpegAudioClockSeconds() < 0.0,
        "near-EOF preview submitted audible PCM");
    setFfmpegAudioPaused(false);
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!isFfmpegPlaybackFinished() && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(isFfmpegPlaybackFinished() &&
        getFfmpegPlaybackEndReason() == PlaybackEndReason::Completed,
        "near-EOF preview could not resume to natural completion");
}

void fastRateConsumesAllFrames(const char* path, double rate)
{
    MediaScope scope;
    openSoftware(path);
    const auto first = waitForFrame();
    anchorFfmpegVideoClock(first.ptsSeconds);
    startFfmpegPlayback();
    // Fill actual WaveOut headers before changing rate, so stale header waits
    // and an already blocked demux audio push are exercised too.
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    setFfmpegPlaybackRate(rate);
    const auto started = Clock::now();
    const auto deadline = started + std::chrono::duration<double>(4.0 / rate + 0.75);
    DecodedVideoFrame pending = first;
    double lastPts = first.ptsSeconds;
    int count = 1;
    while (Clock::now() < deadline)
    {
        advanceFfmpegPlaybackClock();
        if (pending.ptsSeconds <= getFfmpegPlaybackClockSeconds())
        {
            if (decodeNextFfmpegFrame(&pending))
            {
                lastPts = pending.ptsSeconds;
                ++count;
            }
            else if (isFfmpegPlaybackFinished())
            {
                require(getFfmpegPlaybackEndReason() == PlaybackEndReason::Completed,
                    "fast playback did not complete normally");
                require(count == 100 && lastPts >= 3.95,
                    "fast playback lost real frames/tail; count=" + std::to_string(count));
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error(std::to_string(rate) + "x video was throttled by muted audio; last PTS=" +
        std::to_string(lastPts));
}

void pauseFreezesClock(const char* path)
{
    MediaScope scope;
    openSoftware(path);
    const auto frame = waitForFrame();
    anchorFfmpegVideoClock(frame.ptsSeconds);
    startFfmpegPlayback();
    setFfmpegPlaybackRate(2.0);
    setFfmpegAudioPaused(true);
    const double before = getFfmpegPlaybackClockSeconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    advanceFfmpegPlaybackClock();
    require(std::abs(getFfmpegPlaybackClockSeconds() - before) < 0.005,
        "paused playback clock advanced at 2x");
    setFfmpegAudioPaused(false);
    advanceFfmpegPlaybackClock();
    require(std::abs(getFfmpegPlaybackClockSeconds() - before) < 0.04,
        "resume included time spent paused");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    advanceFfmpegPlaybackClock();
    const double advanced = getFfmpegPlaybackClockSeconds() - before;
    require(advanced >= 0.10 && advanced < 1.0, "resumed 2x clock did not advance");
    setFfmpegAudioPaused(true);
    const double paused = getFfmpegPlaybackClockSeconds();
    setFfmpegPlaybackRate(1.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    advanceFfmpegPlaybackClock();
    require(std::abs(getFfmpegPlaybackClockSeconds() - paused) < 0.01,
        "returning to 1x lost requested pause");
}

void stopAndReopenResetState(const char* path)
{
    MediaScope scope;
    openSoftware(path);
    const auto first = waitForFrame();
    anchorFfmpegVideoClock(first.ptsSeconds);
    startFfmpegPlayback();
    setFfmpegPlaybackRate(2.0);
    setFfmpegVolume(0.25f);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    setFfmpegAudioPaused(true);
    // Only the atomic abort is called from a separate (UI) thread.
    std::thread ui([] { requestFfmpegAbort(); });
    ui.join();
    const auto beforeClose = Clock::now();
    closeFfmpegVideo();
    require(Clock::now() - beforeClose < std::chrono::seconds(2),
        "paused stop did not join promptly");
    require(getFfmpegDurationSeconds() == 0.0, "closed duration was not cleared");
    require(!seekFfmpegToPosition(0.0), "seek without an open source succeeded");
    openSoftware(path);
    require(getFfmpegPlaybackRate() == 1.0, "reopen inherited previous playback rate");
    require(getFfmpegPlaybackClockSeconds() < 0.0,
        "reopen inherited the previous video clock before its first frame");
    const auto reopened = waitForFrame();
    anchorFfmpegVideoClock(reopened.ptsSeconds);
    require(std::abs(getFfmpegPlaybackClockSeconds() - reopened.ptsSeconds) < 0.01,
        "reopen did not anchor its first real frame");
}

void silentPlaybackCompletes(const char* path)
{
    MediaScope scope;
    openSoftware(path);
    const auto first = waitForFrame();
    anchorFfmpegVideoClock(first.ptsSeconds);
    startFfmpegPlayback();
    const auto deadline = Clock::now() + std::chrono::duration<double>(
        getFfmpegDurationSeconds() + 5.0);
    DecodedVideoFrame frame;
    int frameCount = 1;
    double lastPts = first.ptsSeconds;
    while (Clock::now() < deadline)
    {
        advanceFfmpegPlaybackClock();
        if (lastPts <= getFfmpegPlaybackClockSeconds() && decodeNextFfmpegFrame(&frame))
        {
            lastPts = frame.ptsSeconds;
            ++frameCount;
        }
        else if (lastPts <= getFfmpegPlaybackClockSeconds() && isFfmpegPlaybackFinished())
        {
            require(getFfmpegPlaybackEndReason() == PlaybackEndReason::Completed,
                "silent playback ended with an error");
            require(frameCount > 2 && lastPts > first.ptsSeconds,
                "silent playback did not deliver advancing real frames");
            require(lastPts >= getFfmpegDurationSeconds() - 0.2,
                "silent playback completed before delivering its tail frames");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("silent 1x playback did not complete within duration plus five seconds");
}

void nonseekableMediaReportsSeekFailure(const char* path)
{
    // Feed the actual audio/video fixture through FFmpeg's real pipe protocol.
    // Once drained, a seek back to the header requires unsupported pipe I/O.
    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    require(CreatePipe(&readHandle, &writeHandle, nullptr, 0) != FALSE,
        "could not create nonseekable media pipe");
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(readHandle),
        _O_RDONLY | _O_BINARY);
    if (descriptor < 0)
    {
        CloseHandle(readHandle);
        CloseHandle(writeHandle);
        throw std::runtime_error("could not open media pipe descriptor");
    }
    std::thread producer([=]
    {
        std::ifstream input(path, std::ios::binary);
        char buffer[4096];
        while (input)
        {
            input.read(buffer, sizeof(buffer));
            DWORD written = 0;
            const auto size = static_cast<DWORD>(input.gcount());
            if (size == 0 || !WriteFile(writeHandle, buffer, size, &written, nullptr))
                break;
        }
        CloseHandle(writeHandle);
    });
    struct PipeScope
    {
        int descriptor;
        std::thread& producer;
        ~PipeScope()
        {
            closeFfmpegVideo();
            _close(descriptor);
            producer.join();
        }
    } scope{descriptor, producer};
    const std::string source = "pipe:" + std::to_string(descriptor);
    openSoftware(source.c_str());
    waitForFrame();
    startFfmpegPlayback();
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    DecodedVideoFrame frame;
    while (!isFfmpegPlaybackFinished() && Clock::now() < deadline)
    {
        while (decodeNextFfmpegFrame(&frame)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(isFfmpegPlaybackFinished(), "nonseekable real media did not drain");
    require(!seekFfmpegToPosition(0.0),
        "real nonseekable av_seek_frame failure was reported as success");
    require(getFfmpegPlaybackEndReason() == PlaybackEndReason::DemuxError,
        "failed real seek did not publish a media error");
}
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: media-controls-test <media-with-audio> <silent-media>\n";
        return 2;
    }
    if (!initializeFfmpeg())
        return 2;
    int failures = 0;
    const auto run = [&](const char* name, auto test)
    {
        try
        {
            test();
            std::cout << "PASS " << name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failures;
            closeFfmpegVideo();
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };
    run("real audio media software decode", [&]
    {
        validateFixture(argv[1], true);
        validateFixture(argv[2], false);
        MediaScope scope;
        openSoftware(argv[1]);
        waitForFrame();
    });
    run("silent 1x clock advances", [&] { silentClockAdvancesAtNormalRate(argv[2]); });
    run("cancellation survives close/open/seek reset", [&] { cancellationSurvivesOpenReset(argv[1]); });
    run("paused long GOP preview and resume", [&] { pausedPreviewAtTarget(argv[1], true); });
    run("paused silent long GOP preview", [&] { pausedPreviewAtTarget(argv[2], false); });
    run("paused audiovisual preview near EOF", [&] { pausedPreviewNearEof(argv[1]); });
    run("paused silent preview near EOF", [&] { pausedPreviewNearEof(argv[2]); });
    run("long GOP fractional video and PCM seek", [&] { accurateAudioSeek(argv[1]); });
    run("2x consumes real audiovisual tail", [&] { fastRateConsumesAllFrames(argv[1], 2.0); });
    run("4x consumes real audiovisual tail", [&] { fastRateConsumesAllFrames(argv[1], 4.0); });
    run("2x to 1x restores initial target PCM", [&] { normalRateRestoresTargetAudio(argv[1]); });
    for (const char* path : {argv[1], argv[2]})
    {
        run("duration and absolute seek bounds", [&] { durationAndSeekBounds(path); });
        run("pause and resume clock", [&] { pauseFreezesClock(path); });
        run("paused abort stop and reopen", [&] { stopAndReopenResetState(path); });
    }
    run("software effective backend and volume controls", [&]
    {
        MediaScope scope;
        openSoftware(argv[1]);
        require(getFfmpegEffectiveBackend() == VideoDecoderBackend::Ffmpeg,
            "software backend was reported incorrectly");
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 2;
        format.nAvgBytesPerSec = 96000;
        HWAVEOUT probe = nullptr;
        require(waveOutOpen(&probe, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) ==
            MMSYSERR_NOERROR, "real audio device is required for volume regression");
        struct WaveScope
        {
            HWAVEOUT output;
            ~WaveScope() { waveOutClose(output); }
        } wave{probe};
        const auto requireVolume = [&](DWORD expected)
        {
            DWORD actual = 0;
            require(waveOutGetVolume(probe, &actual) == MMSYSERR_NOERROR,
                "could not query real WaveOut session volume");
            require(std::abs(static_cast<int>(actual & 0xffff) -
                static_cast<int>(expected)) < 300 &&
                std::abs(static_cast<int>(actual >> 16) -
                static_cast<int>(expected)) < 300,
                "actual WaveOut volume differs from requested value: " +
                std::to_string(actual));
        };
        setFfmpegVolume(-1.0f);
        requireVolume(0);
        setFfmpegVolume(2.0f);
        requireVolume(65535);
        setFfmpegVolume(0.4f);
        requireVolume(26214);
        setFfmpegVolume(std::numeric_limits<float>::quiet_NaN());
        requireVolume(26214);
        setFfmpegPlaybackRate(2.0);
        requireVolume(0);
        setFfmpegVolume(0.25f);
        requireVolume(0);
        setFfmpegPlaybackRate(1.0);
        requireVolume(16384);
        closeFfmpegVideo();
        openSoftware(argv[1]);
        requireVolume(65535);
        waitForFrame();
        startFfmpegPlayback();
        waitForFrame();
    });
    run("silent natural completion", [&] { silentPlaybackCompletes(argv[2]); });
    run("failed open then valid reopen", [&]
    {
        MediaScope scope;
        const std::string missing = std::string(argv[2]) + ".does-not-exist";
        require(!openFfmpegVideo(missing.c_str(),
            createVideoDecoder(VideoDecoderBackend::Ffmpeg), VideoDecoderBackend::Ffmpeg),
            "missing media unexpectedly opened");
        require(getFfmpegDurationSeconds() == 0.0, "failed open retained duration");
        openSoftware(argv[2]);
        waitForFrame();
    });
    run("real Auto backend selection", [&]
    {
        MediaScope scope;
        require(openFfmpegVideo(argv[2], createVideoDecoder(VideoDecoderBackend::Auto),
            VideoDecoderBackend::Auto), "real Auto media open failed");
        waitForFrame();
        AVFormatContext* metadata = nullptr;
        require(avformat_open_input(&metadata, argv[2], nullptr, nullptr) >= 0,
            "could not inspect Auto fixture");
        const int video = av_find_best_stream(metadata, AVMEDIA_TYPE_VIDEO,
            -1, -1, nullptr, 0);
        require(video >= 0, "Auto fixture lacks video");
        const AVCodecID codec = metadata->streams[video]->codecpar->codec_id;
        avformat_close_input(&metadata);
        const auto effective = getFfmpegEffectiveBackend();
        require(effective != VideoDecoderBackend::Auto,
            "effective backend still reports the requested Auto preference");
        if (codec != AV_CODEC_ID_H264 && codec != AV_CODEC_ID_HEVC)
            require(effective == VideoDecoderBackend::Ffmpeg,
                "unsupported MF codec did not report its actual software fallback");
    });
    run("real nonseekable seek failure", [&]
    {
        nonseekableMediaReportsSeekFailure(argv[1]);
    });
    shutdownFfmpeg();
    return failures == 0 ? 0 : 1;
}
