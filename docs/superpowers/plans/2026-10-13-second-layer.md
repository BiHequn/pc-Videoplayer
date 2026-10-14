# Second Layer Implementation Plan

> Agentic workers: use superpowers:subagent-driven-development for disjoint service and rendering tasks; controller owns streaming, media integration, UI, build and whole-system verification.

Goal: complete approved second layer and progressive FLV/HLS cache playback with a repeatable local demo.
Architecture: background service client + loopback TCP server, immutable planar video frames and OpenGL renderer, progressive cache exposed over loopback HTTP to existing FFmpeg demux.
Tech: Windows x64, C++17, Win32, Dear ImGui v1.91.9b, FFmpeg6.1.1, WGL, WinHTTP, Winsock, BCrypt, nlohmann JSON.
Spec: docs/superpowers/specs/2026-10-13-second-layer-design.md

## Constraints

- Preserve first-layer dev commit; development branch codex/second-layer-demo in existing checkout avoids duplicating static dependency installation.
- Disjoint worker write scopes; parent edits CMake/VS files, session, ffmpeg-decoder and desktop UI only.
- Tests exercise sockets, files, decoders and GPU; build/bin are ignored.
- User approval to execute includes routine interface decisions; no new scope confirmation required.

## Task 1: TCP service and upload

Create service/{protocol,crypto,local-server,service-client}.{h,cpp}, service/server-main.cpp; tests/service-integration-test.cpp.
Produce ServiceClient pimpl facade, ServiceSnapshot/UploadSnapshot, async connect/register/login/logout/upload/pause/resume/cancel; LocalServer start/stop/port.
- [x] Write tests for fragmented JSON frames, bad password, account persistence, denied upload, real file hash/offset recovery and lost socket.
- [x] Build red test using temporary standalone MSVC/CMake target; implement production types and functions.
- [x] Implement protocol, password hashing, atomic records, server, heartbeat/reconnect client and resumed upload.
- [x] Run tests and record exact public header contract for parent UI.
- [x] Independent scoped review and fixes.

## Task 2: OpenGL rendering and native YUV decode

Create render/opengl-renderer.{h,cpp}; extend media/video-decoder.h, ffmpeg-video-decoder.{h,cpp}, video-frame-queue.h and app/playback-session.h fields only. Parent adapts ffmpeg-decoder/session copying.
Produce OpenGLRenderer initialize/resize/updateVideo/frameTexture/begin/present/saveScreenshot/shutdown plus VideoEffects; VideoPixelFormat {Bgra,Yuv420p}, planes/strides; nativeYuv in VideoDecoderConfig.
- [x] Real GL color fixture test fails without rendering; test backend native YUV avoids BGRA path.
- [x] Vendor matching official ImGui OpenGL backend, WGL context and loaded GL functions.
- [x] Texture shader conversion, rotation/zoom/filter FBO, pixel readback screenshot.
- [x] Build scoped rendering test and hand exact header APIs to parent.
- [x] Independent review.

## Task 3: Progressive FLV/HLS cache

Create media/progressive-cache.{h,cpp}, media/loopback-http.{h,cpp}; tests/cache-integration-test.cpp and tests/make-stream-media.cpp, tests/cache-playback-test.cpp.
ProgressiveCache start(url,root), snapshot(), playbackUrl(), pause/resume/cancel/stop; state + progress + byte ranges + buffer wait. Loopback endpoint handles GET/HEAD/Range and playlist/segment requests.
- [x] Real limited-speed HTTP fixture; red assertions: read beginning before download complete, missing ranges never zero-filled/EOF, resume partial segment.
- [x] HTTP fetch headers/Range, atomic checkpoint scheduling4/512KiB, dynamic read priority and blocking cache HTTP.
- [x] HLS VOD parsing and rewritten endpoints, segment workers with persistent partial offsets.
- [x] Validate complete files, cancel/reopen and forced exit checkpoints with request byte logs.
- [x] Independent review and fixes.

## Task 4: Media session and UI integration

Modify app/playback-session.*, media/ffmpeg-decoder.*, app/desktop-player.cpp, CMakeLists.txt/VS integration. Add buffer status probe and planar copying, normal/pip compatibility. OpenGL choice selected per process before init.
- [x] Regression tests exercise native planar publish/seek/end and direct network/cached source.
- [x] Session decodes native YUV when OpenGL; network starvation pauses audio output and the playback clock, and EOF releases queued tail audio.
- [x] UI network/connection/account/transfer pages and status/errors, render effects and owned LocalServer lifecycle plus standalone service executable.
- [x] Build and smoke actual windows; controls and server/transfer happy/error paths.

## Task 5: Whole system delivery

- [x] All original and new tests Release/Debug, run actual FLV/HLS progressive decode, socket reconnect and upload resume, forced-exit byte redo bounds. Final root suites: 17/17 Release (185.13s), 17/17 Debug (232.67s); VS Release build succeeds.
- [x] Review full diff and fix material findings. Cache generation/source integrity, validated HTTP body, connection reclamation, service directory ownership/revoked auth/cancel acknowledgement, and audio-only EOF recovery have real regressions.
- [x] README + protocol spec + launch demo scripts + validation record with concrete limits.
- [x] Final report executable paths and exact completed scope; evidence in docs/validation/2026-10-13-second-layer.md. Real demo script launch and original PiP ready/ended confirmed. No third-layer delivery claims.
