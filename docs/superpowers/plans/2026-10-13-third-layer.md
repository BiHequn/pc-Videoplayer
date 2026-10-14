# Third Layer Delivery Implementation Plan

> Agentic workers: use superpowers:subagent-driven-development and dispatching-parallel-agents for disjoint state, benchmark and packaging tasks. Parent owns UI, build integration, soak, fault tests and final verification.

Goal: deliver the approved A/A graduation project with actual stability/performance evidence and a portable demo.
Architecture: existing media/service/cache stay authoritative; add validated persistent desktop state, bounded diagnostics, external measurement harness and reproducible packaging.
Tech: Windows x64, C++17, Win32, Dear ImGui, FFmpeg6.1.1, OpenGL3.3, PowerShell, JSON/CSV/SVG/Markdown.
Spec: docs/superpowers/specs/2026-10-13-third-layer-design.md.

## Constraints

- User approved scope A and document A; exclude new hardware decode/launcher integration, thesis and PPT.
- New branch in existing checkout reuses static dependencies; no remote updates without user request.
- Stable media source during 60-minute soak; record actual duration, samples and executable identity.
- Resource/process/file operations limited to owned task paths/handles.
- No invented performance, disk-full or clean-machine results.

## Task 1: Start stability measurement (parent)

Files: tests/player-soak-test.cpp; CMakeLists.txt.
Consumes PlaybackSession::open/pause/seek/stop/snapshot/frame. Produces JSON samples and explicit passed/failure reason.
- [x] Implement harness checks: missing fixture fails, 100 pause/seek/resume/stop cycles, real frames and recovery; read GetProcessTimes, GetProcessMemoryInfo, GetProcessHandleCount and Toolhelp threads.
- [x] Build target; run short command against generated audio fixture, confirm samples and >100 operations.
- [x] Start `player-soak-test <fixture> <output> --seconds 3600 --cycles 100` as owned process while other work proceeds; record binary hash and logs.
- [x] Wait for true completion and inspect resource trajectory and final elapsed time; 3600.100s, 100 cycles/500 operations, independent CSV verification passed. Numeric process exit code unavailable and explicitly recorded.

## Task 2: Desktop state and diagnostics (state worker + parent UI)

Files: app/desktop-state.{h,cpp}, tests/desktop-state-test.cpp (worker); app/desktop-player.cpp and CMake/VS (parent).
Produces DesktopState containing recent/settings/download history, loadDesktopState(root), saveDesktopState(root,state,error), appendDesktopLog(root,event,details), safe cache removal helper.
- [x] Real-file tests reject corrupt/type-invalid fields, roundtrip settings/recent/downloads, atomically replace and keep credentials out.
- [x] Implement validated persistence, bounded logs and canonical task-directory removal.
- [x] Parent loads after command-line data-root resolution, persists on meaningful changes, exposes history/open-folder/cleanup and state/log errors.
- [x] Test actual desktop restart and persistence; original GUI smoke remains passing.

## Task 3: Benchmark (measurement worker)

Files: tests/make-benchmark-media.cpp, tests/player-benchmark.cpp, scripts/run-performance.ps1; docs/performance/method.md.
- [x] Generate deterministic 1280x720,1920x1080,3840x2160 fixtures with FFmpeg builtin encoder; validate dimensions.
- [x] Measure real PlaybackSession + OpenGL in YUV/BGRA modes, repeats, CPU seconds/core normalization, peak memory, first frame, render/frame counts and sync.
- [x] Record JSON/CSV and share exact CMake target/link contract; parent integrates targets.
- [x] Run measurements with no parallel build load; create plots from actual data, report improvement or no improvement accurately.

## Task 4: Portable package and project docs (packaging worker)

Files: scripts/package-release.ps1, scripts/verify-release.ps1, docs/delivery/{user-guide,architecture,demo,acceptance}.md.
- [x] Stage binaries, fixture files, allowed docs and actual licenses to build/release/versioned directory; emit manifest hashes and ZIP. Formal 3.0.0 includes 65 files, complete performance and actual stability evidence.
- [x] Double-click demo script resolves paths against package directory, uses portable data root, no dev tools required.
- [x] Verify staged package outside source cwd with reduced PATH, actual smoke/window/second-layer demo; formal 3.0.0 8/8 passed, four player exits 0, detailed report and clean-machine limit retained.
- [x] Documentation explains actual states/threads/queues/protocol/cache, user actions and fixed 5–8 minute demo. Parent adds final results/links.

## Task 5: Fault verification and final delivery (parent)

Files: tests/delivery-fault-test.cpp, docs/validation/2026-10-13-third-layer.md, README.md, CMake/VS.
- [x] Real file/socket tests cover blocked cache path, corrupt media then reopen, interruption/resume and resource cleanup; reuse existing auth/upload/validator regressions.
- [x] Build Release/Debug and VSRelease, run full CTest; run package after final binaries generated.
- [x] Review scoped worker changes, fix concrete findings and verify only affected paths before final suite.
- [x] Inspect 60min samples, write honest performance/stability matrix and limitations; no fabricated environment validation.
- [x] Prepare final package/docs/data links; final delivery complete. User subsequently requested commit/push on codex/third-layer-delivery; precommit validation and publication follow that authorization.
