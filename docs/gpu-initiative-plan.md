# Trace GPU / smooth-presentation initiative — active plan

**Status: GATE B PASSED (2026-08-09).** Steps 1–6 of §8 are committed and
validated on the local Windows toolchain: `VideoFrame` replaced bare `QImage` at
the four seams (`03d840e`), the `VideoRenderer` boundary exists (`5765c19`),
generation plumbing landed (`75a3412`, §13), random-access scrub decode runs on
a worker with latest-target-wins (`f77d472`, §14), the drag preview samples on
all-intra media (§15), play/pause survives a drag (§16), and **there is now a
native D3D11 surface** (§17) — opt-in via `TRACE_RENDERER=d3d11`, with
`CpuImageRenderer` still the default.

Steps 5 and 5.5 are **owner-signed-off** (2026-08-09, §15.5 item 3). GATE B is
harness-validated only (§17.4); **owner validation of the rendered picture has
not happened**, and 4K ProRes 422 HQ remains the bar.

**Next is step 7 — GATE C**, planar YUV upload and shader colour conversion.
That is where conversion cost actually moves: today the frame is still converted
by swscale on the CPU and uploaded as BGRA. Carry the two deferred scrub defects
in as regression tripwires (§15.5).

This document holds the **decisions**. The requirements it answers are in
`docs/gpu-initiative-brief.md`; where the two differ, this file wins.

The `gpu-conversion-spec.md` the brief says to delete is the OpenGL/QOpenGLWidget design.
It is archived at `docs/archive/gpu-conversion-spec-SUPERSEDED.md` — not deleted, because its
colorimetry and bit-depth tables were the only written record of those values. They are ported
into §11 below. Nothing else in it should be implemented; see §11 and the archive header for
which of its premises are stale.

---

## 1. Audit — the AVFrame → pixel path

> **This section is the audit as of `71872a2`, i.e. before steps 2–3 landed.** It
> is kept because it is the reasoning the design rests on, and because the four
> seams it identifies are exactly what was changed. For what the code does now,
> read §4 and §12: `QImage` has been replaced by `VideoFrame` at all four seams,
> the convert pool holds `FrameBuffer`s rather than `QImage`s, and the paint has
> moved into `CpuImageRenderer`.

```
av_read_frame / avcodec_receive_frame
  -> AVFrame (yuv420p / yuv422p10le / yuva444p12le ...)
  -> alphaStrippedFormat() re-describes planar YUVA as alpha-less
  -> sws_scale into a RECYCLED QImage borrowed from Impl::convertPool
       (Format_RGB32, i.e. BGRA; in Scrub mode above 1920px wide, converted to
        the DISPLAYED size -- capped at half res, never upscaled)
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
from the byte budget divided by the *smallest* entry the source can produce, clamped to
`[4, 128] + 4`. It exists because `QImage::bits()` was detaching and deep-copying
~37.7 MB per frame while the viewer and cache still referenced the buffer. Frames leave the
decoder as implicitly-shared QImages; the pool only reuses a buffer no one else still holds.

**QImage lifetime:** shared between (a) the convert pool, (b) `ViewerWidget::image_`,
(c) `reverseCache` entries. Any of the three can be the last owner. This is the single most
important constraint on the GPU design — see §4.

**Cache representation today** (updated 2026-08-07, `b5a56af`):

```cpp
struct CachedFrame { long long frame; QImage image; bool previewRes; };
std::deque<CachedFrame> reverseCache;   // FIFO, evict from front
```

Eviction is by **bytes, not entry count**: a running `reverseCacheBytes` against a
192MB budget. This matters to the GPU design because **entries are not all the same
size** — a Scrub preview is converted to the displayed size and can be a fiftieth of
a full-res frame — and `previewRes` entries are refused for Step/landing requests,
which is what keeps a soft frame from ever being inspected. Any GPU-side frame cache
inherits both properties: mixed-resolution entries priced honestly, and a tag saying
what an entry is fit to serve.

Observed occupancy at a 1280x760 window (previews ~640x360, 0.9MB):

| source | full-res frame | entries held |
|---|---|---|
| 1920x1080 | 8.29 MB | 24 (full-res path, not halved at 1920) |
| 3840x2160 | 33.18 MB | **~150 previews** (191.6 MB) |
| 4096x2304 | 37.75 MB | **~104 previews** (191.6 MB) |

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
2. ~~**`setCenterText`**~~ **Settled in `5765c19`:** the renderer owns the whole
   paint, placeholder included, because two painters cannot share one paint event
   and a D3D11 backend will not be using `QPainter` at all. Whichever backend is
   installed draws it. See §12.
3. **Resize flicker** — `IDXGISwapChain::ResizeBuffers` driven from Qt's `resizeEvent`, with
   `WA_PaintOnScreen` / `WA_NoSystemBackground` to stop Qt erasing the surface.

## 4. Decision — frame representation and ownership

**Decoded frames stay CPU-resident. GPU textures are presentation scratch, never frame identity.**

**As shipped in `03d840e`** (`src/core/VideoFrame.h`); it differs from the sketch
this section originally carried, in three ways that are worth keeping:

```cpp
struct VideoFrame {
    std::shared_ptr<FrameBuffer> buffer;  // recycled, from the old convert pool
    long long frameIndex = -1;
    ColorInfo color;                      // matrix + range + whether inferred
    bool previewRes = false;              // fit to shuttle past, not to inspect
};
```

- **`PixelLayout`, not `AVPixelFormat`.** The sketch put an FFmpeg type in the
  struct, but this header is reached from `FrameSource.h` and therefore from the
  image-sequence path, which must compile with `TRACE_WITH_FFMPEG` undefined.
  `PixelLayout` has one value today (`BGRA8`); planar YUV joins it when something
  can produce one. The layout lives on `FrameBuffer`, with `width`/`height`,
  rather than being duplicated on the frame.
- **`previewRes` moved onto the frame.** It was a field of the decoder's private
  cache entry, predicted at the call site by an expression that had to agree with
  the resolution branch inside the converter — and the two read different widths
  (container metadata vs the decoded frame). The converter sets it now from the
  size it actually converted at, so an entry cannot be stored at one resolution
  and labelled another. The old prediction survives only to size the seek-walk
  fill window, which has to be decided before any frame exists.
- **`FrameBuffer` has two origins**: `allocate()` for swscale destinations, and
  `adopt(QImage)` for the still/sequence path, where pixels come from OIIO or
  QImage rather than a conversion. `adopt` detaches on the way in so the buffer
  is the sole owner, which is the same no-aliasing guarantee `allocate` gets for
  free.

**The detach hazard is now structural, not managed.** The pool used to hand back
only entries reporting `isDetached()` because `QImage::bits()` deep-copied ~38MB
at 4K whenever the buffer was still referenced. swscale writes to
`buffer->data()` now, which cannot detach; the pool's free test is
`use_count() == 1`. The `detach` HUD counters are retained and read **0.00 by
construction** — kept so a regression back to the old behaviour would still be
visible.

**A failed conversion reports failure.** `convertCurrentFrame` returns bool and
clears the output frame on entry. The output is frequently the same object across
requests, so the previous behaviour left the *previous* frame in it and the
caller then stamped it with the new index — one frame on screen under another's
name, which is exactly what `e76eabb` exists to prevent.

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

`queueVideoScrubFrame` sets `pendingScrubFrame_`; a 12 ms single-shot `scrubTimer_` coalesces
the *first* update; `flushVideoScrub` decodes **synchronously on the UI thread**. A drag is a
walk: it steps the decoder through every frame between the picture and the pointer, spending up
to `kScrubWalkBudgetMs` (8 ms) per slice and re-arming at **zero interval** while it is still
behind — the coalescing interval deliberately does not throttle catch-up.

The *shape* of latest-wins already exists (`activeScrubFrame_` skips unchanged targets, and the
re-arm handles a target that moved mid-decode). What is missing is the ability to **abandon an
in-flight decode**. That is the whole delta, and it is why the async work is a contained change
rather than a rewrite.

**Two measured facts from 2026-08-07 that the async design should aim at, because they are the
current scrub complaints and neither is a rendering problem:**

- **Stalls, not burstiness, are what a drag feels like.** A fast scrub carries 7–8 gaps of
  30–116 ms (4K H.264) and 21 (1080p), each one a cache miss forcing a seek plus a GOP walk
  against ~0.5 ms for a hit. Paint scheduling cannot reach them — pacing was measured twice and
  rejected (see CLAUDE.md). Moving decode off the UI thread lets a miss be absorbed instead of
  blocking, which is the strongest argument for async that exists, and it is independent of the
  renderer.
- **The slider handle itself trails the pointer** on heavy media, because the walk loop
  saturates the UI thread and mouse-move events queue behind decode work. This is a scheduling
  problem, not a painting one, and async is its actual fix. `kScrubWalkBudgetMs` and the
  zero-interval re-arm are the two knobs that decide how much of the thread the shuttle takes —
  worth an A/B *before* async, as a cheap control.

## 8. Commit plan

Each is independently reviewable and revertable. Gates in **bold**.

1. `docs: supersede the GPU spec with the audited initiative plan` — this file. *(done)*
2. `refactor(core): introduce VideoFrame and retire bare QImage at the frame seams`
   — the four interfaces in §1; CPU renderer constructs a QImage view. No behaviour change.
   **Full CPU regression pass. Stop and validate.** *(done, `03d840e` — see §12)*
3. `refactor(renderer): introduce the CPU/GPU renderer boundary` — `VideoRenderer` interface,
   `CpuImageRenderer` as the only implementation, `TRACE_RENDERER` selector.
   **Stop and validate. → GATE A complete in code.** *(done, `5765c19`)*
4. `refactor(scrub): add generation-numbered frame requests` — IDs and stale-drop plumbing,
   still synchronous. Pure bookkeeping, no threading. *(done, `75a3412` — see §13)*
5. `perf(scrub): move random-access decode to a worker with latest-target-wins` — **GATE D.**
   *(done, `f77d472` — see §14; decoder-side cancellation landed first in `ff55d4e`)*
6. `feat(gpu): add experimental native D3D11 video surface` — **GATE B** (frame, stride,
   aspect, resize, fallback). *(done — see §17)*
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
- **The cheap CPU control for scaling quality has already been taken — do not let a GPU phase
  claim it.** Converting to display size *in swscale* shipped for scrub previews on 2026-08-07
  (`b5a56af`): `sws 7.08 -> 1.87ms` on 4K ProRes 422 HQ, and the viewer now draws previews 1:1
  instead of bilinear-downscaling them. Phase 7 must measure against **this** baseline, not
  against the old full-res-convert-plus-Qt-bilinear path, or it will book a win that was
  really "we stopped using a bad filter" — which is exactly what this bullet was written to
  prevent, before the fix existed.

  What is genuinely left for the GPU here is the **landing frame** (Step mode), which still
  converts full-res and lets Qt scale it — a 6.4x downscale in the validation window. Measured
  local contrast between preview and landing is within 0.7%, so this is not currently a visible
  defect, but it is the remaining CPU-side scaling cost and the honest target for a GPU claim.

---

## 10. On "why async comes after GPU conversion"

The brief (Phase 12) argues async scrub should follow GPU conversion, because once CPU BGRA
conversion is gone "the decoder worker can hand off an `AVFrame` reference ... instead of
shipping a full RGB buffer. Smaller surface area, easier to get ordering right."

**That argument does not hold, and the ordering here is deliberately the other way.**

- `QImage` is already implicitly shared with an atomic refcount. Handing one across threads is
  *already* a refcount bump, not a copy. There is no "full RGB buffer" being shipped today.
- With `VideoFrame` (§4), the cross-thread payload is a `shared_ptr` either way. The GPU path
  does not make the handoff smaller or safer; the ownership model does, and that is §4's job.
- The brief's own §14 says the frame-representation question "collides with async scrub" and is
  "precisely the kind of unanswered question that turns attempt three into another revert."
  That is an argument that **ownership** must precede async — not that the *renderer* must.

There is one real residue of the argument: with planar upload the worker no longer runs
swscale, so its per-request cost falls and cancellation granularity improves. That is a
performance nuance, not a correctness one, and it does not gate the design.

Doing async first also means it is validated against the known-good CPU path, where a working
A/B harness and a measured baseline already exist — rather than against a renderer that is
itself new.

## 11. Colorimetry and bit-depth reference

Ported from the archived OpenGL spec, which is otherwise superseded. These values are
API-independent; only the texture format names change for D3D11.

### Plane formats

| Source format | Codec | Planes | D3D11 texture | Shader scale |
|---|---|---|---|---|
| `yuv420p` | H.264 8-bit | Y, U/2, V/2 | `DXGI_FORMAT_R8_UNORM` | 1.0 |
| `yuv422p10le` | ProRes 422 | Y, U/2 w, V/2 w | `DXGI_FORMAT_R16_UNORM` | 65535/1023 |
| `yuva444p12le` | ProRes 4444 | Y, U, V (A ignored) | `DXGI_FORMAT_R16_UNORM` | 65535/4095 |

`R16_UNORM` normalizes by 65535, but the sample occupies only the low 10 or 12 bits — hence
the scale. FFmpeg's `p10le`/`p12le` are LSB-aligned so these factors should be correct, but
**confirm empirically against a known test pattern rather than inheriting the assumption.**
A wrong factor shows as a global gamma/level shift, not as obvious corruption.

The shader never samples plane 3, which makes `alphaStrippedFormat()` unnecessary on the GPU
path — but do not remove it, the CPU path still needs it.

### Matrix selection

Read `AVFrame::colorspace` and `AVFrame::color_range`; the decoder already tracks both and
exposes them in the HUD `color` line. **As of `03d840e` this is also carried on the frame**
as `ColorInfo` (`matrix`, `fullRange`, `inferred`), mapped from the sws coefficient choice so
there is one decision rather than two — the GPU path needs it as shader constants, where
swscale is not the thing doing the conversion.

- `AVCOL_SPC_BT709` → BT.709
- `AVCOL_SPC_BT470BG` / `AVCOL_SPC_SMPTE170M` → BT.601
- `AVCOL_SPC_BT2020_NCL` / `_CL` → BT.2020
- `AVCOL_SPC_UNSPECIFIED` → infer by height: `>= 720` is BT.709, else BT.601
- `AVCOL_RANGE_JPEG` → full range; anything else → limited

### Normalization, before the matrix

```
limited:  y = (Y - 16/255) / (219/255)      c = (C - 128/255) / (224/255)
full:     y =  Y                            c =  C - 128/255
```

### Matrices (`u` = Cb, `v` = Cr, both centred at 0)

```
BT.709   R = y + 1.5748*v
         G = y - 0.1873*u - 0.4681*v
         B = y + 1.8556*u

BT.601   R = y + 1.4020*v
         G = y - 0.344136*u - 0.714136*v
         B = y + 1.7720*u

BT.2020  R = y + 1.4746*v
         G = y - 0.16455*u - 0.57135*v
         B = y + 1.8814*u
```

Pass the 3x3 and the two normalization terms as **shader constants**, not compiled variants.
BT.2020 gets the correct matrix but **no tonemap** — HDR/PQ content will still look wrong.
That is a known gap carried over from the CPU path, not a new regression.

---

## 12. The renderer boundary, as built (`5765c19`)

`src/render/VideoRenderer.h` — `initialize(host)`, `setFrame`, `clearFrame`,
`setPlaceholderText`, `resize`, `paint(host)`, `name()`, `stats()`.
`CpuImageRenderer` holds the existing path verbatim; `ViewerWidget` hosts it.

Three decisions here that the audit left open:

- **The renderer owns the whole paint, including the no-frame placeholder.** Two
  painters cannot share one paint event, and a D3D11 backend will not be drawing
  through `QPainter` at all, so the host must not draw alongside it. This settles
  §3 hazard 2 — D3D draws it, or rather, whichever backend is installed does.
- **No `QPainter` in the interface.** `paint(QWidget*)` lets the CPU backend
  construct its own painter and a GPU backend ignore the host and present to its
  swapchain, rather than passing a painter one of them would have to ignore.
- **`ViewerWidget` keeps the scheduling, the renderer keeps the pixels.** The
  widget owns when a repaint was requested and how long it took to arrive
  (`updateCount`, `lastUpdateToPaintMs`, `paintsWithoutNewImage`) because those
  are properties of the host's event loop, not of any backend. `RenderStats` is
  folded into `ViewerPerfStats` after each paint so the HUD reads one struct.

`TRACE_RENDERER` defaults to `cpu`; an unknown value warns on stderr and falls
back. The HUD `renderer` field names what is actually presenting — **a GPU path
that quietly never engages while the app looks fine is the failure mode this is
built against**, and it is the same reason CI should assert the renderer
initializes rather than merely that the app launches.

### Validation record for steps 2–3

1080p H.264 validation clip, local Windows toolchain, 1296x799 window. Both
commits measured; the figures below are step 3 and are unchanged from step 2.

| | result | recorded baseline |
|---|---|---|
| playback | 99.4% real time, 225 frames, rep 4 skip 0, `clk-upd 1/1` | 99.1%, rep 3–5, skip 0 |
| forward drag | `shown 239 delta 0`, 1.72ms/f, `lag 0f`, 235/236 painted, 3 seeks, 0 stalls | 1.79ms/f, `lag 0f` |
| backward drag | `shown 5 delta 0`, rev-hit 91.6%, `lag 0f` | 91–94% hits |
| reversals | `shown 5 delta 0`, rev-hit 93.4%, 760/761 painted, `lag 0f` | — |
| all of the above | `detach 0.00`, `stale-blocked 0`, `recov 0` | must stay 0 |
| PNG sequence | steps and cache-hits to the exact frame | — |

**Run the reversal set, not just a sweep.** A single smooth drag scores perfectly
on every number above and still misses correctness bugs: `2523d77` only appeared
under hard direction reversals held under one continuous press, running into both
ends of the clip. The reversal run is also the useful stress of buffer recycling —
it turned over ~730 cache evictions with `recov 0`.

---

## 13. Generation plumbing, as built (`75a3412`)

What §6 demands of a third async attempt, and where each part now stands:

| §6 requirement | status |
|---|---|
| Monotonic generation on every request, carried to the result | **done** — `MainWindow::requestGeneration_` |
| Superseded results dropped at the boundary, never reaching the viewer | **done** — one check in `loadCurrentFrame` |
| Key results by the requested index, never the landed index | **done** — read off `VideoFrame::frameIndex` |
| Latest-wins target slot, depth 1, not a FIFO queue | already true — `pendingScrubFrame_` |
| One decoder, one owning thread | step 5 |
| Cooperative cancellation inside the GOP walk | step 5 |
| Discarding a stale result costs one refcount decrement | done in step 2 |

`supersedeInFlightRequests()` is the single entry point that invalidates work in
flight. It **bumps on every change of target**, not only when storage is busy,
because "the target moved" is the condition the worker acts on and it has to mean
the same thing whether or not a read happens to be outstanding at that instant.
`ioCancelCount_` was already this counter in all but name; it only counted
remote-I/O cancellations, which is why it was not reusable as-is.

**HUD `gen N drop M`.** `gen` counts target changes and climbs through any drag;
`drop` counts results actually discarded for staleness. They are split because
they answer different questions, and `drop` is the interesting one: 0 on local
media today, non-zero on a slow remote source, and the number that says whether
latest-target-wins is doing any work once decode moves off the UI thread. If
step 5 lands and `drop` stays 0 during a fast drag on heavy media, the worker is
not actually being superseded and something is wrong.

**Fixed in passing:** `target`/`shown`/`delta` went stale on cache hits, because
the hit path returns before the decoder's per-decode perf fields are written — so
a drag running mostly on hits reported whatever the last real decode left behind.
They come off the delivered frame now. Measured on the backward drag at ~92%
hits: previously `target 5 | shown 5` with the playhead on frame 3, now
`target 3 | shown 3`. The values were not wrong so much as old, which is worse in
a line whose whole job is to say which frame is on screen.

**What step 5 must not do**, restating §6 against what now exists: do not open a
second `VideoDecoderFFmpeg` on the same file (the first reverted attempt did, and
`frameFromPts`'s monotonic bump is per-decoder state, so two of them can disagree
about which index a PTS maps to); do not queue requests FIFO; do not key results
by the landed index; and make cancellation checkable *inside* the GOP walk, since
that is the only loop long enough to matter. The `MediaIoSource` rule stands — a
superseded read runs to completion and is reported stale, never abandoned
mid-buffer, because the destination belongs to FFmpeg.

---

## 14. Step 5 — the async scrub worker (GATE D design)

### 14.1 The control measurement, taken first

§7 named `kScrubWalkBudgetMs` and the zero-interval re-arm as the two knobs that
decide how much of the UI thread a synchronous shuttle takes, and said they were
worth an A/B *before* async as a cheap control. That A/B has been run, on the
reversal gesture set, measured with the new `ui gap` instrument (`e343c55`) — a
1ms timer that can only fire when the event loop is running, so the interval
between two firings is exactly how long the thread was unable to deliver a mouse
move or a repaint.

| file | walk budget | re-arm | **worst UI gap** | gaps >16ms | paints |
|---|---|---|---|---|---|
| 4K H.264 | 8ms *(shipped)* | 0ms | **115.6ms** | 3 of 1676 | 442 |
| 4K H.264 | 2ms | 0ms | **112.0ms** | 3 of 1374 | 398 |
| 4K H.264 | 8ms | 12ms | **113.1ms** | 3 of 1887 | 388 |
| 1080p H.264 | 8ms *(shipped)* | 0ms | **86.6ms** | 25 of 442 | 826 |
| 1080p H.264 | 2ms | 0ms | **81.3ms** | 23 of 508 | 672 |

**Quartering the budget moves the worst UI-thread block by 3%.** The reason is
structural and it is the finding that justifies the whole step: the budget is
checked *between* frames, so it bounds the loop but cannot subdivide a single
`decodeFrameAt`. The worst block is one call — a cache miss forcing a seek plus a
GOP walk — and no scheduler policy on the UI thread can reach inside it. The
budget only bounds how many *cheap* frames a slice chains, which is exactly why
1080p (1.7ms/frame, several frames per slice) moves a little and 4K (one
expensive frame per slice) does not move at all.

Two things worth carrying forward from the same table:

- **1080p is the worse case for responsiveness, not 4K.** Average UI gap is
  6.53ms against 1.70ms, and 5.7% of samples exceed 16ms against 0.2%. Cheap
  frames mean the slice runs to its full budget and re-arms immediately, so the
  thread is nearly saturated; expensive frames mean one decode and a return to
  the event loop. This is very likely the owner's "1080p backward is still a lil
  glitchy" on a file whose throughput numbers are excellent.
- **Release latency is 91ms on 4K H.264 and 0.1ms on 1080p.** The landing is a
  seek plus a GOP walk and is deliberately synchronous; it is reported separately
  so it is never confused with the drag.

The async win can now be booked honestly, because the scheduler tweak it might
have been confused with has been measured and does nothing.

### 14.2 Decoder ownership

**Today.** One `VideoDecoderFFmpeg`, owned by the UI thread, called only from it.
`storageBusy_` is a re-entrancy guard against the UI thread re-entering its own
decode when a remote read pumps the event loop. No other thread touches it.

**Proposed: a lease, not a move.** The decoder stays a member of `MainWindow` and
stays on the UI thread by default. For the duration of a drag it is *leased* to
one worker thread, and while leased the UI thread does not touch it at all —
not `decodeFrameAt`, not `perfStats()`, not `ioStats()`, not `currentFrame()`.
Exactly one owner at any instant, and the transition points are all on the UI
thread, all at moments when the UI thread is provably not inside a decoder call.

The alternative — moving the decoder permanently to a worker and having playback
call in synchronously — was rejected: it would put a thread round trip inside the
validated audio-mastered playback tick, and the brief forbids rewriting playback.

`metadata_` is the one thing read while leased. It is written only by `open()`,
which cannot run while a lease is out, so it is immutable for the lease's
lifetime. Everything else the HUD wants is **snapshotted by the worker and
published with the result**; `refreshHud` reads the snapshot. That removes the
whole class of "benign" counter races rather than arguing about them.

### 14.3 Ownership transitions

| event | transition | cost |
|---|---|---|
| **scrub start** — first drag slice, `flushVideoScrub` decides the walk is due | UI→Worker. UI sets the decoder's request mode and direction, sets its stall pump to null, marks the lease held, posts the request. | none |
| **scrub end** — release, or the drag converges and no request is outstanding | Worker→UI via `reclaimDecoder()`: raise cancel, wait for the worker to park at a checkpoint, restore the stall pump, clear the lease. | one cancellation checkpoint |
| **playback start** | `reclaimDecoder()` first, then playback proceeds exactly as today. | same |
| **media switch / close** | `reclaimDecoder()` before `videoDecoder_.close()`. Generation bump means any result already produced is stale and can never be inserted against new media. | same |
| **shutdown** | `reclaimDecoder()`, then stop and join the worker. The worker holds no decoder when parked, so the join is bounded by one checkpoint. | same |

The stall pump is nulled while leased because it pumps *the calling thread's*
event loop and touches widgets. On the worker a remote read simply blocks the
worker, which is the correct behaviour and the entire point. The `MediaIoSource`
rule is unchanged: a read already in flight runs to completion and is reported
stale.

`reclaimDecoder()` is a bounded UI-thread wait, and that is worth stating
plainly rather than hiding: it is not zero. It is bounded by one cancellation
checkpoint — roughly one packet decode — which is *less* than the UI thread
already pays per frame of the synchronous walk today, and it happens on release
and media transitions rather than per frame.

### 14.4 The latest-target slot

Depth 1, replaced rather than queued, as §6 requires:

```
worker state, one mutex:
    std::optional<Request> pending_;    // overwritten by post(), never queued
    Request                active_;      // what the worker is decoding now
    std::atomic<long long> latestGeneration_;   // read by the cancel checkpoint
```

`post(frame, generation)` overwrites `pending_` and stores `latestGeneration_`.
The worker takes `pending_` into `active_` at the top of its loop, so a target
that is superseded before the worker gets to it is simply never decoded.

**A subtlety that would have been a bug.** `requestGeneration_` bumps on every
change of *pointer* target, which during a drag is every slider move. The
worker's target is not the pointer — it is `activeScrubFrame_ + direction`, the
next frame of the shuttle, and that does **not** change when the pointer moves.
Testing staleness against `requestGeneration_` directly would therefore abandon a
perfectly good in-flight walk step on every mouse move.

The fix is not a second counter. Requests are stamped with the value of
`requestGeneration_` *at the moment they are posted*, and the staleness test is
`active_.generation != latestGeneration_` — "has a newer request been posted to
the worker", not "has the pointer moved". The number is still
`requestGeneration_`; `latestGeneration_` only records which of its values was
last handed to the worker.

### 14.5 Generation flow

```
pointer moves            -> queueVideoScrubFrame -> supersedeInFlightRequests()
                            ++requestGeneration_          (unchanged, §13)

walk target changes      -> worker.post(frame, requestGeneration_)
                            latestGeneration_ = requestGeneration_

decode checkpoint        -> active_.generation != latestGeneration_ ? abandon
    (inside the GOP walk)

result published         -> {requestedFrame, generation, VideoFrame, telemetry}

UI delivery boundary     -> generation != worker.latestGeneration() ? drop
                            ++supersededResults_        (the existing `drop`)
```

Release, play, media switch and shutdown all go through `reclaimDecoder()`,
which bumps `requestGeneration_` and pushes the new value into
`latestGeneration_` — so every result already in flight is stale by construction
and **no preview can appear after the exact landing**. That is the invariant, and
it is enforced by the same counter that already exists rather than by ordering.

### 14.6 Safe cancellation points inside the GOP walk

`decodeUntilTarget`'s outer `while (true)`, at the very top, before the
drained check. At that point:

- no `AVPacket` is owned — every path unrefs before looping
- `impl_->frame` is not being written
- the codec is between send/receive cycles, not mid-transition
- no swscale conversion is in progress

It is reached **once per packet**, which on long-GOP H.264 is once per ~0.5–3ms —
the granularity that makes cancellation latency small enough to matter. The inner
receive loop is deliberately *not* instrumented: it normally yields one frame then
EAGAIN and returns to the outer loop anyway, so a check there buys nothing and
adds a point where the codec state is less obviously quiescent.

A cache-fill conversion during a Scrub seek walk can delay the next checkpoint by
one conversion (~1.9ms at 4K preview resolution). Accepted, and measured.

**Abandonment is not failure.** `decodeUntilTarget` returns three states rather
than a bool. `Abandoned` must not take the recovery-seek path — `recov` is a
correctness counter that has to stay 0, and an abandoned walk is not a
mispositioned decoder. Decoder state after an abandon is coherent:
`lastDecodedFrame` reflects what was actually decoded, `currentFrame_` is
untouched because no frame was produced, and the next request re-evaluates
`needSeek` from both.

### 14.7 VideoFrame across the thread boundary

Nothing to add. This is what step 2 bought:

- `shared_ptr<FrameBuffer>` has an atomic refcount, so handing a frame across
  threads is a refcount bump and dropping a stale one is a single decrement.
- The convert pool's free test is `use_count() == 1`. A count of 1 means the pool
  is the only holder, so no other thread has a reference to copy *from*; the
  worst a concurrent decrement by the UI thread can cause is a missed reuse, and
  the pool then allocates or picks another slot.
- The viewer and the frame cache keep their own references, so a buffer the UI is
  still displaying can never be handed back to swscale.
- The pool container itself, and `reverseCache`, are decoder-internal and
  therefore covered by the lease.

The worker decodes into a member `VideoFrame` so the pool sees a steady number of
outstanding references across requests, exactly as `videoFrameBuffer_` does on
the UI side today.

### 14.8 The six prior failure modes

| §6 defect | how this design avoids it |
|---|---|
| 1. second decoder on the same file | One decoder, leased. The worker has no decoder of its own and cannot construct one; it holds a pointer that is only valid while the lease is out. |
| 2. FIFO queue, no generation | Depth-1 slot, overwritten. Every request carries a generation, tested at a checkpoint inside the walk and again at the UI delivery boundary. |
| 3. results keyed by the landed index | The result carries the **requested** frame and the delivered `VideoFrame::frameIndex`. §13 already made `target`/`shown`/`delta` read off the frame; the worker publishes both so a disagreement is visible rather than papered over. |
| 4. stale insert after reset | There is no `ready_` map to insert into. A result is published to a single slot and is dropped at the boundary if its generation is not the latest. `reclaimDecoder()` bumps the generation *before* the worker can park, so anything produced during the wait is already stale. |
| 5. eviction picks the lowest frame index | The cache is untouched by this step. Eviction stays FIFO by bytes (`9513965`, `b5a56af`). |
| 6. `stop()` joins mid-GOP-walk | The join is preceded by cancellation and bounded by one checkpoint. The worker holds no decoder when parked. |

### 14.9 What stays synchronous, and why

Only the **drag shuttle** moves. The press landing, the release landing, frame
stepping, playback and the image-sequence path all keep the exact code they have
today.

That is deliberate on the release landing in particular: making it synchronous is
what makes "no older preview may appear afterwards" trivially true, because the
UI thread reclaims the decoder — which bumps the generation and invalidates every
in-flight result — *before* it decodes the exact frame. The 91ms it costs on 4K
H.264 is a measured, single, wanted decode, not a stall.

**Cache hits now cost a thread round trip.** With the decoder leased, the UI
thread cannot consult the cache without taking the lease back, and a second
UI-side index would be a second source of truth about what is cached. The hit
itself is still the ~0.5ms fast path it was — it is not routed through a seek —
but delivery costs one hop through the event loop. This is the one place the
design pays something, and it is measured rather than assumed.

### 14.10 Validation record — GATE D PASSED (2026-08-08, `f77d472`)

Local Windows toolchain, 1296x812 window. Correctness gates held everywhere:
`delta 0`, `detach 0.00`, `stale-blocked 0`, `recov 0`, no stale or out-of-order
frame presented.

| | result |
|---|---|
| 1080p playback, 10s | 99.1% real time, 240/240 frames, rep 4 skip 0, `clk-upd 1/1`; **worker `posted 0`** |
| 1080p reversals | ui gap 2.22/**45.8**ms, **3 of 1305** over 16ms; rev-hit 93.6%, 23 seeks, 594 paints |
| 1080p fast back | `abandoned 1 stale 1 cancel 2.07ms`; rev-hit 89.3% |
| 4K H.264 reversals | ui gap 1.63/**111.1**ms, **1 of 1751** over 16ms; rev-hit 98.0% |
| 4K H.264 fast fwd | **stalls 0 of 93**; `target 120 shown 120` |
| 4K H.264 fast back | rev-hit 83.8%, ui gap 1.63/93.2ms |
| 4K ProRes 422 HQ reversals | **stalls 0 of 438**, max paint gap **17.0ms**; rev-hit 94.7%; ui gap 1.84/31.3ms |
| 4K ProRes 4444 reversals | `cancel 12.88ms`, `stale 4`, `drop 3`; ui gap 1.64/44.9ms |
| PNG sequence | reversals + step cycles exact; path untouched |
| step ±5, three cycles | landed 61, ends 61 |
| play after release | plays, audio `MASTER`, skip 0; runs to end and Space restarts |
| open another file mid-drag | new media at frame 0, `posted 0 stale 0 drop 0`, no outgoing frame shown |
| quit mid-drag on 4444 | **exited in 53ms** |

Against the synchronous baseline, on the same gesture:

| | sync | async |
|---|---|---|
| 1080p worst UI gap | 86.6ms | **45.8ms** |
| 1080p gaps >16ms | 25 of 442 (5.7%) | **3 of 1305 (0.2%)** |
| 4K worst UI gap | 115.6ms | 111.1ms |
| 4K gaps >16ms | 3 of 1676 | **1 of 1751** |

**The 4K worst-gap figure has not moved and that is not a failure to report
away.** The surviving gap is the *press landing* — a click must jump exactly to
where it was pointed, so that decode is deliberately synchronous and on 4K
H.264 it is a seek plus a GOP walk. The two mid-drag stalls that were there are
gone. It is now the single largest UI-thread block left in a scrub gesture and
is the obvious next thing to attack; see 14.12.

### 14.11 What the mechanisms actually did, and where

Each was exercised rather than assumed, and the media that exercises each one is
worth recording because most gestures on most files exercise none of them:

- **Latest-target-wins at the UI boundary** — `drop 3` on ProRes 4444.
- **Staleness detected before publishing** — `stale 1–4`, several files.
- **`revokeLease()` waiting out an in-flight decode** — 12.88ms on 4444. That is
  one ProRes frame and therefore the floor: every frame is a keyframe, a seek
  lands on the target, and there is no walk to interrupt.
- **The checkpoint inside the GOP walk abandoning one** — 4K H.264, backward
  drag, `TRACE_SEEK_CACHE_WINDOW=0` so misses dominate: **a walk that would have
  run ~70ms given up in 0.04ms**. Also fires unforced on 1080p fast backward
  (`abandoned 1`, `cancel 2.07ms`).

**Cancellation is rarer than the design anticipated, for a good reason.** Only
one request is in flight at a time, and the shuttle's target is the next frame
after the one on screen — so it changes on a direction reversal, not as the
pointer travels. Most drags on most files supersede nothing, and that is the
mechanism working: the in-flight window is one frame wide. `drop` staying near 0
on light media is therefore *not* the alarm §13 expected it to be; the alarm is
`drop` staying 0 on **ProRes 4444**, which lags far enough behind the pointer
for reversals and releases to land inside a decode.

### 14.12 Open after Gate D

1. **The press landing is now the biggest UI-thread block in a drag** —
   90–125ms on 4K H.264, 13–32ms on ProRes. It has to be exact and it has to be
   the frame the user pointed at, so it cannot simply be made approximate; but
   it could be issued to the worker and awaited with the event loop alive, the
   way remote reads already are.
2. **The picture still stalls where the decoder does.** Async moved the block
   off the UI thread; it did not make a cache miss cheaper. 4K H.264 still
   carries paint gaps in the 30–100ms range on a miss. Prefetching ahead of the
   drag direction while the worker is idle is the fix, and it is now cheap to
   build because the worker exists.
3. **`uiblock seek` in the HUD measures the worker, not the UI, while a lease is
   out.** `MediaIoSource` attributes a read with no stall pump as fully blocking
   the caller, and the caller during a drag is the worker — which is the point.
   The `ui gap` line is the authoritative UI-responsiveness number now.

### 14.13 Expected files changed (superseded by §15 for step 5.5)

`src/core/VideoDecoderFFmpeg.{h,cpp}` (cancel predicate, three-state walk result,
abandon counters) · `src/core/ScrubDecodeWorker.{h,cpp}` (new) ·
`src/app/MainWindow.{h,cpp}` (lease, chain, telemetry snapshot, HUD) ·
`app/CMakeLists.txt` · this file · `CLAUDE.md` · `scripts/measure/`.

---

## 15. Step 5.5 — directional scrub-ahead (measured, and mostly answered "no")

Step 5 moved decode off the UI thread. It did not make decode faster, so the
picture could still trail the pointer badly on heavy media. This step asked
whether speculative prefetch closes that gap. **The answer, measured first, is
that the gap has two unrelated causes and prefetch addresses neither.**

### 15.1 The lag model, measured before building anything

0.5s full-clip sweeps, instrumented by `49a6120`. `supply` is what the decoder
supplied over what the hand asked for; `p2p` is how long ago the pointer was
where the picture now is.

| file | dir | ptr f/s | dec f/s | supply | behind end/max | p2p | walk max | seeks | rev-hit |
|---|---|---|---|---|---|---|---|---|---|
| 1080p H.264 | FWD | 254.5 | 273.5 | 107% | 0 / 102 | 310ms | 0f | 1 | 78.6% |
| 1080p H.264 | REV | 249.6 | 148.0 | **59%** | 90 / 159 | 600ms | **29f** | 11 | 91.4% |
| 4K H.264 | FWD | 123.5 | 210.2 | 170% | 0 / 27 | 47ms | 0f | 2 | 69.2% |
| 4K H.264 | REV | 124.4 | 90.0 | **72%** | 32 / 87 | 507ms | **29f** | 10 | 85.4% |
| ProRes 422 HQ | FWD | 176.5 | 143.1 | 81% | 30 / 89 | 478ms | 0f | 2 | 42.9% |
| ProRes 422 HQ | REV | 177.1 | 138.5 | 78% | 29 / 98 | 499ms | 0f | 125 | 2.3% |
| ProRes 4444 | FWD | 272.0 | 52.4 | **19%** | 179 / 201 | 729ms | 0f | 2 | **0.0%** |
| ProRes 4444 | REV | 273.8 | 51.8 | **19%** | 180 / 201 | 738ms | 0f | 46 | **0.0%** |

**ProRes is a pure throughput deficit.** `walk max 0f` everywhere: every frame is
a keyframe, a seek lands on the target, no intermediate frames are ever produced
and there is therefore nothing to cache — which is why `rev-hit` reads 0.0%, and
why that is not a cache-policy failure but a cache with no input. Seeks are free:
4444 forward performs 2 and backward performs 46, at the same 52 f/s. The
179-frame lag is exactly what 19% supply predicts. Prefetch decodes the same
frames earlier, not faster, so it cannot help. The only lever is decoding fewer.

**H.264 is miss cost, not per-frame cost.** Forward ends `behind 0` with `walk
max 0f` and 1–2 seeks — pure sequential decode. Backward drops to 59–72% on the
*same frames*, with `walk max 29f` and 7–8 stalls. 4K H.264 runs 210 f/s forward
and 90 f/s backward; the entire difference is the occasional seek plus GOP walk.

**One reframing.** These sweeps are 2.5×–11× playback, well beyond the ~4× the
owner asked for. At 4× (96 f/s) the same capacities give 1080p 154–284%, 4K
H.264 forward 219%, 422 HQ 144–149%, 4K H.264 backward 94%, **4444 54%**. Only
4444 is genuinely short at the stated target.

### 15.2 What was built

**Velocity-adaptive preview sampling** (`77738f0`, gate corrected in `f08f015`),
for the throughput-deficit half. See CLAUDE.md for the rule, the estimator
choices and the four failed gate inferences. Result: 4444 `p2p 729 → 22ms`, max
lag `201 → 32f`; 422 HQ `p2p 478 → 7ms`.

**A wider seek-walk cache fill**, 60ms → 240ms (`f08f015`), for the miss-cost
half. Step 5 is what made this available: the budget was set when the walk ran
synchronously and a 240ms fill froze the window for 240ms.

### 15.3 Directional prefetch was NOT built, and this is why

The obvious design — spend idle worker time decoding ahead of the drag — has no
idle worker time to spend in the only case that needs it. H.264 backward
measures 59–74% supply, which means the worker is saturated for the whole
gesture; there is no slack. Where slack exists (H.264 forward, 107–170%) the
picture already ends `behind 0` with zero stalls, so there is nothing to buy.

The lever that does exist is not *when* the worker spends its time but *what it
spends it on*: a wider fill per seek converts one expensive miss into a run of
future hits, which is the same benefit prefetch was supposed to deliver, using
time the worker was already using. That is §15.2's second change.

**Do not revisit speculative lookahead without first showing measured idle
worker time coinciding with stalls.** As of this step that combination does not
occur on any file in the test set.

### 15.4 Final matrix

All rows: `delta 0`, `detach 0.00`, `stale-blocked 0`, `recov 0`.

| file | gesture | sampling | p2p | behind end/max | stalls |
|---|---|---|---|---|---|
| ProRes 4444 | fast fwd | ON, stride 5 | **22ms** | 0 / 48f | 1 of 21 |
| ProRes 4444 | reversals | ON, stride 3 | **7ms** | 0 / 48f | 6 of 136 |
| ProRes 422 HQ | reversals | ON, stride 2 | **4ms** | 0 / 20f | 1 of 148 |
| 4K H.264 | reversals | GATED | 6ms | 0 / 32f | 2 of 408 |
| 1080p H.264 | reversals | GATED | 499ms | 22 / 108f | 13 of 555 |

The two gated rows match the Step 5 baseline within noise (4K `rev-hit 98.0%`,
`stalls 2 of 408` against `2 of 432`), which is the requirement: nothing about a
file that must not sample may change. Step ±5 after a sampled 4444 drag returns
to the landed frame exactly, three times over.

### 15.5 Open after step 5.5

1. **1080p backward is now the weakest case** — `supply 60%`, 13 stalls, `p2p
   499ms`. It cannot sample (long-GOP) and gains little from a wider fill,
   because nothing is halved at 1080p so 24 full-res entries exhaust the byte
   budget and it is bytes rather than time that binds. Converting *Step* and
   cache-fill conversions to display size, as scrub previews already are, would
   multiply its effective cache depth. That is the one untried lever.
2. **The press landing** remains the largest UI-thread block in a drag (90–125ms
   on 4K H.264). Untouched here on purpose; step 5.6 if it is worth doing.
3. ~~**Owner validation of the sampled preview is required and has not
   happened.**~~ **SIGNED OFF 2026-08-09, against the full test set.** Every
   figure above said the picture is closer to the pointer. None of them said
   whether a preview that shows every 5th frame *reads* as smooth shuttling or
   as strobing. **It reads as shuttling.** Sampling on intra-only media stays.

   This is the third time the project has recorded the same split, and it held
   again: the harness says the mechanism works, only the owner says the bar
   holds. Note what was being validated is not a throughput number — it is
   whether deliberately breaking the "never skip a frame" rule, in the one place
   it is not in force, is perceptible. It is not.

   **Two defects were assessed and deliberately deferred, not overlooked:**

   - an occasional small hitch on 4K H.264 under extreme-speed scrub;
   - ProRes 4444 pausing briefly and then catching up under extreme
     back-and-forth.

   Both are the known stall profile — a cache miss forcing a seek plus a GOP
   walk — and §15.3 already records that directional prefetch was measured and
   declined as the fix. **Owner instruction: do not continue chasing the rare
   extreme-scrub hitch unless the next architecture work makes it worse.**

   They are therefore **regression tripwires for GATE B, not work items**. After
   the D3D11 surface lands, re-run the 4K H.264 and 4444 extreme gestures and
   confirm neither got worse. A tripwire that is never re-run is just a deferred
   bug, so this is part of the gate, not a note beside it.
4. **Play/pause state did not survive a drag** — the one correctness gap the
   owner found in the same pass. Fixed as step 5.6 (`473b90e`); see §16.

---

## 16. Step 5.6 — play/pause state across a drag (`473b90e`)

The one correctness gap in the step 5/5.5 sign-off pass. Scrubbing did not
*interrupt* playback, it *ended* it: the `sliderPressed` and `valueChanged`
lambdas both paused unconditionally and nothing in `sliderReleased` restored.

### 16.1 Why a snapshot cannot work

The obvious fix — capture `playback_` state when the drag begins — is wrong, and
the reason is an emission order that is easy to assume the other way round.

With `SH_Slider_AbsoluteSetButtons` in force (`9a214f2`), `QSlider` sets the
value from the click position **before** it calls `setSliderDown(true)`. So the
first `valueChanged` of a groove click arrives *before* `sliderPressed`, and by
the time `sliderPressed` could capture anything, the `valueChanged` lambda has
already paused. A capture there records "was paused" for a click that began
during playback. Gating the capture on `isSliderDown()` fails identically, and
for the same reason.

**The design does not depend on that ordering being what it is.** That is the
point of it. The ordering was not verified against the Qt 6.10.2 source (the Src
component is not installed on this box) and does not need to be — carrying an
intent rather than a snapshot makes the question unreachable.

### 16.2 Intent, not mechanism

`userPlayIntent_` means *the user has asked for playback and has not asked for
it to stop*. It is distinct from `playTimer_.isActive()`, which is whether the
mechanism is currently running. A scrub suspends the mechanism and never writes
the intent; `sliderReleased` restores iff the intent is set.

- Set **true** by Play, and by `L` at 1x (which is the same thing). `L` above
  1x sets it false, for the same reason off-speed shuttle is silent: an
  off-speed run is a different gesture and resuming it at 1x is the wrong
  answer.
- Set **false** by pause, explicit stepping (buttons and arrow keys), `J`, `K`,
  opening media, and running out of frames.
- The scrub path never writes it.

This is the shape of `reclaimDecoder()` (§14.2): one property enforced at one
choke point rather than a convention observed at a dozen call sites. It also
answers "what if Play or Pause is pressed while the release frame is still
resolving?" **by construction** — the press flips the intent, the restore reads
the intent, so the latest command wins and there is no race to lose.

**The wheel is the one gesture that needed classifying.** A wheel notch over the
groove enters `valueChanged` with no press and no release, so nothing would ever
restore, and the intent would outlive the gesture and fire on some later drag.
It is a stepping gesture, so an event filter clears the intent and lets the
event through unchanged — wheel-to-step behaves exactly as before. Every other
route into `valueChanged` is either part of a press/drag or a programmatic set
guarded by `suppressSliderSignal_`.

### 16.3 Resume ordering

Resume runs **after** the landing, and two things in that order are load-bearing:

1. `flushVideoScrub(true)` lands the exact, full-res frame. Playback decodes
   synchronously on the UI thread, so the decoder lease must be back first —
   and it is, because the landing goes through `loadCurrentFrame`, which calls
   `reclaimDecoder()`.
2. That same `reclaimDecoder()` **already** bumps the generation and pushes it
   at the worker (§14.5), so no older preview-resolution frame can be painted
   after the landing. No additional supersede call is needed at the release, and
   a bare `supersedeInFlightRequests()` would not have done the job anyway — it
   deliberately does not tell the worker, and `onScrubResult` drops on
   `scrubWorker_.latestGeneration()`.

Resuming any earlier would also start audio at the *preview* position, because
`startAudioForPlayback()` takes its offset from `playback_.state().currentFrame`.

Resume is declined in two cases: `playbackAtEnd_` (releasing on the last frame
must not silently restart the file — Play owns the rewind, `c3335ec`), and a
landing deferred by a storage stall (`pendingScrubFrame_ >= 0`, the LucidLink
path where `flushVideoScrub` re-arms instead of decoding). The second stays
paused rather than playing from a frame the release has not landed yet; that is
the safe half of the trade and the user can press Play once the frame arrives.

### 16.4 `startPlaybackRun()`

`togglePlayPause` inlined ~40 lines resetting the cadence and telemetry counters
(`playbackRateClock_`, `firstPresentNs_`, `schedulerTicks_`,
`audioRepeatedFrames_`, `lastClockUpdateMark_`, …). Resume needs all of it, so
it is extracted and called from both paths. Duplicating it would rot silently,
and a resume that skipped it would make the HUD misreport the resumed run —
which is precisely the state step 6's cadence measurements would start from.

### 16.5 Rational frame rate (`7b924be`)

Carried in the same step because it is independent of the GPU work and is a
correctness issue today. `metadata_.fps` was `av_q2d(fr)` with the `AVRational`
discarded on the spot, so 24000/1001 became a double and the tick interval, the
timecode readout and seek arithmetic all worked from the approximation.
`VideoMetadata` now carries `fpsNum`/`fpsDen` alongside it. Nothing reads the
pair yet — behaviour is unchanged — but a rate that is already rounded cannot be
the reference for late-present or jitter, so this is a prerequisite for GATE E.

`int`/`int`, not `AVRational`: this header is reached from `MainWindow.h` and
must compile with `TRACE_WITH_FFMPEG` undefined — the same rule that keeps
`AVPixelFormat` out of `VideoFrame.h` (§4).

### 16.6 Validation record — PASSED (2026-08-09)

Run on the local Windows toolchain against the `Trace_Testing_Assets` set. All
rows `delta 0`, `detach 0.00`, `stale-blocked 0`, `recov 0`.

**The harness could not exercise this gesture at all**, so `lifecycle.ps1` grew
`-PlayThroughDrag` and its control `-PausedThroughDrag`, plus the `-KeyAtRelease`,
`-WheelFirst` and `-ToEnd` modifiers. They decide by comparing the picture across
a second of wall time rather than by reading the HUD, because "is it still
playing" is a question about motion and the frame counter would need OCR.

**The negative control was taken first**, by rebuilding `MainWindow` at `044b2ea`
and running the same gesture: `PLAY-THROUGH-DRAG: FAIL - picture frozen after
release (0%)`. Against 13.3% on the fixed build. The test detects the bug it was
written for, which is the only thing that makes the passes below mean anything.

| gesture | file | result |
|---|---|---|
| play → drag → release | 1080p H.264 | **PASS** 13.3% moved |
| play → drag → release | 4K H.264 | **PASS** 41.4% |
| play → drag → release | ProRes 4444 | **PASS** 20.1% |
| play → drag → release | PNG sequence | **PASS** 94.9% |
| paused → drag → release (control) | all four + still | **PASS** 0.0% every time |
| play → drag → Space at release | 4K H.264 | **PASS** still — pause won |
| paused → drag → Space at release | 4K H.264 | **PASS** 41.7% — play won |
| play → wheel → drag → release | 1080p | **PASS** still — wheel cleared intent |
| play → drag to END → release | 1080p | **PASS** still, 411/411 |

Read against the numbered list:

1. ✓ Resumed run reads `target 215 | shown 215 | delta 0`, `walk 0f`.
2. ✓ The paused control is 0.0% moved on every file including a still.
3. ✓ Both directions. Note *why* it holds: the landing blocks the UI thread, so
   the keypress is queued behind it and is delivered after the resume — the
   later command wins because it is genuinely later, not because of a race that
   happened to resolve well.
4. ✓ 1080p `clk 8.948s × 23.976 = 214.5 → frame 215`; 4K `clk 2.732s × 24.000 =
   65.6 → frame 66`. Audio restarted at the **released** frame, not the
   pre-scrub one — from frame 0 it would read ~0.2s. `MASTER`, `clk-upd 1/1`,
   `sync -22.3ms` / `-23.5ms`, `rep 0 skip 0`, `late 0`.
5. ✓ The resumed frame reads `dst RGB32/BGRA` with no size suffix on 4K H.264
   and 4444 — full-res. A preview would read `RGB32/BGRA 202x360`. 1080p is not
   evidence here (previews are already full-res below the 1920 threshold); 4K is.
6. ✓ `-StepCycle` 209 → 209 over 3×(Right×5/Left×5). `-PlayAfter` ran.
   `-KillMidDrag` exited in 56ms. `-SwitchMedia` loaded the incoming file at
   frame 0, paused, `posted 0 stale 0`. `-SnapRelease` on 4444 gave **`drop 6`**,
   non-zero as required — `drop 0` there would mean the worker is never being
   superseded.
7. ✓ Released on 411/411: `presented 1 frames | ticks 1 | presents 1`. The
   resumed run started, ticked once, found nothing to advance to and stopped.
   The file did not restart.
8. ✓ PNG sequence 94.9% moved (the non-video branch of `sliderReleased`); still
   image 0.0% and no crash.
9. ✓ A pass here is also proof the filter fired: had the wheel not reached it,
   the intent would have survived and the release would have resumed playback.

**Scrub regression, against the §15.4 baselines**: 4K H.264 reversals
`rev-hit 97.9% (236/241)` vs 98.0%, `stalls 2 of 402` vs 2 of 408, `sample
GATED`, `ra-walk 8.67f/seek`. 4444 snap release `sample ON stride 5`,
`rev-hit 0.0%`, `ra-walk 0.00f/seek`. Unchanged.

<details>
<summary>Original checklist, kept for what each item was for</summary>

Owner's four confirmations plus what this codebase's own history says to check:

1. Playing → drag → release → playback continues **from the released frame**,
   and the first presented frame equals the landing frame (`delta 0`).
2. Paused → drag → release → stays paused.
3. Paused → drag → Play pressed mid-release → plays. Playing → drag → Pause
   pressed mid-release → stays paused. Run on **4K H.264**, where the press
   landing is 90–125ms and the window is wide enough to actually hit.
4. Audio restarts at the released frame, not the pre-scrub one — HUD `sync`, and
   `clk × fps` matching the frame index.
5. No preview frame after the landing: the resumed first frame reads full-res,
   not `previewRes`. `drop` non-zero on a snap release with 4444.
6. `scripts/measure/lifecycle.ps1` in full — it exists for exactly these
   transitions. `scrub.ps1 -SnapRelease` is the only gesture that reliably
   catches a decode in flight, and therefore the only one that exercises
   cancellation at all.
7. Release on the last frame with intent true → stays there, does not restart.
8. Playing → drag on an **image sequence** and on a **still** — the non-video
   branch of `sliderReleased` is a separate code path and takes the same
   restore.
9. Wheel over the groove while playing → steps as before, and a later drag does
   **not** resurrect playback.

</details>

---

## 17. Step 6 — GATE B, the native D3D11 surface (`feat(gpu)`)

The first native presentation path. Scope as §8 item 6: frame, stride, aspect,
resize, fallback. It presents exactly what the CPU backend presents — swscale's
BGRA — so the commit can be judged on "does a native surface show the right
pixels at the right size" alone. Planar YUV upload and shader colour conversion
are GATE C.

Opt-in via `TRACE_RENDERER=d3d11`; `cpu` stays the default until GATE E.

### 17.1 What it is

A DXGI flip-model swapchain (`FLIP_DISCARD`, 2 buffers, `B8G8R8A8_UNORM`), one
`D3D11_USAGE_DYNAMIC` texture uploaded per frame with `WRITE_DISCARD`, and a
fullscreen triangle generated from `SV_VertexID` — no vertex buffer, no input
layout.

**Letterboxing is done with the viewport, not with vertices.** The clear paints
the whole back buffer black and the viewport is set to the fitted rect, so the
bars are free and aspect handling lives in exactly one expression. The shader
never learns that letterboxing exists, which is what keeps GATE C a change of
pixel shader and nothing else.

**Flip model is chosen deliberately, not by default.** It is the mode that
supports waitable swapchains and DXGI frame-latency control — the stated payoff
of picking native D3D11 over QRhi (§5). `DISCARD` would have built the GATE E
dead end in on day one.

**Shaders are compiled by fxc at build time**, emitted as byte arrays via
`/Fh /Vn` and linked into the exe. `d3dcompiler_47.dll` is not deployed by
`windeployqt`, so a runtime `D3DCompile` would configure green and then fail to
create a shader on a clean machine. `vs_4_0`/`ps_4_0`, so feature level 10.0
and WARP stay in scope for CI. If fxc is not found the whole backend is left
out of the build and the app is exactly what it was — a missing tool must not be
a broken build.

**The device falls back to WARP** and says so: the HUD reads `d3d11 (warp)`, so
a machine that quietly went software is never mistaken for one measuring
hardware.

### 17.2 The surface is a child HWND — and the first reason recorded for that was wrong

The renderer creates its own child window and presents into that, rather than
into the host widget's HWND.

**Both approaches were built and both work.** The other one — `WA_PaintOnScreen`
plus a null `paintEngine()`, presenting into the widget's own HWND — was built
first, showed a black video rect, and was replaced on the theory that Qt's
backing store was painting over the region. **That theory was wrong, and the
measurement that produced it was a trap worth recording**: the 1080p validation
clip *opens on a black frame*. Every capture was a correct render of black.

The bisect that found it: clear the back buffer to red instead of black. The red
appeared, with a 202px black strip exactly where the video viewport was — which
proved present, compositing, viewport and letterboxing were all already correct
and moved the search to the texture. The upload log then read
`px0 0 0 0 255`, i.e. the *source* frame was black. Re-run on a clip with a
non-black first frame, the host-HWND approach presents perfectly.

So the choice rests on narrower but real grounds:

- `WA_PaintOnScreen` is documented **X11-only**. It happens to work on Windows;
  nothing says it will keep doing so, and GATE E stacks waitable swapchains on
  whatever this is built on.
- Child-window compositing above the parent's client area, plus
  `WS_CLIPCHILDREN` on the parent to exclude the region from the parent's own
  painting, is documented Win32 behaviour.
- Qt's widget model is left alone entirely. `ViewerWidget` needs no
  `paintEngine()` override, so nothing about the app changes shape when the GPU
  backend is off.

**It also settles hazard 1 of §3 (drag-and-drop) by construction.** The surface
window is not registered as an OLE drop target, so a drop over the video area
falls through to the ancestor that is — the one Qt registered for MainWindow.
Nothing needs forwarding.

### 17.3 Fallback is the host's job, not the factory's

`createRenderer()` can only decline a backend it *knows* cannot run. A GPU
backend fails for reasons that only exist once there is a device and a window,
and only `ViewerWidget` has the widget — so it adopts a renderer, and on failure
adopts `createCpuRenderer()` instead and says loudly which one is presenting.

`adoptRenderer()` applies the widget-level attributes **before** `initialize()`,
because that call is what realises the HWND the backend takes. Splitting it into
create/configure/initialize at the call site would make that ordering a
convention rather than a property — the same shape as `reclaimDecoder()` (§14.2).

### 17.4 Validation record — GATE B PASSED (2026-08-09)

All rows `delta 0`, `detach 0.00`, `stale-blocked 0`, `recov 0`.

| check | result |
|---|---|
| 4K H.264, 4K ProRes 4444, PNG sequence | correct picture, correct aspect, `renderer d3d11` |
| 1080p 9x16 H.264 | correct — including the black frame 0 that caused §17.2 |
| playback, 4K H.264 + audio | **98.3% of real time, 120/120, rep 3 skip 0** |
| playback, same clip on `cpu` (control) | **98.3%, 120/120, rep 2 skip 0** |
| scrub reversals, 4K H.264 | `rev-hit 97.9% (228/233)`, `stalls 2 of 394`, `sample GATED` |
| same gesture on `cpu` (control) | `rev-hit 97.9% (236/241)`, `stalls 2 of 402` |
| resize 900x700 and 1700x900 | swapchain and surface follow, aspect preserved |
| `TRACE_RENDERER=vulkan` | warns, runs `cpu`, picture correct |
| default (no env) | `renderer cpu`, unchanged |

**Playback and scrub match the CPU baseline exactly**, which is the requirement:
GATE B is about a native surface being correct, not about being faster.

Paint cost did drop, and it is recorded here only so a later phase does not book
it twice: 4K H.264 `paint 0.39/0.54 tot 0.54 draw 0.50` on cpu against
`paint 0.01/0.04 tot 0.14 draw 0.00` on d3d11. That is presentation cost, which
was never the bottleneck — the playback rate is identical, which is the honest
summary.

### 17.5 Open after GATE B

1. **The deferred scrub defects are still the tripwires** (§15.5 item 3). The 4K
   H.264 reversal gesture measures unchanged through the D3D11 path, so the
   surface did not make the stall profile worse. The extreme-speed gestures the
   owner reported have not been re-run by hand.
2. **Owner validation has not happened.** Every number above is the harness. The
   split this project has recorded four times now applies here too, and it
   applies with particular force to a *rendering* change: nothing in the table
   says the picture looks right, only that it is the right frame at the right
   size. 4K ProRes 422 HQ remains the bar.
3. **No planar YUV yet** — the frame is still converted by swscale on the CPU
   and uploaded as BGRA, so the upload is 4 bytes per pixel of a frame the GPU
   could have assembled from 1.5. That is GATE C and is where the conversion
   cost actually moves.
4. **Fullscreen is untested** through the native surface. It is a Qt window
   state change with the layout intact (§3), so the surface should simply
   follow its parent, but that is an expectation rather than a measurement.
