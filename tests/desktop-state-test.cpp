#include "app/desktop-state.h"
#include <Windows.h>
#include <winioctl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <thread>

namespace fs = std::filesystem;
using Json = nlohmann::json;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary); out << bytes;
    require(bool(out), "fixture write failed");
}
std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    require(bool(in), "fixture read failed");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
bool defaults(const DesktopState& s) {
    return s.recent.empty() && s.downloads.empty() && s.decoder == 0 &&
        s.brightness == 1 && s.volume == 1 && s.rotation == 0 && s.zoom == 1 &&
        s.filter == 0 && s.networkMode == 1 && !s.showInfo &&
        s.host == "127.0.0.1" && s.port == 18760;
}
// Literal independently generated SHA256("http://example.test/video.flv").
const char* source = "http://example.test/video.flv";
const char* digest = "89122422cc85d3d16e15a1d1cedf8962c02c9b694a8c1b556a85dab855a29395";
// Independently calculated hash of the original signed URL used below.
const char* signedSource = "https://alice:USERINFOSECRET@example.test/video.flv?token=QUERYSECRET&quality=hd#FRAGMENTSECRET";
const char* signedDigest = "1d0ca67524eeac5929e07dca9b0aed12132aa183e87e7cfccb199546539b645d";
fs::path task(const fs::path& root) { return root / "cache" / digest; }
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
}
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: desktop-state-test <owned-output-root>\n"; return 2; }
    const auto base = fs::absolute(fs::u8path(argv[1])) /
        (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    fs::create_directories(base);
    int failed = 0, passed = 0;
    auto test = [&](const char* name, const std::function<void(const fs::path&)>& fn) {
        try { const auto root = base / name; fs::create_directories(root); fn(root);
            ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("missing-and-unicode-roundtrip", [](const fs::path& folder) {
        const auto root = folder / fs::u8path(u8"设置-目录"); std::string error = "old";
        require(defaults(loadDesktopState(root.u8string(), error)) && error.empty(), "missing state must be defaults without error");
        DesktopState s; s.recent = {u8"C:/视频/示例.mp4", "http://example.test/a.flv"};
        s.decoder = 2; s.brightness = 1.25f; s.volume = .375f; s.rotation = 270;
        s.zoom = 2.5f; s.filter = 2; s.networkMode = 2; s.showInfo = true;
        s.host = "example.test"; s.port = 54321;
        s.downloads = {{source, std::numeric_limits<std::uint64_t>::max(),
            4294967297ULL, u8"已暂停"}};
        require(saveDesktopState(root.u8string(), s, error) && error.empty(), "save valid state failed");
        auto loaded = loadDesktopState(root.u8string(), error);
        require(error.empty() && loaded.recent == s.recent && loaded.decoder == 2 &&
            loaded.brightness == 1.25f && loaded.volume == .375f && loaded.rotation == 270 &&
            loaded.zoom == 2.5f && loaded.filter == 2 && loaded.networkMode == 2 && loaded.showInfo &&
            loaded.host == s.host && loaded.port == 54321 && loaded.downloads.size() == 1 &&
            loaded.downloads[0].total == s.downloads[0].total &&
            loaded.downloads[0].downloaded == 4294967297ULL && loaded.downloads[0].state == u8"已暂停",
            "roundtrip lost settings, UTF-8 or uint64 counters");
        require(read(root / "desktop-state.json").find(u8"视频") != std::string::npos, "JSON must be UTF-8");
    });
    test("corrupt-and-invalid-defaults", [](const fs::path& root) {
        const std::vector<std::string> invalid{
            "{broken", "[]", "null", R"({"port":"18760"})", R"({"port":1.5})",
            R"({"port":0})", R"({"port":65536})", R"({"port":18446744073709551615})",
            R"({"decoder":3})", R"({"brightness":0.2})", R"({"brightness":2.1})",
            R"({"volume":-1})", R"({"zoom":3.1})", R"({"rotation":45})",
            R"({"filter":-1})", R"({"networkMode":3})", R"({"showInfo":1})",
            R"({"host":false})", R"({"host":"user:secret@example.test"})",
            R"({"recent":[3]})", R"({"recent":[""]})", R"({"recent":["bad\u0000path"]})",
            R"({"downloads":[{"source":"a","total":-1,"downloaded":0,"state":"idle"}]})",
            R"({"downloads":[{"source":"a","total":1,"downloaded":2,"state":"idle"}]})",
            R"({"downloads":[{"source":"a","total":1.5,"downloaded":0,"state":"idle"}]})",
            R"({"downloads":[{"source":"a"}]})",
            Json({{"recent", std::vector<std::string>(11, "a")}}).dump(),
            Json({{"host", std::string(128, 'x')}}).dump(),
            Json({{"recent", {std::string(2048, 'x')}}}).dump(),
            Json({{"downloads", std::vector<Json>(101, {{"source", "a"}, {"total", 0}, {"downloaded", 0}, {"state", "idle"}})}}).dump(),
            std::string(1024 * 1024 + 1, ' ')};
        std::size_t caseIndex = 0;
        for (const auto& bytes : invalid) {
            write(root / "desktop-state.json", bytes); std::string error;
            if (!defaults(loadDesktopState(root.u8string(), error)) || error.empty())
                throw std::runtime_error("invalid record must return all defaults and diagnostic; case " + std::to_string(caseIndex));
            require(error.find(bytes.substr(0, 100)) == std::string::npos || bytes.size() < 4, "diagnostic must not echo secret file contents");
            ++caseIndex;
        }
        write(root / "desktop-state.json", R"({"volume":0.5})"); std::string error;
        require(loadDesktopState(root.u8string(), error).volume == .5f && error.empty(), "missing optional fields must default");
    });
    test("credentials-never-persist", [](const fs::path& root) {
        DesktopState s;
        s.recent = {"https://alice:USERINFOSECRET@example.test/video.flv?token=QUERYSECRET&quality=hd#FRAGMENTSECRET", "https://example.test/safe.flv"};
        s.downloads = {{s.recent[0], 10, 4, "Authorization: Bearer STATESECRET"}};
        std::string error; require(saveDesktopState(root.u8string(), s, error), "redacted state save failed");
        const auto bytes = read(root / "desktop-state.json");
        for (const auto* secret : {"USERINFOSECRET", "QUERYSECRET", "FRAGMENTSECRET", "STATESECRET"})
            require(bytes.find(secret) == std::string::npos, "credential leaked into state");
        const auto loaded = loadDesktopState(root.u8string(), error);
        require(loaded.recent == std::vector<std::string>{"https://example.test/safe.flv"}, "redacted recent URL must be omitted to prevent wrong-source playback");
        require(loaded.downloads[0].requiresSource, "redacted download must require fresh source");
        require(saveDesktopState(root.u8string(), loaded, error) && loadDesktopState(root.u8string(), error).downloads[0].requiresSource,
            "resave cleared requiresSource flag");
        write(root / "desktop-state.json", R"({"password":"PASSWORDSECRET","token":"TOKENSECRET","recent":["https://u:LOADSECRET@example.test/a?token=QUERYSECRET"]})");
        s = loadDesktopState(root.u8string(), error);
        require(error.empty() && s.recent.empty() && saveDesktopState(root.u8string(), s, error), "unknown credentials must be ignored and legacy recent redaction omitted");
        require(read(root / "desktop-state.json").find("SECRET") == std::string::npos, "load-save retained credentials");
        write(root / "desktop-state.json", R"({"downloads":[{"source":"https://example.test/a?token=QUERYSECRET","total":10,"downloaded":1,"state":"paused","requiresSource":false}]})");
        require(loadDesktopState(root.u8string(), error).downloads[0].requiresSource, "legacy/untrusted record failed to derive source requirement");
    });
    test("invalid-save-keeps-old-and-blocked-root", [](const fs::path& root) {
        std::string error; DesktopState s; s.volume = .5f;
        require(saveDesktopState(root.u8string(), s, error), "initial save failed");
        const auto old = read(root / "desktop-state.json");
        for (const auto value : {-1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            s.volume = value; require(!saveDesktopState(root.u8string(), s, error) && !error.empty(), "invalid float save accepted");
            require(read(root / "desktop-state.json") == old, "invalid save replaced valid record");
        }
        s = {}; s.recent = {std::string("bad\0path", 8)};
        require(!saveDesktopState(root.u8string(), s, error), "NUL source accepted");
        s = {}; s.recent = {std::string("\xff", 1)};
        require(!saveDesktopState(root.u8string(), s, error), "invalid UTF-8 source accepted");
        write(root / "blocked", "file");
        require(!saveDesktopState((root / "blocked" / "child").u8string(), {}, error) && !error.empty(), "blocked root must return error");
        require(defaults(loadDesktopState((root / "blocked" / "child").u8string(), error)) && !error.empty(), "blocked load must return error");
        appendDesktopLog((root / "blocked" / "child").u8string(), "error", "no crash");
        require(!appendDesktopLog((root / "blocked" / "child").u8string(), "error", "no crash", error) && !error.empty(), "log failure not visible in reporting overload");
    });
    test("atomic-sharing-and-readers", [](const fs::path& root) {
        std::string error; DesktopState s; s.port = 20001;
        require(saveDesktopState(root.u8string(), s, error), "initial atomic save failed");
        Handle blocked; blocked.value = CreateFileW((root / "desktop-state.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        require(blocked.value != INVALID_HANDLE_VALUE, "blocking reader fixture failed");
        s.port = 20002; require(!saveDesktopState(root.u8string(), s, error) && !error.empty(), "incompatible sharing must report failure");
        require(Json::parse(read(root / "desktop-state.json"))["port"] == 20001, "sharing failure damaged old state");
        CloseHandle(blocked.value); blocked.value = INVALID_HANDLE_VALUE;
        std::atomic<bool> stop{false}, bad{false}, observed{false};
        std::string readerError;
        std::thread reader([&] { while (!stop) {
            Handle f; f.value = CreateFileW((root / "desktop-state.json").c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f.value == INVALID_HANDLE_VALUE) { readerError = "open " + std::to_string(GetLastError()); bad = true; break; }
            char data[4096]; DWORD got = 0;
            if (!ReadFile(f.value, data, sizeof(data), &got, nullptr)) { readerError = "read " + std::to_string(GetLastError()); bad = true; break; }
            try { const int port = Json::parse(data, data + got).at("port").get<int>();
                if (port != 20001 && port != 20002) bad = true; observed = true; }
            catch (...) { readerError = "parse"; bad = true; }
        }});
        // Ensure at least one real read, independently of scheduler timing.
        const auto deadline = GetTickCount64() + 3000;
        while (!observed && !bad && GetTickCount64() < deadline) Sleep(1);
        bool saved = true;
        std::string firstSaveError;
        for (int i = 0; i < 40; ++i) { s.port = i % 2 ? 20001 : 20002;
            if (!saveDesktopState(root.u8string(), s, error)) {
                saved = false; if (firstSaveError.empty()) firstSaveError = "iteration " + std::to_string(i) + ": " + error;
            }
        }
        stop = true; reader.join();
        if (!saved || !observed || bad) throw std::runtime_error("atomic read/save failed: save=" +
            std::to_string(saved) + " observed=" + std::to_string(bool(observed)) + " reader=" + readerError + " saveError=" + firstSaveError);
        for (const auto& entry : fs::directory_iterator(root)) require(entry.path().extension() != ".tmp", "atomic temporary file leaked");
    });
    test("logs-redacted-and-bounded", [](const fs::path& root) {
        appendDesktopLog(root.u8string(), "https://u:EVENTSECRET@example.test/a?token=EVENTQUERYSECRET",
            "request https://u:USERSECRET@example.test/v?x=QUERYSECRET#FRAGMENTSECRET Authorization: Bearer BEARERSECRET; password=PASSWORDSECRET token=TOKENSECRET api_key=APISECRET\nnext");
        appendDesktopLog(root.u8string(), "login", R"({"password": "JSONSECRET", "access_token":"ACCESSSECRET", "Authorization":"Basic BASICSECRET"})");
        auto bytes = read(root / "desktop.log");
        require(bytes.find("SECRET") == std::string::npos && bytes.find("example.test/v") != std::string::npos, "log credential redaction failed");
        require(bytes.find("\nnext") == std::string::npos, "log newline injection accepted");
        for (int i = 0; i < 350; ++i) appendDesktopLog(root.u8string(), "load", std::string(8192, 'x'));
        appendDesktopLog(root.u8string(), "final-event", std::string(2 * 1024 * 1024, 'z'));
        require(fs::file_size(root / "desktop.log") <= 1024 * 1024 && read(root / "desktop.log").find("final-event") != std::string::npos, "log cap or newest event violated");
    });
    test("logs-sharing-failure-and-clipped-userinfo", [](const fs::path& root) {
        std::string error;
        require(appendDesktopLog(root.u8string(), "init", "ready", error), "reporting log write failed");
        Handle blocked; blocked.value = CreateFileW((root / "desktop.log").c_str(), GENERIC_READ, 0,
            nullptr, OPEN_EXISTING, 0, nullptr);
        require(blocked.value != INVALID_HANDLE_VALUE, "log sharing fixture failed");
        require(!appendDesktopLog(root.u8string(), "event", "data", error) && !error.empty(), "log sharing failure must be visible");
        CloseHandle(blocked.value); blocked.value = INVALID_HANDLE_VALUE;
        appendDesktopLog(root.u8string(), "request", "https://USERSECRET:" + std::string(20000, 'x') + "@example.test/a");
        require(read(root / "desktop.log").find("USERSECRET") == std::string::npos, "input clipping leaked a long URL userinfo prefix");
    });
    test("delete-exact-task-and-active-lock", [](const fs::path& root) {
        const auto dir = task(root); write(dir / "nested" / "chunk.flv", "chunk");
        write(root / "cache" / "other-task" / "keep", "keep");
        Handle lock; lock.value = CreateFileW((dir / "task.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(lock.value != INVALID_HANDLE_VALUE, "active lock fixture failed");
        std::string error;
        require(!removeDesktopCache(root.u8string(), source, error) && !error.empty(), "active task deletion accepted");
        require(read(dir / "nested" / "chunk.flv") == "chunk", "active task touched");
        CloseHandle(lock.value); lock.value = INVALID_HANDLE_VALUE;
        if (!removeDesktopCache(root.u8string(), source, error) || !error.empty() || fs::exists(dir))
            throw std::runtime_error("inactive task not deleted: " + error);
        require(read(root / "cache" / "other-task" / "keep") == "keep", "sibling cache deleted");
        require(removeDesktopCache(root.u8string(), source, error) && error.empty(), "missing task must be idempotent");
        require(!removeDesktopCache(root.u8string(), "", error), "empty source cleanup accepted");
        require(!removeDesktopCache("", source, error), "empty root cleanup accepted");
    });
    test("delete-blocked-and-lock-reparse", [](const fs::path& root) {
        const auto dir = task(root); write(dir / "busy.flv", "chunk");
        Handle busy; busy.value = CreateFileW((dir / "busy.flv").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        require(busy.value != INVALID_HANDLE_VALUE, "busy fixture failed");
        std::string error; require(!removeDesktopCache(root.u8string(), source, error) && !error.empty(), "non-shareable media deletion accepted");
        require(fs::exists(dir / "busy.flv"), "busy file deleted");
        CloseHandle(busy.value); busy.value = INVALID_HANDLE_VALUE;
        fs::remove(dir / "task.lock");
        fs::create_directory(dir / "task.lock");
        require(!removeDesktopCache(root.u8string(), source, error), "directory as task.lock accepted");
        require(fs::exists(dir / "busy.flv"), "invalid lock allowed partial deletion");
    });
    test("signed-history-cleanup-by-key", [](const fs::path& root) {
        DesktopState s; s.downloads = {{signedSource, 100, 40, "paused"}};
        std::string error;
        require(saveDesktopState(root.u8string(), s, error), "signed history save failed");
        const auto bytes = read(root / "desktop-state.json");
        require(bytes.find("SECRET") == std::string::npos, "task key persistence leaked original credentials");
        auto loaded = loadDesktopState(root.u8string(), error);
        require(error.empty() && loaded.downloads.size() == 1 && loaded.downloads[0].requiresSource &&
            loaded.downloads[0].source == "https://example.test/video.flv" &&
            loaded.downloads[0].taskKey == signedDigest, "redacted history must retain the original task hash");
        require(saveDesktopState(root.u8string(), loaded, error), "redacted history resave failed");
        loaded = loadDesktopState(root.u8string(), error);
        require(error.empty() && loaded.downloads[0].taskKey == signedDigest, "redacted resave derived wrong task hash");
        const auto dir = root / "cache" / signedDigest;
        write(dir / "chunk.flv", "signed-task");
        write(task(root) / "keep", "other-task");
        Handle lock; lock.value = CreateFileW((dir / "task.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(lock.value != INVALID_HANDLE_VALUE, "signed task lock fixture failed");
        require(!removeDesktopCacheByKey(root.u8string(), loaded.downloads[0].taskKey, error) && !error.empty(),
            "by-key cleanup ignored active task lock");
        require(read(dir / "chunk.flv") == "signed-task", "active signed task touched");
        CloseHandle(lock.value); lock.value = INVALID_HANDLE_VALUE;
        require(removeDesktopCacheByKey(root.u8string(), loaded.downloads[0].taskKey, error) &&
            error.empty() && !fs::exists(dir), "redacted signed task cleanup failed");
        require(read(task(root) / "keep") == "other-task", "by-key cleanup touched another task");
        write(dir / "chunk.flv", "signed-task");
        require(removeDesktopCache(root.u8string(), signedSource, error) && !fs::exists(dir),
            "source cleanup and by-key cleanup disagree on original task hash");
        require(removeDesktopCacheByKey(root.u8string(), signedDigest, error) && error.empty(), "missing keyed task not idempotent");
    });
    test("task-key-validation-and-legacy-history", [](const fs::path& root) {
        const auto dir = task(root); write(dir / "keep", "sentinel");
        const std::vector<std::string> invalidKeys{"", std::string(63, 'a'), std::string(65, 'a'),
            std::string(64, 'A'), std::string(63, 'a') + "g", "../" + std::string(61, 'a'),
            std::string(63, 'a') + "/", std::string(63, 'a') + "\\", std::string(63, 'a') + std::string(1, '\0')};
        for (const auto& key : invalidKeys) {
            std::string error;
            require(!removeDesktopCacheByKey(root.u8string(), key, error) && !error.empty(), "forged task key cleanup accepted");
            require(read(dir / "keep") == "sentinel", "invalid keyed cleanup touched task");
            write(root / "desktop-state.json", Json({{"downloads", {{{"source", source}, {"total", 1},
                {"downloaded", 0}, {"state", "paused"}, {"taskKey", key}}}}}).dump());
            require(defaults(loadDesktopState(root.u8string(), error)) && !error.empty(), "invalid persisted task key accepted");
        }
        std::string error;
        Json legacy{{"downloads", Json::array({Json{{"source", source}, {"total", 1},
            {"downloaded", 0}, {"state", "paused"}}})}};
        write(root / "desktop-state.json", legacy.dump());
        auto loaded = loadDesktopState(root.u8string(), error);
        require(error.empty() && loaded.downloads[0].taskKey == digest, "legacy original source did not derive task key");
        legacy["downloads"][0]["source"] = signedSource;
        write(root / "desktop-state.json", legacy.dump());
        loaded = loadDesktopState(root.u8string(), error);
        require(error.empty() && loaded.downloads[0].taskKey == signedDigest && loaded.downloads[0].requiresSource,
            "legacy original signed source did not derive key before redaction");
        legacy["downloads"][0]["source"] = "https://example.test/video.flv";
        legacy["downloads"][0]["requiresSource"] = true;
        write(root / "desktop-state.json", legacy.dump());
        loaded = loadDesktopState(root.u8string(), error);
        require(error.empty() && loaded.downloads[0].taskKey.empty(), "legacy redacted history invented a task key");
        require(saveDesktopState(root.u8string(), loaded, error) &&
            loadDesktopState(root.u8string(), error).downloads[0].taskKey.empty(), "legacy redacted resave invented a task key");
        legacy["downloads"][0]["source"] = source;
        legacy["downloads"][0]["requiresSource"] = false;
        legacy["downloads"][0]["taskKey"] = signedDigest;
        write(root / "desktop-state.json", legacy.dump());
        require(defaults(loadDesktopState(root.u8string(), error)) && !error.empty(), "known source with mismatched task key accepted");
        DesktopState bad; bad.downloads = {{source, 1, 0, "paused", false, "../bad"}};
        const auto old = read(root / "desktop-state.json");
        require(!saveDesktopState(root.u8string(), bad, error) && !error.empty() && read(root / "desktop-state.json") == old,
            "invalid task key save replaced previous record");
        write(root / "desktop-state.json", Json({{"downloads", {{{"source", source}, {"total", 1},
            {"downloaded", 0}, {"state", "paused"}, {"taskKey", 123}}}}}).dump());
        require(defaults(loadDesktopState(root.u8string(), error)) && !error.empty(), "numeric task key accepted");
    });
    test("delete-long-root-short-isolation", [](const fs::path& folder) {
        auto root = folder;
        // Original task.lock fits classic MAX_PATH (259 chars); the former
        // .desktop-clean-<hash>-suffix path exceeded it. Do not use an extended
        // path prefix: that would hide the path growth regression.
        constexpr std::size_t wantedRoot = 174;
        require(root.wstring().size() < wantedRoot - 2, "long-root fixture output prefix is too long");
        while (root.wstring().size() < wantedRoot) {
            const auto left = wantedRoot - root.wstring().size();
            if (left < 2) break;
            root /= std::string(std::min<std::size_t>(left - 1, 24), 'x');
        }
        const auto dir = task(root);
        require((dir / "task.lock").wstring().size() < 260, "original task fixture exceeds MAX_PATH");
        write(dir / "chunk.flv", "chunk");
        std::string error;
        if (!removeDesktopCacheByKey(root.u8string(), digest, error))
            throw std::runtime_error("long-root keyed cleanup failed: " + error);
        require(!fs::exists(dir), "long-root task retained");
        require(fs::is_empty(root / "cache"), "isolated long-root directory leaked");
    });
    test("delete-junction-escapes", [](const fs::path& root) {
        const auto outside = root / "outside"; write(outside / "keep", "outside-sentinel");
        fs::create_directories(task(root));
        // Mount-point reparse fixtures need no symbolic-link privilege.
        auto junction = [&](const fs::path& link) {
            fs::create_directory(link);
            Handle h; h.value = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            require(h.value != INVALID_HANDLE_VALUE, "junction handle failed");
            const std::wstring target = L"\\??\\" + outside.wstring();
            std::vector<unsigned char> buffer(16 + (target.size() + 2) * sizeof(wchar_t));
            auto put16 = [&](int at, WORD value) { std::memcpy(buffer.data() + at, &value, 2); };
            DWORD tag = IO_REPARSE_TAG_MOUNT_POINT; std::memcpy(buffer.data(), &tag, 4);
            put16(4, WORD(buffer.size() - 8)); put16(8, 0); put16(10, WORD(target.size() * 2));
            put16(12, WORD((target.size() + 1) * 2)); put16(14, 0);
            std::memcpy(buffer.data() + 16, target.data(), target.size() * sizeof(wchar_t));
            DWORD returned = 0;
            require(DeviceIoControl(h.value, FSCTL_SET_REPARSE_POINT, buffer.data(), DWORD(buffer.size()), nullptr, 0, &returned, nullptr), "junction fixture failed");
        };
        junction(task(root) / "escape"); std::string error;
        require(!removeDesktopCacheByKey(root.u8string(), digest, error) && !error.empty(), "by-key nested junction escape accepted");
        require(!removeDesktopCache(root.u8string(), source, error) && !error.empty(), "nested junction escape accepted");
        require(read(outside / "keep") == "outside-sentinel", "escape target touched");
        require(RemoveDirectoryW((task(root) / "escape").c_str()), "fixture junction unlink failed");
        if (!removeDesktopCache(root.u8string(), source, error)) throw std::runtime_error("safe directory after junction removal failed: " + error);
        junction(task(root));
        require(!removeDesktopCache(root.u8string(), source, error), "task junction accepted");
        require(RemoveDirectoryW(task(root).c_str()), "task junction unlink failed");
        require(RemoveDirectoryW((root / "cache").c_str()), "cache fixture remove failed");
        junction(root / "cache");
        require(!removeDesktopCache(root.u8string(), source, error), "cache ancestor junction accepted");
        require(read(outside / "keep") == "outside-sentinel", "ancestor escape target touched");
        require(RemoveDirectoryW((root / "cache").c_str()), "ancestor junction unlink failed");
        junction(root / "alias");
        require(!removeDesktopCache((root / "alias" / "data").u8string(), source, error), "data root ancestor junction accepted");
        require(RemoveDirectoryW((root / "alias").c_str()), "root junction unlink failed");
    });
    std::cout << passed << " passed, " << failed << " failed; artifacts " << base.u8string() << '\n';
    return failed ? 1 : 0;
}
