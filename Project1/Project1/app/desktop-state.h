#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct DownloadRecord {
    std::string source;
    std::uint64_t total = 0, downloaded = 0;
    std::string state;
    bool requiresSource = false;
    // SHA256 of original source UTF-8 bytes, 64 lowercase hex characters.
    // Empty only for legacy redacted history whose original source is unknown.
    std::string taskKey;
};

struct DesktopState {
    std::vector<std::string> recent;
    int decoder = 0;
    float brightness = 1, volume = 1;
    int rotation = 0;
    float zoom = 1;
    int filter = 0, networkMode = 1;
    bool showInfo = false;
    std::string host = "127.0.0.1";
    int port = 18760;
    std::vector<DownloadRecord> downloads;
};

// Synchronous helpers: the owner serializes calls for a data root. UTF-8 paths
// and strings throughout. Missing desktop-state.json returns defaults without
// error; malformed/oversized state returns all defaults with a visible error.
// Limits: 10 recent, 100 downloads, 2047 bytes/source, 127 bytes/host,
// 64 bytes/download state, 1 MiB/JSON. Unknown JSON fields are never re-saved.
// Recent network addresses with userinfo/query/fragment are omitted rather
// than stored as a playable altered source. Download sources omit these parts
// and persist requiresSource=true whenever altered. taskKey retains the original
// source hash for cleanup even when credentials are unavailable for resume;
// the caller must request a fresh source to resume requiresSource history.
DesktopState loadDesktopState(const std::string& root, std::string& error);
// Invalid state/write failures leave the old record intact. A successful save
// clears error and atomically replaces desktop-state.json using UTF-8 JSON.
bool saveDesktopState(const std::string& root, const DesktopState& state, std::string& error);
// Best-effort, never throws. desktop.log stays <= 1 MiB, including large events.
// Redacts URL query/fragment/userinfo and named auth/password/token secrets.
void appendDesktopLog(const std::string& root, const std::string& event, const std::string& details);
// Reporting overload for UI diagnostics; false returns a sanitized I/O reason.
bool appendDesktopLog(const std::string& root, const std::string& event,
    const std::string& details, std::string& error);
// root is the DESKTOP data root, NOT root/cache. Computes the same lowercase
// SHA256(original source UTF-8 bytes) as ProgressiveCache under root/cache.
// Stop this application's cache/playback first; an exclusive task.lock rejects
// tasks held by any instance (paused and complete tasks may still hold it).
// Rejects reparse points in ancestors/descendants and pins inspected paths
// during deletion. Missing tasks succeed; errors can leave a partial cleanup.
bool removeDesktopCache(const std::string& root, const std::string& source, std::string& error);
// Same lock/containment checks as source cleanup, using a persisted task key.
// Allows cleanup of requiresSource history without saving credentials. Rejects
// anything except exactly 64 lowercase hex characters; never accepts a path.
bool removeDesktopCacheByKey(const std::string& root, const std::string& key, std::string& error);
