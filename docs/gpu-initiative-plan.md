# Trace GPU / smooth-presentation initiative — active plan

**Status: GATE A COMPLETE IN CODE (2026-08-07).** Steps 1–3 of §8 are committed
and validated on the local Windows toolchain: `VideoFrame` has replaced bare
`QImage` at the four seams (`03d840e`), and the `VideoRenderer` boundary exists
with `CpuImageRenderer` as its only implementation (`5765c19`). Step 4's
generation plumbing is in too (`75a3412`, §13). No GPU backend yet. **Next is
step 5 — GATE D**, the async scrub worker, which is where the real risk lives.

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
