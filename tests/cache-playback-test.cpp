#include "media/ffmpeg-runtime.h"
#include "media/ffmpeg-decoder.h"
#include "media/loopback-http.h"
#include "media/progressive-cache.h"
#include <Windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include <libavformat/avformat.h>
}

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using json = nlohmann::json;
constexpr std::uint64_t blockSize = 512 * 1024;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void validateFixture(const fs::path& path, AVCodecID expectedVideo, bool audio) {
    require(fs::exists(path), "missing generated stream fixture: " + path.u8string());
    AVFormatContext* format = nullptr;
    require(avformat_open_input(&format, path.u8string().c_str(), nullptr, nullptr) >= 0,
        "cannot open generated fixture: " + path.u8string());
    struct Scope { AVFormatContext*& f; ~Scope() { avformat_close_input(&f); } } scope{format};
    require(avformat_find_stream_info(format, nullptr) >= 0, "fixture stream discovery failed");
    const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    require(video >= 0, "fixture has no video");
    const auto* stream = format->streams[video];
    require(stream->codecpar->codec_id == expectedVideo, "wrong built-in fixture video codec");
    require(stream->codecpar->width == 320 && stream->codecpar->height == 180, "wrong fixture dimensions");
    require(std::abs(av_q2d(stream->avg_frame_rate) - 25.0) < 0.1, "fixture must be 25 fps");
    require(format->duration >= 11.9 * AV_TIME_BASE && format->duration <= 12.2 * AV_TIME_BASE,
        "fixture must have about 12 seconds of media");
    const int sound = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    require((sound >= 0) == audio, "wrong fixture audio stream presence");
    if (audio) require(format->streams[sound]->codecpar->codec_id == AV_CODEC_ID_PCM_S16LE,
        "FLV fixture must contain supported little-endian PCM");
}
void validateFixtures(const fs::path& dir) {
    validateFixture(dir / "stream.flv", AV_CODEC_ID_FLV1, true);
    validateFixture(dir / "hls" / "index.m3u8", AV_CODEC_ID_MPEG2VIDEO, false);
    std::ifstream playlist(dir / "hls" / "index.m3u8");
    std::string line; int segments = 0; bool vod = false, end = false;
    while (std::getline(playlist, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        vod |= line == "#EXT-X-PLAYLIST-TYPE:VOD";
        end |= line == "#EXT-X-ENDLIST";
        if (!line.empty() && line.front() != '#') {
            require(fs::exists(dir / "hls" / line), "playlist references a missing segment");
            ++segments;
        }
    }
    require(vod && end && segments == 8, "HLS fixture must be a finite eight-segment VOD");
    std::cout << "PASS generated FLV1/PCM and MPEG-2 eight-segment HLS fixtures\n";
}

std::vector<unsigned char> readFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "cannot read file: " + path.u8string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
template<class Predicate>
void until(Predicate predicate, std::chrono::milliseconds timeout, const std::string& message) {
    const auto deadline = Clock::now() + timeout;
    while (!predicate()) {
        require(Clock::now() < deadline, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
struct Internet {
    HINTERNET value = nullptr;
    explicit Internet(HINTERNET handle) : value(handle) { require(value != nullptr, "WinHTTP handle allocation"); }
    ~Internet() { WinHttpCloseHandle(value); }
    Internet(const Internet&) = delete;
};
struct HttpReply {
    DWORD status = 0;
    std::string etag, contentRange;
    std::vector<unsigned char> bytes;
};
std::string httpHeader(HINTERNET request, DWORD key) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, key, nullptr, nullptr, &size, nullptr);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return {};
    std::wstring value(size / sizeof(wchar_t), 0);
    require(WinHttpQueryHeaders(request, key, nullptr, value.data(), &size, nullptr) != FALSE, "read HTTP header");
    while (!value.empty() && !value.back()) value.pop_back();
    std::string result;
    for (const auto c : value) {
        require(c < 128, "expected ASCII fixture HTTP header");
        result += static_cast<char>(c);
    }
    return result;
}
HttpReply get(const std::string& url, const std::string& range = {}, bool head = false,
    std::size_t limit = 64 * 1024 * 1024) {
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = DWORD(-1);
    require(WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) != FALSE, "parse loopback URL");
    Internet session(WinHttpOpen(L"stream-playback-test", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0));
    WinHttpSetTimeouts(session.value, 3000, 3000, 3000, 5000);
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength), path(parts.lpszUrlPath, parts.dwUrlPathLength);
    Internet connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
    Internet request(WinHttpOpenRequest(connection.value, head ? L"HEAD" : L"GET", path.c_str(), nullptr, nullptr, nullptr, 0));
    const std::string headers = range.empty() ? "" : "Range: bytes=" + range + "\r\n";
    const std::wstring wh(headers.begin(), headers.end());
    require(WinHttpSendRequest(request.value, wh.c_str(), DWORD(wh.size()), nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(request.value, nullptr), "loopback HTTP request failed");
    HttpReply reply; DWORD size = sizeof(reply.status);
    require(WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        nullptr, &reply.status, &size, nullptr) != FALSE, "HTTP status missing");
    reply.etag = httpHeader(request.value, WINHTTP_QUERY_ETAG);
    reply.contentRange = httpHeader(request.value, WINHTTP_QUERY_CONTENT_RANGE);
    if (!head) {
        unsigned char bytes[16384];
        while (reply.bytes.size() < limit) {
            DWORD count = 0;
            if (!WinHttpReadData(request.value, bytes, DWORD(std::min(sizeof(bytes), limit-reply.bytes.size())), &count) || !count) break;
            reply.bytes.insert(reply.bytes.end(), bytes, bytes+count);
        }
    }
    return reply;
}
struct OriginRequest {
    std::string method, path;
    std::uint64_t first = 0, last = 0, sent = 0;
    bool ranged = false;
};
class Origin {
public:
    explicit Origin(const fs::path& directory) {
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) files.emplace("/" + fs::relative(entry.path(), directory).generic_u8string(), readFile(entry.path()));
        }
        require(server.start([this](const LocalHttpRequest& req, LocalHttpResponse& res) { serve(req, res); }), "origin startup failed");
    }
    ~Origin() {
        stopping = true; holdLast = false; server.stop();
        if (!reportPath.empty()) { try { dump(reportPath); } catch (...) {} }
    }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(server.port()) + path; }
    std::size_t mark() const { std::lock_guard<std::mutex> lock(mutex); return requests.size(); }
    std::vector<OriginRequest> log() const { std::lock_guard<std::mutex> lock(mutex); return requests; }
    void dump(const fs::path& path) const {
        json rows = json::array();
        fs::create_directories(path.parent_path());
        for (const auto& r : log()) rows.push_back({{"method",r.method},{"path",r.path},{"first",r.first},{"last",r.last},{"sent",r.sent},{"ranged",r.ranged}});
        std::ofstream output(path); output << rows.dump(2);
    }
    std::map<std::string,std::vector<unsigned char>> files;
    fs::path reportPath;
    std::atomic<int> delayMs{16};
    std::atomic<bool> holdLast{false};
private:
    LoopbackHttp server;
    std::atomic<bool> stopping{false};
    mutable std::mutex mutex;
    std::vector<OriginRequest> requests;
    void serve(const LocalHttpRequest& req, LocalHttpResponse& res) {
        const auto found = files.find(req.path);
        if (found == files.end()) { res.headers(404, 0, "text/plain"); return; }
        if (req.method != "GET" && req.method != "HEAD") { res.headers(405, 0, "text/plain"); return; }
        const auto& bytes = found->second;
        std::uint64_t first = 0, last = bytes.size()-1;
        const bool partial = !req.range.empty();
        try {
            if (partial) {
                require(req.range.rfind("bytes=",0) == 0 && req.range.find(',') == std::string::npos, "bad range");
                const auto dash = req.range.find('-');
                require(dash != std::string::npos, "bad range");
                if (dash == 6) {
                    const auto suffix = std::stoull(req.range.substr(dash+1));
                    require(suffix > 0, "bad suffix"); first = bytes.size() > suffix ? bytes.size()-suffix : 0;
                } else {
                    first = std::stoull(req.range.substr(6,dash-6));
                    if (dash+1 < req.range.size()) last = std::min<std::uint64_t>(last,std::stoull(req.range.substr(dash+1)));
                }
                require(first <= last, "range out of bounds");
            }
        } catch (...) { res.headers(416,0,"text/plain","Content-Range: bytes */"+std::to_string(bytes.size())+"\r\n"); return; }
        std::size_t id;
        { std::lock_guard<std::mutex> lock(mutex); id = requests.size(); requests.push_back({req.method,req.path,first,last,0,partial}); }
        const bool playlist = req.path.find(".m3u8") != std::string::npos;
        const auto type = playlist ? "application/vnd.apple.mpegurl" : req.path.find(".ts") != std::string::npos ? "video/mp2t" : "video/x-flv";
        const std::string extra = "ETag: \"stream-fixture-v1-" + std::to_string(bytes.size()) + "\"\r\nAccept-Ranges: bytes\r\n" +
            (partial ? "Content-Range: bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" + std::to_string(bytes.size()) + "\r\n" : "");
        if (!res.headers(partial ? 206 : 200, last-first+1, type, extra) || req.method == "HEAD") return;
        for (auto pos = first; pos <= last && !stopping;) {
            // Metadata's 0-0 request must stay available even when a distant
            // fragment is deliberately held to exercise a waiting cache reader.
            while (holdLast && req.path == "/hls/segment-07.ts" && last > first && !stopping)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (stopping) return;
            const auto count = std::min<std::uint64_t>(8192,last-pos+1);
            if (!res.write(bytes.data()+pos,static_cast<std::size_t>(count))) return;
            pos += count;
            { std::lock_guard<std::mutex> lock(mutex); requests[id].sent += count; }
            if (!playlist && last > first) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs.load()));
        }
    }
};
struct PlaybackScope {
    ProgressiveCache& cache;
    ~PlaybackScope() { cache.cancel(); requestFfmpegAbort(); closeFfmpegVideo(); cache.stop(); }
};
void validFrame(const DecodedVideoFrame& frame) {
    require(frame.data && frame.width == 320 && frame.height == 180 && frame.stride >= 1280 &&
        frame.pixelFormat == VideoPixelFormat::Bgra && std::isfinite(frame.ptsSeconds), "invalid real BGRA frame");
    bool varied = false;
    for (int x = 4; x < frame.width*4; x += 4) varied |= frame.data[x] != frame.data[0];
    require(varied, "decoded frame contains no real fixture texture");
}
DecodedVideoFrame nextFrame(std::chrono::seconds timeout = std::chrono::seconds(10)) {
    DecodedVideoFrame frame;
    until([&] {
        require(getFfmpegPlaybackEndReason() != PlaybackEndReason::DecodeError &&
            getFfmpegPlaybackEndReason() != PlaybackEndReason::DemuxError, "demux/decode failed before frame");
        return decodeNextFfmpegFrame(&frame);
    }, std::chrono::duration_cast<std::chrono::milliseconds>(timeout), "timed out waiting for decoded frame");
    validFrame(frame); return frame;
}
void openPlayback(const std::string& url) {
    require(openFfmpegVideo(url.c_str(), createVideoDecoder(VideoDecoderBackend::Ffmpeg),
        VideoDecoderBackend::Ffmpeg), "openFfmpegVideo failed over progressive loopback");
    require(getFfmpegEffectiveBackend() == VideoDecoderBackend::Ffmpeg, "test must use FFmpeg backend");
}
void complete(ProgressiveCache& cache) {
    until([&] {
        const auto s = cache.snapshot();
        require(s.state != CacheState::Error, "cache failed: " + s.error);
        return s.state == CacheState::Complete;
    }, std::chrono::seconds(30), "cache did not finish");
}
void rewrittenPlaylist(ProgressiveCache& cache, Origin& origin, bool compareSegments) {
    const auto reply = get(cache.playbackUrl());
    require(reply.status == 200, "cached HLS playlist status");
    const std::string text(reply.bytes.begin(),reply.bytes.end());
    require(text.rfind("#EXTM3U",0) == 0 && text.find("#EXT-X-ENDLIST") != std::string::npos &&
        text.find("#EXT-X-PLAYLIST-TYPE:VOD") != std::string::npos, "rewritten HLS lost VOD metadata");
    const auto host = cache.playbackUrl().substr(0,cache.playbackUrl().find("/index.m3u8"));
    std::istringstream lines(text); std::string line; int index = 0;
    while (std::getline(lines,line)) if (!line.empty() && line.front() != '#') {
        require(line == "/segment/" + std::to_string(index), "HLS URI does not address cache-local fragment");
        if (compareSegments) {
            char path[64]; std::snprintf(path,sizeof(path),"/hls/segment-%02d.ts",index);
            const auto segment = get(host+line);
            require(segment.status == 200 && segment.bytes == origin.files.at(path), "cached HLS segment differs from generated TS");
        }
        ++index;
    }
    require(index == 8, "rewritten playlist lost fragments");
}
void playbackScenario(const fs::path& fixtures, const fs::path& root, bool hls) {
    Origin origin(fixtures);
    origin.reportPath = root/"origin-ranges.json";
    const auto path = hls ? "/hls/index.m3u8" : "/stream.flv";
    const auto head = get(origin.url(hls ? "/hls/segment-00.ts" : "/stream.flv"),{},true);
    require(head.status == 200 && !head.etag.empty() && head.bytes.empty(), "origin HEAD/ETag contract");
    const auto range = get(origin.url(hls ? "/hls/segment-00.ts" : "/stream.flv"),"100-199");
    const auto& original = origin.files.at(hls ? "/hls/segment-00.ts" : "/stream.flv");
    require(range.status == 206 && range.bytes.size() == 100 && !range.contentRange.empty() &&
        std::equal(range.bytes.begin(),range.bytes.end(),original.begin()+100), "origin exact Range contract");
    ProgressiveCache cache;
    require(cache.start(origin.url(path), root.u8string()), "cache start: " + cache.snapshot().error);
    PlaybackScope scope{cache};
    if (hls) rewrittenPlaylist(cache,origin,false);
    const auto started = Clock::now();
    openPlayback(cache.playbackUrl());
    const auto first = nextFrame(); const auto atFirst = cache.snapshot();
    std::cout << (hls ? "HLS" : "FLV") << " first frame PTS=" << first.ptsSeconds << " in " <<
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count() <<
        "ms downloaded=" << atFirst.downloaded << "/" << atFirst.total << '\n';
    require(atFirst.total > 0 && atFirst.downloaded > 0 && atFirst.downloaded < atFirst.total &&
        atFirst.state != CacheState::Complete, "first valid frame waited for download completion");
    anchorFfmpegVideoClock(first.ptsSeconds); startFfmpegPlayback(); setFfmpegAudioPaused(true);
    require(cache.snapshot().downloaded < cache.snapshot().total, "paused seek did not start with a partial cache");
    origin.delayMs = 2;
    DecodedVideoFrame preview;
    const double target = hls ? first.ptsSeconds + 3.013 : 3.013;
    const auto seekStarted = Clock::now();
    require(previewFfmpegAtPosition(target,&preview), "paused progressive seek preview failed");
    require(Clock::now()-seekStarted < std::chrono::seconds(5), "paused seek exceeded five seconds");
    validFrame(preview);
    require(preview.ptsSeconds >= target-0.001 && preview.ptsSeconds < target+0.1,
        "paused seek returned preroll instead of target frame: " + std::to_string(preview.ptsSeconds));
    const double paused = getFfmpegPlaybackClockSeconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(80)); advanceFfmpegPlaybackClock();
    require(std::abs(getFfmpegPlaybackClockSeconds()-paused) < 0.01, "paused seek resumed the playback clock");
    std::cout << (hls ? "HLS" : "FLV") << " paused seek target=" << target << " frame=" << preview.ptsSeconds <<
        " elapsed=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-seekStarted).count() << "ms\n";
    complete(cache);
    if (hls) rewrittenPlaylist(cache,origin,true);
    else require(get(cache.playbackUrl()).bytes == origin.files.at("/stream.flv"), "complete FLV cache differs from fixture");
    // Decode the cached file from the beginning to EOF at 4x. Count independently
    // of timestamps/clock so lost tail frames cannot look like completion.
    closeFfmpegVideo(); openPlayback(cache.playbackUrl());
    auto frame = nextFrame(); int count = 1; double last = frame.ptsSeconds;
    anchorFfmpegVideoClock(frame.ptsSeconds); setFfmpegPlaybackRate(4.0); startFfmpegPlayback();
    const auto deadline = Clock::now()+std::chrono::seconds(8);
    for (;;) {
        advanceFfmpegPlaybackClock();
        if (decodeNextFfmpegFrame(&frame)) { validFrame(frame); ++count; last = frame.ptsSeconds; }
        else if (isFfmpegPlaybackFinished()) break;
        require(Clock::now() < deadline, "4x playback did not reach EOF");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(count == 300 && getFfmpegPlaybackEndReason() == PlaybackEndReason::Completed,
        "cached playback lost frames or failed EOF; frames=" + std::to_string(count));
    std::cout << "PASS " << (hls ? "HLS" : "FLV") << " first-frame partial download, paused seek, exact cache bytes, 4x EOF frames=" << count << " lastPTS=" << last << '\n';
    cache.cancel(); requestFfmpegAbort(); closeFfmpegVideo(); cache.stop();
    origin.dump(root/"origin-ranges.json");
}

json manifest(const fs::path& root) {
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.path().filename() == "state.json") {
            // Workers replace the checkpoint with MoveFileExW. An ordinary
            // ifstream denies delete sharing on Windows and can make the cache
            // save fail merely because this observer is inspecting progress.
            HANDLE file = CreateFileW(entry.path().c_str(),GENERIC_READ,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            require(file != INVALID_HANDLE_VALUE, "open checkpoint with delete sharing");
            struct FileScope { HANDLE file; ~FileScope() { CloseHandle(file); } } scope{file};
            std::string contents; char bytes[4096]; DWORD count = 0;
            for (;;) {
                require(ReadFile(file,bytes,sizeof(bytes),&count,nullptr) != FALSE,"read checkpoint");
                if (!count) break;
                contents.append(bytes,count);
            }
            return json::parse(contents);
        }
    }
    throw std::runtime_error("cache did not persist state.json");
}
std::uint64_t durableBytes(const json& state) {
    std::uint64_t total = 0;
    for (const auto& resource : state.at("resources")) {
        std::uint64_t i = 0;
        for (const auto& offset : resource.at("blocks")) total += offset.get<std::uint64_t>() - (i++)*blockSize;
    }
    return total;
}
void partialFragmentResume(const fs::path& fixtures, const fs::path& root) {
    Origin origin(fixtures); ProgressiveCache cache;
    origin.reportPath = root/"origin-ranges.json";
    const auto source = origin.url("/hls/index.m3u8");
    require(cache.start(source,root.u8string()), "HLS partial start failed");
    until([&] { return cache.snapshot().downloaded >= 256*1024; },std::chrono::seconds(5),"no partial HLS download");
    cache.pause();
    require(cache.snapshot().state == CacheState::Paused, "partial HLS cache did not pause");
    cache.cancel(); cache.stop();
    const auto saved = manifest(root); const auto persisted = durableBytes(saved);
    require(persisted > 0 && persisted < origin.files.at("/hls/segment-00.ts").size(),
        "test did not cancel inside an incomplete first HLS fragment");
    bool partial = false;
    for (const auto& resource : saved.at("resources")) {
        std::uint64_t index = 0;
        for (const auto& offset : resource.at("blocks")) {
            const auto first = index++*blockSize;
            const auto last = std::min<std::uint64_t>(first+blockSize,resource.at("length").get<std::uint64_t>());
            partial |= offset.get<std::uint64_t>() > first && offset.get<std::uint64_t>() < last;
        }
    }
    require(partial, "checkpoint contains no partial-fragment offset");
    const auto marker = origin.mark(); origin.delayMs = 1;
    ProgressiveCache resumed;
    require(resumed.start(source,root.u8string()), "HLS partial restart failed");
    require(resumed.snapshot().downloaded >= persisted, "HLS restart discarded partial-fragment bytes");
    complete(resumed);
    const auto log = origin.log(); bool resumedPartial = false;
    for (std::size_t n = marker; n < log.size(); ++n) {
        const auto& request = log[n];
        if (request.method != "GET" || !request.ranged || request.last <= request.first) continue;
        for (const auto& resource : saved.at("resources")) {
            const auto url = resource.at("url").get<std::string>();
            if (url.substr(url.find('/',url.find("://")+3)) != request.path) continue;
            const auto block = request.first/blockSize;
            const auto durable = resource.at("blocks").at(static_cast<std::size_t>(block)).get<std::uint64_t>();
            require(request.first == durable, "HLS restart requested bytes before/after its persisted offset");
            resumedPartial |= durable > block*blockSize;
        }
    }
    require(resumedPartial, "origin never received a partial-fragment resume Range");
    rewrittenPlaylist(resumed,origin,true);
    resumed.stop(); origin.dump(root/"origin-ranges.json");
    std::ofstream(root/"partial-checkpoint.json") << saved.dump(2);
    std::cout << "PASS HLS partial fragment cancel/restart persisted=" << persisted << " bytes; Range begins at durable offsets\n";
}

void waitingReaderCancellation(const fs::path& fixtures, const fs::path& root) {
    Origin origin(fixtures); origin.holdLast = true;
    origin.reportPath = root/"origin-ranges.json";
    ProgressiveCache cache;
    require(cache.start(origin.url("/hls/index.m3u8"),root.u8string()), "wait cancellation start failed");
    cache.pause();
    const auto host = cache.playbackUrl().substr(0,cache.playbackUrl().find("/index.m3u8"));
    // A real HTTP reader seeks to an unavailable fragment. Cancellation must
    // release the cache condition variable even while its downloader is paused.
    auto reader = std::async(std::launch::async,[&] { return get(host+"/segment/7","0-65535"); });
    bool waiting = false;
    try {
        until([&] { return cache.snapshot().waitingForData; },std::chrono::seconds(3),"cache never advertised wait-for-buffer");
        waiting = true;
    } catch (...) {
        cache.cancel(); origin.holdLast = false; reader.wait(); cache.stop(); throw;
    }
    const auto begin = Clock::now(); cache.cancel(); origin.holdLast = false;
    const auto status = reader.wait_for(std::chrono::milliseconds(1000));
    if (status != std::future_status::ready) { cache.stop(); reader.wait(); }
    require(waiting && status == std::future_status::ready, "wait-for-buffer cancellation exceeded one second");
    const auto reply = reader.get();
    require(reply.bytes.empty(), "waiting reader received unwritten/sparse bytes after cancel");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-begin).count();
    require(cache.snapshot().state == CacheState::Cancelled, "cancelled state missing");
    cache.stop(); origin.dump(root/"origin-ranges.json");
    std::cout << "PASS HLS wait-for-buffer cancellation released HTTP reader in " << elapsed << "ms\n";
}

void writeJsonAtomic(const fs::path& path, const json& value) {
    const auto temp = fs::path(path.wstring()+L".tmp");
    { std::ofstream output(temp); output << value.dump(2); require(bool(output), "write child checkpoint report"); }
    require(MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH) != FALSE,
        "publish child checkpoint report");
}
int crashChild(const std::string& url, const fs::path& root, const fs::path& ready, const fs::path& report) {
    ProgressiveCache cache;
    require(cache.start(url,root.u8string()), "crash child cache start failed");
    until([&] {
        const auto state = cache.snapshot();
        const auto durable = durableBytes(manifest(root));
        require(state.state != CacheState::Error, "crash child download error: " + state.error);
        // At least four durable blocks and at least some fresh, uncheckpointed
        // bytes are required; killing an idle/completed child would prove little.
        return durable >= 4*blockSize && state.downloaded > durable+64*1024 && state.downloaded < state.total;
    },std::chrono::seconds(15),"child did not reach active checkpoint boundary");
    const auto state = cache.snapshot();
    writeJsonAtomic(report,{{"pid",GetCurrentProcessId()},{"downloaded",state.downloaded},
        {"total",state.total},{"ranges",state.ranges},{"checkpoint",manifest(root)}});
    writeJsonAtomic(ready,{{"pid",GetCurrentProcessId()},{"ready",true}});
    // The parent terminates this exact process through its CreateProcess handle;
    // no cache cancel/stop or C++ destructor is allowed on the successful path.
    for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
std::wstring quoteWindows(const std::wstring& text) {
    std::wstring out = L"\""; std::size_t slashes = 0;
    for (const auto c : text) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') { out.append(slashes*2+1,L'\\'); out += c; }
        else { out.append(slashes,L'\\'); out += c; }
        slashes = 0;
    }
    out.append(slashes*2,L'\\'); return out+L"\"";
}
struct ChildProcess {
    PROCESS_INFORMATION info{};
    ~ChildProcess() {
        if (info.hProcess) {
            if (WaitForSingleObject(info.hProcess,0) == WAIT_TIMEOUT) {
                TerminateProcess(info.hProcess,97); WaitForSingleObject(info.hProcess,2000);
            }
            CloseHandle(info.hProcess);
        }
        if (info.hThread) CloseHandle(info.hThread);
    }
};
void crashCheckpoint(const fs::path& fixtures, const fs::path& root, bool hls) {
    Origin origin(fixtures); origin.delayMs = 6;
    origin.reportPath = root/"origin-ranges.json";
    const auto cacheRoot = root/"cache"; fs::create_directories(cacheRoot);
    const auto ready = root/"ready.json", report = root/"child-checkpoint.json";
    const auto source = origin.url(hls ? "/hls/index.m3u8" : "/stream.flv");
    wchar_t executable[32768]{};
    require(GetModuleFileNameW(nullptr,executable,32768) > 0, "resolve exact crash-child executable");
    std::wstring command = quoteWindows(executable)+L" --crash-child "+quoteWindows(std::wstring(source.begin(),source.end()))+
        L" "+quoteWindows(cacheRoot.wstring())+L" "+quoteWindows(ready.wstring())+L" "+quoteWindows(report.wstring());
    ChildProcess child; STARTUPINFOW startup{sizeof(startup)};
    require(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child.info) != FALSE,
        "launch exact cache crash child");
    until([&] {
        if (WaitForSingleObject(child.info.hProcess,0) != WAIT_TIMEOUT) {
            std::string error = "crash child exited before readiness";
            if (fs::exists(report)) {
                std::ifstream input(report); json detail; input >> detail;
                error += ": " + detail.value("error",std::string("missing child error"));
            }
            throw std::runtime_error(error);
        }
        return fs::exists(ready);
    },std::chrono::seconds(20),"crash child readiness timed out");
    json childReady; { std::ifstream input(ready); input >> childReady; }
    require(childReady.at("pid").get<DWORD>() == child.info.dwProcessId, "ready marker belongs to another child");
    require(TerminateProcess(child.info.hProcess,99) != FALSE, "terminate exact crash child failed");
    require(WaitForSingleObject(child.info.hProcess,2000) == WAIT_OBJECT_0, "crash child did not exit promptly");
    DWORD exitCode = 0; GetExitCodeProcess(child.info.hProcess,&exitCode);
    require(exitCode == 99, "child did not have forced-termination exit code");
    const auto saved = manifest(cacheRoot); const auto persisted = durableBytes(saved);
    require(persisted >= 4*blockSize, "forced crash left no durable checkpoint");
    // Give closed child sockets a moment to make origin writes fail. This keeps
    // the before/after request log boundary unambiguous.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto marker = origin.mark(); origin.delayMs = 1;
    ProgressiveCache restarted;
    require(restarted.start(source,cacheRoot.u8string()), "restart after forced termination failed");
    require(restarted.snapshot().downloaded >= persisted, "forced crash restart forgot persisted data");
    complete(restarted);
    if (hls) rewrittenPlaylist(restarted,origin,true);
    else require(get(restarted.playbackUrl()).bytes == origin.files.at("/stream.flv"), "crash restart corrupted FLV bytes");
    restarted.stop();
    const auto requests = origin.log();
    json proof = json::array(); std::uint64_t maxOverlap = 0; bool continued = false;
    std::size_t replayedBlocks = 0;
    for (std::size_t n = marker; n < requests.size(); ++n) {
        const auto& after = requests[n];
        if (after.method != "GET" || !after.ranged || after.last <= after.first) continue; // playlist/body reads and metadata 0-0
        const json* matched = nullptr;
        for (const auto& resource : saved.at("resources")) {
            const auto url = resource.at("url").get<std::string>();
            if (url.substr(url.find('/',url.find("://")+3)) == after.path) matched = &resource;
        }
        require(matched != nullptr, "resumed origin request is not a checkpoint resource");
        const auto index = static_cast<std::size_t>(after.first/blockSize);
        const auto offset = matched->at("blocks").at(index).get<std::uint64_t>();
        require(after.first == offset, "crash restart did not begin exactly at durable Range offset");
        continued |= after.first > 0;
        std::uint64_t overlap = 0;
        for (std::size_t before = 0; before < marker; ++before) {
            const auto& previous = requests[before];
            if (previous.path != after.path || previous.method != "GET" || !previous.ranged || previous.last <= previous.first) continue;
            const auto first = std::max(previous.first,after.first);
            const auto last = std::min(previous.first+previous.sent,after.first+after.sent);
            if (last > first) overlap += last-first;
        }
        // Actual successful origin writes provide a conservative network-byte
        // bound (including bytes accepted by the socket but not cache-durable).
        require(overlap < 1024*1024, "crash replay exceeded one MiB for a resumed worker block");
        maxOverlap = std::max(maxOverlap,overlap);
        replayedBlocks += overlap > 0;
        proof.push_back({{"path",after.path},{"rangeBegin",after.first},{"rangeEnd",after.last},
            {"durableOffset",offset},{"replayedOriginBytes",overlap}});
    }
    require(continued && !proof.empty() && maxOverlap > 0, "forced crash did not exercise checkpoint replay");
    require(replayedBlocks <= 4, "crash restart replayed more than four active worker blocks");
    writeJsonAtomic(root/"crash-proof.json",{{"childPid",child.info.dwProcessId},{"forcedExit",exitCode},
        {"persistedBytes",persisted},{"maxWorkerOverlap",maxOverlap},{"replayedWorkerBlocks",replayedBlocks},
        {"resumedRanges",proof},{"checkpoint",saved}});
    origin.dump(root/"origin-ranges.json");
    std::cout << "PASS " << (hls ? "HLS" : "FLV") << " forced crash exact child PID=" << child.info.dwProcessId <<
        " persisted=" << persisted << " max worker replay=" << maxOverlap << " bytes (<1MiB), full bytes verified\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 6 && std::string(argv[1]) == "--crash-child") {
            try {
                return crashChild(argv[2],fs::u8path(argv[3]),fs::u8path(argv[4]),fs::u8path(argv[5]));
            } catch (const std::exception& error) {
                writeJsonAtomic(fs::u8path(argv[5]),{{"pid",GetCurrentProcessId()},{"error",error.what()}});
                throw;
            }
        }
        require(argc >= 3, "usage: cache-playback-test fixtures output-root [--fixtures-only]");
        require(initializeFfmpeg(), "FFmpeg initialization failed");
        const auto fixtures = fs::absolute(fs::u8path(argv[1]));
        validateFixtures(fixtures);
        const std::string mode = argc > 3 ? argv[3] : "--all";
        require(mode == "--all" || mode == "--fixtures-only" || mode == "--flv" || mode == "--hls" ||
            mode == "--hls-resume" || mode == "--wait-cancel" || mode == "--crash-flv" || mode == "--crash-hls", "unknown test scenario");
        if (mode != "--fixtures-only") {
            const auto root = fs::absolute(fs::u8path(argv[2])) /
                (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
            fs::create_directories(root);
            std::cout << "Run artifacts: " << root.u8string() << '\n';
            if (mode == "--all" || mode == "--flv") playbackScenario(fixtures,root/"flv",false);
            if (mode == "--all" || mode == "--hls") playbackScenario(fixtures,root/"hls",true);
            if (mode == "--all" || mode == "--hls-resume") partialFragmentResume(fixtures,root/"hls-resume");
            if (mode == "--all" || mode == "--wait-cancel") waitingReaderCancellation(fixtures,root/"wait-cancel");
            if (mode == "--all" || mode == "--crash-flv") crashCheckpoint(fixtures,root/"crash-flv",false);
            if (mode == "--all" || mode == "--crash-hls") crashCheckpoint(fixtures,root/"crash-hls",true);
        }
        shutdownFfmpeg();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        shutdownFfmpeg(); return 1;
    }
}
