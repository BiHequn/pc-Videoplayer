# Desktop Player Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development for independent media work and review. Steps are tracked below.

**Goal:** Complete the user-approved first layer with a runnable desktop player.

**Architecture:** A Win32/ImGui shell owns rendering; one playback worker owns the legacy media backend and publishes snapshots and BGRA frames. Retain the PiP runtime behind `--pip`.

**Tech Stack:** Windows x64, C++17, Dear ImGui Win32/DX11, FFmpeg/MF, CMake/MSBuild.

**Spec:** `docs/superpowers/specs/2026-10-13-desktop-player-design.md`

## Global Constraints

- Preserve pre-existing project changes. The user authorized committing and pushing the completed work on 2026-10-13.
- All decoder lifecycle and control operations run on the playback worker.
- Chinese dark UI; no placeholder account/upload features.
- Dependency source pinned; document rebuild steps.

## Task 1: Media controls and regression checks

- [x] Add real-backend failing tests for duration, seek bounds, volume, cancel/reopen and silent-video clock progress.
- [x] Add duration, absolute seek, volume, effective backend, frame clock anchor and cancellation interfaces to `media/ffmpeg-decoder.*`.
- [x] Ensure reopen resets rate and clocks; pause/seek preserves requested paused state at session layer.
- [x] Build and run the tests with actual local media.

## Task 2: Background playback session

- [x] Add integration tests for idle -> opening -> playing, pause/resume, stop, error/reopen, seek and natural completion.
- [x] Implement `PlaybackSession` with a command queue, mutex snapshot, published frame and joined worker destruction.
- [x] Verify software playback before testing Auto fallback separately.

## Task 3: Desktop rendering and controls

- [x] Vendor pinned Dear ImGui sources and license; add CMake and existing VS project integration.
- [x] Create D3D11 device/swapchain, offscreen video target and ImGui render loop.
- [x] Implement Chinese empty/loading/playback/error states, open/drop, pause/stop/replay, timeline, volume, brightness, decoder selection, full screen and diagnostics.
- [x] Default entry starts desktop shell; `--pip` invokes existing runtime.
- [x] Compile Release and Debug; inspect the live window and the app's own framebuffer captures.

## Task 4: Verify and document

- [x] Run integration tests, real GUI smoke checks and independent code review.
- [x] Fix material findings; rerun affected checks.
- [x] Write README and validation record, including remaining limits and executable location.

## Execution rulings

- Continue in the current `dev` checkout because the accepted scope explicitly builds on existing uncommitted media changes. A new checkout would omit them.
- Execute tightly coupled UI/session work locally and delegate the disjoint media controls/tests and final review. No extra execution-choice approval is needed after B.

## Final evidence

- Final CMake Release and Debug: all four CTest entries passed, including 22 real media checks.
- Visual Studio Release / x64: built successfully.
- Real 1080p H.264 MP4 Auto desktop smoke: passed; effective backend Media Foundation.
- PiP regression: exit 0 with ready and ended events.
- See `docs/validation/2026-10-13-first-layer.md` for evidence, commands and limits.
