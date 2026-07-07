# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What Trace is

A fast, minimal Windows desktop media player for professional review workflows (editors, VFX artists, motion designers, AI video creators). Three pillars: **Simple** (clean black stage, no libraries/playlists), **Fast** (instant launch/load, responsive scrub), **Trustworthy** (frame order, stepping, and timing must be exact — "next frame" means the actual next frame). It sits between editing/compositing apps: open a render, check a frame, move on. Not an editor, not an asset manager.

Current alpha focus: 4K H.264 MP4 + ProRes MOV playback, reliable reverse playback, frame-accurate stepping, trustworthy scrubbing. Formats and UI features come after the playback foundation is dependable. Longer-term: image sequences, EXR, OCIO color management, timecode/frame HUD (partially present).

Owner context: Anj is a VFX/motion-design lead, not a programmer. Explain things plainly; he tests builds on a Windows RTX 4090 box; development happens on macOS. Don't ask him to debug code — give exact copy-paste terminal commands when he needs to run anything.

## Build and test

There is no local build on the Mac (no Qt6/FFmpeg toolchain assumed) and no test suite yet. **GitHub Actions is the source of truth for builds.**

- Repo: `https://github.com/bigsbypuglise/trace-alpha` (GitHub account: bigsbypuglise; private)
- Every push to any branch builds Windows (VS2022, Qt 6.7.2 via install-qt-action, FFmpeg via vcpkg) and uploads artifact `trace-alpha-windows-x64.zip` (workflow: `.github/workflows/windows-release.yml`)
- Tags matching `v*` also publish a GitHub prerelease with that ZIP
- Claude's sandbox cannot push (proxy blocks github.com) — commit locally, then have Anj run `cd ~/Claude/OpenClaw/trace && git push origin main`
- Manual validation checklist: `docs/windows-validation-checklist.md`
- Local build if a toolchain exists: `cmake -S . -B build && cmake --build build --config Release --target Trace`

FFmpeg and OpenImageIO are optional at compile time (`TRACE_WITH_FFMPEG`, `TRACE_WITH_OIIO` defines). Code touching them must stay inside those guards and compile without them.

## Architecture

Qt6 Widgets app, C++20, CMake. Single executable target `Trace` defined in `app/CMakeLists.txt` (sources live in `src/`).

The core abstraction is `trace::core::FrameSource` (`src/core/FrameSource.h`): a pull-based interface — `frameAt(frameIndex, outImage, error)`. Two implementations:

- `VideoFrameSource` → thin wrapper over `VideoDecoderFFmpeg` (mp4/mov)
- `ImageSequenceFrameSource` → `StillImageLoader` (stills and numbered sequences via `SequenceParser`; OIIO when available, QImage otherwise)

`MainWindow` (`src/app/MainWindow.cpp`) owns everything and drives playback **synchronously on the UI thread**: a `QTimer` at ~1/fps ticks → `PlaybackController` (pure state machine: mode/speed/current frame, J-K-L jog, stepping) computes the target frame → `loadCurrentFrame()` pulls from the FrameSource → `ViewerWidget` paints. `TransportOverlay` is the HUD; `refreshHud()` builds a dev diagnostics line from `VideoPerfStats`. There is deliberately **no decode thread** — an async prefetch pipeline was tried and reverted (commits a171e3a/1d280eb, reverted 9cd2a0c/a2f7999) because it broke frame ordering.

`VideoDecoderFFmpeg` (`src/core/VideoDecoderFFmpeg.cpp`) is where nearly all playback complexity lives:

- **Linear forward decode** is the invariant. Frames map PTS→index via `frameFromPts` with a monotonic bump to prevent frame-order bounce (commit 7a3fa95). Playback decodes exactly one frame per request — steady per-tick cost.
- **Request modes** (`Playback`/`Scrub`/`Step`) change behavior: seeks happen on scrub, backward moves, or jumps >1; sws conversion quality is mode-aware (fast flags for Playback/Scrub, `SWS_FULL_CHR_H_INT|SWS_ACCURATE_RND` for Step, since a paused frame is being inspected). Env overrides for A/B: `TRACE_PERF_FAST_CONVERT=1`, `TRACE_PERF_ACCURATE_CONVERT=1`.
- **Reverse playback/stepping** works from a 12-frame `reverseCache` filled with frames decoded en route to a target; a cache miss triggers seek-to-keyframe + decode forward.
- swscale is slice-threaded (threads=auto) when FFmpeg ≥ 5.1 at build time.

Scrubbing is throttled in `MainWindow` (12 ms single-shot `scrubTimer_` coalesces slider moves; release forces exact frame).

## Decisions already made — don't relitigate casually

- **No async decode thread** until there's a design that provably preserves frame order; the March 2026 attempt was reverted. If revisiting, sequence-number every request and drop stale results.
- **Forward-fill queue was removed** (July 2026): it decoded up to 4 frames per timer tick in bursts and caused rhythmic stutter on 4K ProRes. Don't re-add synchronous read-ahead.
- **Seek lands on keyframe labeled as target** (from bounce fix 7a3fa95): after a seek, the first decoded frame is bumped to the requested index. Exact for all-intra codecs (ProRes); on long-GOP H.264 a scrub shows the nearest keyframe. This is a known accuracy gap, not accidental — fixing it means decoding forward from keyframe to true target without breaking scrub responsiveness.
- Video playback never skips frames (timer clamps steps to 1 for video) — heavy files slow down rather than drop frames. Deliberate: ordering over rate, for now.
- Windows ships as **portable ZIP only** — no installer until packaging/playback stabilize (`docs/release-notes-alpha.md`).
- **Scrub shows a half-res preview at ≥1920px wide sources** (July 2026): sws conversion dominates 4K frame cost; the viewer upscales to fit. The landing frame (slider release) is always full-res accurate via Step mode, and half-res previews never enter the reverse cache. Don't "fix" scrub softness by removing this — fix it by making conversion faster.
- **Transport widgets must not take keyboard focus** (`setFocusPolicy(Qt::NoFocus)` on the slider): keyboard belongs to frame stepping and J-K-L. If a new widget steals arrows/space, this is why.

## Roadmap (rough priority)

1. ~~Validate the July 2026 ProRes perf fix~~ **Validated 2026-07-06**: 4K ProRes plays smooth on the 4090 box (dec ~1ms, cvt ~14ms). Next: validate the scrub-responsiveness + arrow-key fixes, and test 4K H.264 MP4.
2. Frame-exact scrub/step on long-GOP H.264 (see keyframe decision above)
3. Reliable reverse playback beyond the 12-frame cache (ProRes is cheap — every frame is a keyframe; H.264 needs GOP-aware backward buffering)
4. EXR / image-sequence review polish, OCIO display transform (TODO marker in `StillImageLoader.cpp`)

## Working conventions

- Commit style follows the existing log: `playback:`, `perf:`, `ci:`, `docs:`, `fix(windows):` prefixes with imperative subjects.
- Keep changes conservative and testable per push — Anj can only validate via CI ZIP builds on Windows, so each push should be a coherent, revertable step.
- Update this file's Roadmap/Decisions sections at the end of each working session so the next session starts current.
