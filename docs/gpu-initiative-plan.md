# Trace GPU / smooth-presentation initiative — active plan

**Status: Gate A complete (architecture audit). No renderer code written yet.**

This document supersedes any earlier GPU design note. A `docs/gpu-conversion-spec.md`
was referenced by the initiative brief but **does not exist in this repository** and never
did on this branch — if one reappears from another checkout, this file wins.

---

## 1. Audit — the current AVFrame → pixel path

```
av_read_frame / avcodec_receive_frame
  -> AVFrame (yuv420p / yuv422p10le / yuva444p12le ...)
  -> alphaStrippedFormat() re-describes planar YUVA as alpha-less
  -> sws_scale into a RECYCLED QImage borrowed from Impl::convertPool
       (Format_RGB32, i.e. BGRA; half-res at >=1920px wide in Scrub mode)
  -> VideoDecoderFFmpeg::decodeFrameAt(long long, QImage& out, QString&, RequestMode)
  -> VideoFrameSource::frameAt(...)                 [FrameSource interface]
  -> MainWindow::loadCurrentFrame(...)
  -> ViewerWidget::setImage(const QImage&)          [shallow, implicitly-shared assign]
  -> ViewerWidget::paintEvent
       fitted = image_.size().scaled(size(), KeepAspectRatio)
       SmoothPixmapTransform = (fitted != source size) && !TRACE_NEAREST_SCALE
       p.drawImage(target, image_)
```

**Where BGRA is created:** `sws_scale`, inside `VideoDecoderFFmpeg`. There are four sws
context slots keyed on the full tuple (geometry + fast/accurate flag), with colorimetry
applied per slot via `sws_setColorspaceDetails`.

**Where QImage ownership begins:** `Impl::convertPool`, a `std::deque<QImage>` sized
`reverseCacheCapacity + 4`. It exists because `QImage::bits()` was detaching and deep-copying
~37.7 MB per frame while the viewer and cache still referenced the buffer. Frames leave the
decoder as implicitly-shared QImages; the pool only reuses a buffer no one else still holds.

**QImage lifetime:** shared between (a) the convert pool, (b) `ViewerWidget::image_`,
(c) `reverseCache` entries. Any of the three can be the last owner. This is the single most
important constraint on the GPU design — see §4.

**Cache representation today:**

```cpp
struct CachedFrame { long long frame; QImage image; };
std::deque<CachedFrame> reverseCache;   // FIFO, evict from front
```

Capacity is footprint-derived at open: `192MB / (w*h*4)`, clamped to `[4, 32]`.

| source | frame bytes | capacity |
|---|---|---|
| 1920x1080 | 8.29 MB | **24** |
| 3840x2160 | 33.18 MB | **6** |
| 4096x2304 | 37.75 MB | **5** |

> The initiative brief said "~32 at 1080p / ~7 at 4K". That is wrong, and it came from a
> stale comment at `VideoDecoderFFmpeg.cpp:648`. The clamp ceiling is 32 but the footprint
> math never reaches it at 1080p. HUD confirms 24/24, 6/6, 5/5. **Fix that comment.**

**Interfaces that currently require QImage** — the complete list, and it is short:

1. `FrameSource::frameAt(long long, QImage&, QString&)`
2. `VideoDecoderFFmpeg::decodeFrameAt(long long, QImage&, QString&, RequestMode)`
3. `ViewerWidget::setImage(const QImage&)`
4. `StillImageLoader` / `LoadedImageInfo::image`, and `FrameCache`

Four seams. The renderer boundary is genuinely small.

## 2. Audit — thread ownership

| thread | owns |
|---|---|
| **UI / main** | video demux, decode, swscale, frame cache, playback timer, scrub timer, paint, HUD |
| **Audio decode** | `AudioOutput::Impl::DecodeThread` — its own `AVFormatContext`, its own ring. Fully isolated from video. |
| **Storage read worker** | `MediaIoSource`, **remote sources only**. Local keeps the direct synchronous read. The UI thread pumps the event loop while it waits. |

There is deliberately **no video decode thread**. Two consequences for this initiative:

- `storageBusy_` already exists as a re-entrancy guard — the established precedent for "the
  decoder is busy, defer this input". The async scrub design should extend that idea rather
  than invent a parallel one.
- Everything in the pixel path above runs on one thread, so introducing a renderer boundary
  needs no locking today.

## 3. Audit — windowing, resize, fullscreen

```
QVBoxLayout (central)
  viewer_        stretch 1     ViewerWidget : QWidget
  transportBar_  stretch 0
  overlay_       stretch 0     TransportOverlay  (HUD — BELOW the video, not over it)
```

**Nothing composites on top of the video rect.** This is the finding that most changes the
native-D3D11 cost estimate: the usual blocker for a native child HWND inside Qt — overlay
widgets not compositing with it — does not apply to Trace.

- **Resize:** pure Qt. `paintEvent` refits every paint; no renderer state to invalidate.
- **Fullscreen:** `setWindowState(windowState() ^ Qt::WindowFullScreen)` on the MainWindow.
  The layout persists, so the HUD and transport stay visible. No exclusive-fullscreen path.
- **Device / swapchain lifetime:** none today.

Three native-HWND hazards that *do* apply and must be handled in the prototype:

1. **Drag-and-drop.** `MainWindow::dragEnterEvent` / `dropEvent` implement drop-to-open. A
   native child HWND can swallow drops over the video area.
2. **`setCenterText`** ("Drop media or File > Open") is painted by ViewerWidget when there is
   no image. With no frame there is nothing to present — decide whether D3D draws it or the
   Qt widget stays until first frame.
3. **Resize flicker** — `IDXGISwapChain::ResizeBuffers` driven from Qt's `resizeEvent`, with
   `WA_PaintOnScreen` / `WA_NoSystemBackground` to stop Qt erasing the surface.

## 4. Decision — frame representation and ownership

**Decoded frames stay CPU-resident. GPU textures are presentation scratch, never frame identity.**

```cpp
// Refcounted, renderer-agnostic. Replaces bare QImage at the four seams in §1.
struct VideoFrame {
    std::shared_ptr<FrameBuffer> buffer;  // recycled, from the existing convert pool
    long long frameIndex;
    int width, height;
    AVPixelFormat format;                 // BGRA today; planar YUV in Phase 5
    ColorInfo color;                      // matrix + range, already tracked per sws slot
};
```

- `QImage` becomes a **view** the CPU renderer constructs over `buffer` (zero-copy via the
  `QImage(uchar*, w, h, bytesPerLine, format)` ctor plus a cleanup functor holding the
  shared_ptr). D3D11 uploads from the same `buffer`.
- The existing `convertPool` becomes the `FrameBuffer` allocator. Same recycling, same
  detach-avoidance, now explicit rather than relying on QImage's implicit sharing.

Answering the brief's ownership questions directly:

| question | answer |
|---|---|
| Who owns a decoded frame? | `VideoFrame`'s `shared_ptr<FrameBuffer>`. Cache, viewer and in-flight request are all just holders. |
| Who owns its GPU texture? | The renderer, exclusively. Textures are a small fixed ring of upload targets, not keyed to frame identity. |
| When can either be recycled? | Buffer: when the last `VideoFrame` releases it. Texture: after its present completes; it is never the only copy of anything. |
| Can a stale request release it immediately? | Yes — dropping the `VideoFrame` is the entire cost. No GPU teardown involved. |
| What if the renderer changes? | Nothing. Frames are renderer-agnostic; the new renderer re-uploads from the same buffers. |
| What if the media changes? | Cache cleared, generation bumped; buffers free as holders release. |
| What after device loss? | Drop all textures, recreate device, re-upload from CPU frames. **No frame is ever lost.** |

**Why this over GPU-resident caching:** it keeps the validated 192 MB system-RAM cost model,
removes the VRAM budget question entirely, makes device loss trivially recoverable, and
decouples the async-scrub design from the renderer design.

**Honest cost:** it forecloses "decode straight into a GPU surface and keep it there", which
is what D3D11VA / NVDEC eventually want. Mitigation: define `VideoFrame` as a variant
(`CPU planes | GPU surface`) from day one and implement only the CPU arm. Cheap now, avoids
a rewrite in Phase 14.

## 5. Decision — renderer backend

**Native D3D11 + DXGI**, hosted in the existing Qt Widgets app.

- The usual argument for QRhi (Qt handles compositing) is **weak here**, because nothing
  composites over the video rect (§3).
- CI pins **Qt 6.7.2**; `QRhiWidget` was *introduced* in 6.7 and local is **6.10.2**. A
  3-minor skew on a new, evolving API is a real risk under the repo's "green must mean
  launchable" rule.
- Qt's RHI does not expose waitable swapchains or DXGI frame-latency control, which is a
  stated core payoff of the initiative (Phase 13). Choosing QRhi would build a dead end into
  the presentation-clock work.

**First supported pixel format:** `yuv420p` 8-bit, 1080p H.264 — matching the brief.
Everything else falls back to `CPU_QIMAGE` automatically.

**Build notes:** link `d3d11.lib` and `dxgi.lib`. **Compile shaders at build time with fxc**,
not at runtime with `D3DCompile` — `d3dcompiler_47.dll` is not deployed by `windeployqt`, and
shipping a build that configures green but cannot create a shader is exactly the failure mode
the CI rules exist to prevent. CI runners have no GPU, so D3D11 will land on WARP; have CI
assert the renderer *initializes* so a broken path fails the build rather than silently
falling back forever.

`TRACE_RENDERER=cpu` stays the default until Gate E.

## 6. Post-mortem — the two reverted async attempts

Commits `a171e3a` (async prefetch pipeline) and `1d280eb` (timing governor), reverted by
`9cd2a0c` / `a2f7999`. The revert messages are bare, so this is read from the code.

Note first: these were **playback read-ahead**, not scrub. Their lessons transfer partially.

`PlaybackPrefetcher` ran a `std::thread` worker with `std::deque<long long> pending_` and
`std::unordered_map<long long, PrefetchedFrame> ready_`. Defects, in order of severity:

1. **It opened a second `VideoDecoderFFmpeg` on the same file** (`VideoDecoderFFmpeg
   videoDecoder;` local to `workerLoop`). Two decoders, two independent linear positions, two
   seek histories. `frameFromPts` and its monotonic index bump are *per-decoder state*, so the
   two could disagree about which index a PTS maps to. This alone breaks frame ordering.
2. **No generation or sequence number anywhere.** `pending_` is FIFO, so superseded requests
   are decoded to completion before newer ones. There is no way to express "the target moved."
   This is precisely the "request A, B, C while the pointer is already at Z" failure.
3. **Results were keyed by the frame that was *landed on*, not the one requested** —
   `prefetched.frameIndex = videoDecoder.currentFrame();`. `take(frameIndex)` does an exact
   lookup, so an off-target landing is filed under a key nobody asks for: guaranteed miss,
   duplicate decode on the main decoder, and wasted worker time.
4. **Stale-insert-after-reset race.** `resetUnlocked()` clears the containers, but a decode
   already in flight re-acquires the lock afterwards and inserts its now-irrelevant result into
   `ready_`. Nothing checks whether the world moved.
5. **Eviction picks the numerically lowest frame index.** During reverse playback the lowest
   index is exactly what is needed next — wrong by construction in one direction.
6. **`stop()` joins the worker while it may be mid-GOP-walk**, blocking the UI thread for the
   duration of a decode with no cancellation check inside the walk.

**What a third implementation must do differently:**

- **One decoder, one owning thread.** Never a second decoder on the same file.
- **Monotonic generation ID on every request, carried through to the result.** Anything with
  `generation < current` is dropped at the boundary and never reaches the viewer.
- **Key results by the requested index *and* generation**, never by the landed index.
- **Latest-wins target slot, not a FIFO queue.** Depth 1. A new target overwrites the pending
  one; it does not queue behind it.
- **Cooperative cancellation checked inside the GOP walk**, so both supersede and shutdown are
  bounded. The existing `MediaIoSource` cancellation rule applies: a superseded read still runs
  to completion and is *reported stale* — never abandoned mid-buffer.
- **Frame ownership via `VideoFrame` refcount**, so discarding a stale result costs one
  decrement.

This is Gate D material. **Not implementing it yet.**

## 7. Impact on the existing scrub path

`queueVideoScrubFrame` sets `pendingScrubFrame_`; a 12 ms single-shot `scrubTimer_` coalesces;
`flushVideoScrub` decodes **synchronously on the UI thread** and re-arms the timer if the
target moved while it was decoding.

The *shape* of latest-wins already exists (`activeScrubFrame_` skips unchanged targets, and the
re-arm handles a target that moved mid-decode). What is missing is the ability to **abandon an
in-flight decode**. That is the whole delta, and it is why the async work is a contained change
rather than a rewrite.

## 8. Commit plan

Each is independently reviewable and revertable. Gates in **bold**.

1. `docs: supersede the GPU spec with the audited initiative plan` — this file. *(done)*
2. `refactor(core): introduce VideoFrame and retire bare QImage at the frame seams`
   — the four interfaces in §1; CPU renderer constructs a QImage view. No behaviour change.
   **Full CPU regression pass. Stop and validate.**
3. `refactor(renderer): introduce the CPU/GPU renderer boundary` — `VideoRenderer` interface,
   `CpuImageRenderer` as the only implementation, `TRACE_RENDERER` selector.
   **Stop and validate. → GATE A complete in code.**
4. `refactor(scrub): add generation-numbered frame requests` — IDs and stale-drop plumbing,
   still synchronous. Pure bookkeeping, no threading.
5. `perf(scrub): move random-access decode to a worker with latest-target-wins` — **GATE D.**
6. `feat(gpu): add experimental native D3D11 video surface` — **GATE B** (frame, stride,
   aspect, resize, fallback).
7. `feat(gpu): add planar YUV upload and shader colour conversion` — **GATE C.**
8. `perf(gpu): reuse textures and upload resources`
9. `perf(gpu): add GPU scaling and telemetry`
10. `feat(gpu): add high-bit-depth ProRes presentation`
11. `perf(gpu): add DXGI presentation timing` — **GATE E** before any default change.

Steps 4–5 deliberately precede the GPU work: they are renderer-independent, they address the
half of the product complaint that GPU presentation cannot fix, and with §4 in place they do
not depend on any GPU decision.

## 9. Open items carried into Phase 13

- **Audio/vsync composition must be stated before it is built.** As of `cd79d49`, when audio
  drives, the audio clock decides *both* when to present and which frame — that unification is
  what removed the hold/skip churn. A vsync-driven presentation model also wants to decide
  *when*. Rule: **audio stays the rate/position authority; vsync becomes the phase authority.**
  Vsync picks the instant, the audio clock picks the frame for that instant. One owner per
  question, or the two-scheduler bug comes straight back.
- **10-bit output is separate from 10-bit processing.** GPU high-bit-depth conversion avoids
  banding, but true 10-bit display needs an `R10G10B10A2` swapchain and a display in 10-bit
  mode. Do not conflate them in Phase 11 claims.
- **Cheap CPU control for scaling quality.** Converting to display size *in swscale* is both
  cheaper and higher quality than full-res convert plus Qt's bilinear (already noted in
  CLAUDE.md). Worth measuring as a control so Phase 7 cannot claim a win that was really just
  "we stopped using a bad filter."
