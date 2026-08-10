# Trace GPU / smooth-presentation initiative — active plan

**Status: GATE B PASSED, GATE C IMPLEMENTED AND MEASURED (2026-08-09).** Steps
1-7 of §8 are committed and validated on the local Windows toolchain.

**GATE E is pulled ahead of items 8-10 by owner decision (2026-08-09).** Items
8, 9 and 10 are deferred, not cancelled. §23.5 recorded the argument and declined
to act on it unilaterally; the owner has now taken it.

**GATE E is PASSED at step 1, with owner sign-off (2026-08-09) — §24.13, §24.14.**
The integer-tick beat is gone on every file and every renderer: the 1.5-2.5x
bucket goes 5 -> 0 on 4444 with the planar path, long-gap spacing 58/61/62 ->
none, and all three audio-mastered files improved with `rep` falling 4-5 -> 1. A
negative control in the same binary (`TRACE_DEADLINE_SCHED=0`) still shows the
fault. **The owner signed off running the CPU default with no GPU path
involved**, so E1 alone cleared the complaint.

**E2 (the DXGI phase source and the present/decode swap) is NOT built and is
stopped by owner decision** — the design is retained unbuilt at §24.4-24.6. Two
of its premises changed under measurement: `DwmGetCompositionTimingInfo` **fails
on this machine** (§24.4), so a renderer-independent phase source does not exist
and any future E2 is d3d11-only; and the panel is **239.999 Hz**, i.e. exactly
10 refreshes per 24.000fps frame (§24.2).

**GATE B is signed off by the owner (§20.2, §17.5 item 2).** CPU and D3D11 are
visually equivalent in fit-to-window and fullscreen; the 150% case is accepted
with no meaningful softness, scaling artifacts, colour or framing difference, so
§20.3 closes as acceptable rather than as a defect. ProRes 4444 scrub passed
too. Verdict: **proceed with D3D11.** The two measurable blockers were fixed
first -- the HUD unit bug (`58ec879`) and the fractional-DPI rect divergence
(`ddb38ca`), where the downscale ratio turned out to be a confound.

**GATE C is implemented and measured (§22).** Planar YUV upload with the matrix
in the pixel shader, confirmed against the CPU path at 8, 10 and 12 bits (max
channel delta 2-3). Conversion cost falls 2.5-4.1x; presented rate is unchanged,
so the win is headroom rather than throughput. Scrub is unchanged, which is what
§20.7 asked to be verified.

**~~`cpu` remains the default renderer.~~ SUPERSEDED 2026-08-10: `d3d11` IS THE
DEFAULT** (§25). `TRACE_RENDERER=cpu` is now the control and the escape hatch.
Statements elsewhere in this file that `cpu` is the default are dated records of
what was true when the measurement beside them was taken; they are left as
written rather than rewritten, because a decision log that edits its own history
cannot be checked.

The overlay question is **settled and the work is stopped** (§20.1): the child
HWND stays, `WA_PaintOnScreen` is not promoted, and renderer-composited
translucency is proven viable at no measured playback cost (§19). The final
interface is built after GPU integration completes, not before.

**`cpu` remains the default renderer and every experimental path is off unless
its environment variable is set** — confirmed at runtime (§20.6).

~~**Do not start GATE C** until GATE B is signed off.~~ GATE C is done (§22) and
scrubbing was measured separately as §20.7 required: previews stay on swscale and
decoder throughput is identical across three runs each. GATE B's remaining item
is the owner's eye, which no amount of further implementation supplies.

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

`TRACE_RENDERER=cpu` stays the default until Gate E. **Fulfilled 2026-08-10:
Gate E passed and the default is now `d3d11` — see §25.**

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
   *(done, `e8566a4` — see §22)*
8. ~~`perf(gpu): reuse textures and upload resources`~~ — **CLOSED, answered-no
   2026-08-10, see §27.** The reuse is already in the GATE B code (`tex 3` across
   261 frames, `tex 4` across a 406-paint drag) and the residual upload is memcpy
   bandwidth. The telemetry that answered it shipped (`e88d002`).
9. ~~`perf(gpu): add GPU scaling and telemetry`~~ — **DONE 2026-08-10, `f2d6d57`,
   see §28.** A real defect: the sampler took one 2x2 tap at a 6.4x downscale,
   measured 0.74 of the way from a correct area reduction to point sampling, on
   **every** full-resolution frame rather than only the landing §9 named. Fixed
   with a box reduction in the shader: 4444 0.74 → 0.02, 422 HQ 0.89 → 0.00, no
   measurable playback or scrub cost. The telemetry half landed early with §27.
   **Owner visual sign-off PASSED 2026-08-10**, and the drag preview's remaining
   softness is accepted as-is.
10. `feat(gpu): add high-bit-depth ProRes presentation` — **deferred**
11. **GATE E** — pulled ahead of 8-10 (2026-08-09, owner decision) and **split in
    two**, which §24.11 Q1 asked and the result justified:
    - 11a. `perf(playback): schedule presents against the exact source rate`
      — E1, renderer-independent. *(done, `e2b8655` — **GATE E PASSED**, owner
      sign-off on the CPU default; see §24.13, §24.14)*
    - 11b. `perf(gpu): add DXGI presentation timing` — E2, the vblank phase
      source and the present/decode swap. **NOT BUILT, stopped by owner
      decision.** Design retained unbuilt at §24.4-24.6.

**On the reorder.** Locked real-time playback is priority #1; §23 measured the
residual stutter as the integer-tick beat, which is universal and which only
item 11 fixes; and §23.4 measured headroom — which is all items 8-10 buy — as no
longer the binding constraint on 4444 once the planar path is on. There is no
technical dependency from 8, 9 or 10 into 11: the flip-model swapchain item 11
needs landed at GATE B. §22.7 items 2-5 stand unchanged.

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

`TRACE_RENDERER` defaulted to `cpu` when this was written; **it defaults to
`d3d11` as of 2026-08-10 (§25)**. An unknown value warns on stderr and falls
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
2. ~~**Owner validation has not happened.**~~ **GATE B PASSED — owner visual
   sign-off, 2026-08-09.** CPU and D3D11 are visually equivalent in
   fit-to-window and in fullscreen. **The 150% result is accepted**: no
   meaningful softness, no scaling artifacts, no colour difference, no framing
   difference — so §20.3, which was the one thing that needed an eye rather than
   a number, is closed as acceptable rather than as a defect. **ProRes 4444
   scrub also passed.** Verdict: proceed with D3D11.

   This is the fifth time the project has recorded the same split, and it held
   again: the harness said the right frame was at the right size, and only the
   owner could say the picture looks right. `cpu` remains the default renderer
   until GATE E — sign-off is on the *rendering*, not on changing the default.
3. **No planar YUV yet** — the frame is still converted by swscale on the CPU
   and uploaded as BGRA, so the upload is 4 bytes per pixel of a frame the GPU
   could have assembled from 1.5. That is GATE C and is where the conversion
   cost actually moves.
4. **Fullscreen is untested** through the native surface. It is a Qt window
   state change with the layout intact (§3), so the surface should simply
   follow its parent, but that is an expectation rather than a measurement.

---

## 18. GATE B validation round two (2026-08-09) — visual, surface, overlay, scheduling

Requested after §17: the automated numbers established that the right frame
reaches the surface, not that the picture is right or that the surface can host
an interface. This section is that second pass.

### 18.1 The black-first-frame hazard is now designed out of the tools

`scripts/measure/visual.ps1` replaces ad-hoc captures for anything visual. It
steps to a **named** frame rather than capturing wherever the app opened,
measures luma spread and non-black fraction inside the video band only (the HUD
below it would otherwise supply the "content" on its own), and classifies:

- uniform **red** → FAIL, "the swapchain cleared and never drew";
- uniform **dark** → **INDETERMINATE**, never a pass and never a fail, with the
  message saying to check the clip's frame before concluding anything;
- otherwise → PASS with the measured spread.

`ab.ps1` drives both renderers to the same frame; `abdiff.ps1` pixel-diffs the
pair. The red clear is retained as `TRACE_D3D11_CLEAR_DIAG=1`.

**`abdiff.ps1` insets 8px from the window edge**, and that is not cosmetic: the
first run reported 0.46–0.87% of pixels differing on every clip, and the whole
difference was a 4px strip at x 1290–1293 of a 1296px capture — the resize
border, where the desktop behind shows through and two captures taken moments
apart legitimately differ.

### 18.2 CPU/GPU visual A/B

Same build, same clip, same frame, `TRACE_RENDERER` the only variable.

| clip | frame | cpu | d3d11 | pixel diff (video band) |
|---|---|---|---|---|
| 1080p H.264 9x16 vertical | 60 | PASS spread 245 | PASS spread 245 | 31 px (0.064%), max Δ **6** |
| 4K H.264 | 40 | PASS spread 255 | PASS spread 255 | 0 px, max Δ **1** |
| 4K ProRes 422 HQ | 40 | PASS spread 228 | PASS spread 228 | 0 px, max Δ **1** |
| 4K ProRes 4444 | 40 | PASS spread 254 | PASS spread 255 | 0 px, max Δ **2** |
| PNG sequence | 30 | PASS spread 248 | PASS spread 249 | — |

At the shipping DPI the two backends are **the same picture**. Frame identity,
orientation, aspect, letterbox geometry and black level all match by
construction, because the pixels match.

**At 1.5x DPI they do not.** `QT_SCALE_FACTOR=1.5` (a per-process knob — no
system setting was changed) gives 4209 differing pixels of 108k sampled
(**3.9%**, max Δ **75**), and the bounding box is x 488–1445, y 94–580, i.e.
**inside the video**, spanning the whole rect. The video is at the same position
and the same size in both. This is a scaling-quality difference between Qt's
raster bilinear and the D3D11 sampler under a 4x downscale, not a geometry or
identity fault — but it is a real visible difference and it needs an eye, not a
number.

It also exposed a **HUD units inconsistency**: at 1.5x the CPU backend reports
`display 640x360` (logical) and D3D11 reports `display 960x540` (device pixels)
for the same on-screen rectangle. D3D11's is the honest figure. They agree at
dpr 1, which is why this never showed before.

### 18.3 Window and surface lifecycle — PASSED

`scripts/measure/surface.ps1`, 4K H.264 at frame 40, every step checked for
"went black" rather than left to be noticed:

initial, small (700x560), large (1800x950), **eight rapid resizes**, maximize,
restore, minimize+restore, **fullscreen**, exit fullscreen — all report content,
none black. Child `TraceD3D11Surface` window census after all of it: **exactly
1** (0 on the cpu control, as it must be). Window geometry identical before and
after fullscreen (1430x854). Shutdown clean in **105 ms** (cpu control 57 ms).

Fullscreen specifically: swapchain resized to the 2560x1440 client area, video
correctly positioned, `display 1751x985` — exactly 16:9 — `delta 0`,
`renderer d3d11`.

**Not testable on this machine:** multi-monitor and mixed-monitor DPI. The box
has a single display (`\.\DISPLAY1 2560x1440`, primary). High-DPI was exercised
via `QT_SCALE_FACTOR` only, which scales Qt but not the physical panel.

### 18.4 The overlay spike — the child HWND CANNOT host ordinary Qt widgets

This is the finding that matters most, and it is a design blocker rather than a
defect. `src/ui/OverlaySpike.*`, `TRACE_OVERLAY_SPIKE=1|2|3`, places a centred
label, a clickable button and a translucent panel over the video rect and logs
every hover, press, click and wheel it receives.

| surface | overlay widgets | visible | mouse input | translucent |
|---|---|---|---|---|
| cpu (no native surface) | plain Qt children | **yes** | **yes** | **yes** |
| child HWND (shipped) | plain Qt children | **NO** | **NO** | — |
| child HWND | native siblings (`WA_NativeWindow`) | yes | yes | **NO** |
| child HWND | native + `WA_TranslucentBackground` | yes | (n/t) | **NO**, and worse |
| host HWND (`WA_PaintOnScreen`) | plain Qt children | **yes** | **yes** | **NO** |

On the shipped child-HWND design, plain Qt children are not merely hidden — they
are **not hit-testable**. `WindowFromPoint` over the button returns
`TraceD3D11Surface`, and the app logs no hover and no press. The same build on
`cpu` logs hover-enter, press and click. Making each control native
(`WA_NativeWindow`) restores both visibility and full input, and the hit-test
then resolves to the Qt window.

**Translucency is lost on every native-surface variant**, including host-HWND.
Neither design puts the video pixels anywhere Qt can blend against, so the
translucent panel composites to opaque. `WA_TranslucentBackground` made it worse
(the label lost its styled background too). This is a shared constraint and
therefore does **not** discriminate between the surface designs — it has to be
solved separately whichever is kept.

### 18.5 Source-rate scheduling audit

1. **Stored**: `VideoMetadata::fpsNum`/`fpsDen` (`VideoDecoderFFmpeg.h`), set
   from `av_guess_frame_rate` at `VideoDecoderFFmpeg.cpp:825-827`. The decoder
   also keeps `impl_->fpsQ` for PTS→index arithmetic.
2. **Enters scheduling**: **nowhere.** Nothing reads the pair. Every consumer
   goes through `FrameSource::fps()`, which returns `double`.
3. **Rounded to integer**: yes, once —
   `schedulerIntervalMs_ = floor(1000.0/fps)` (`MainWindow.cpp:1008`), the
   QTimer interval. 23.976 → 41 ms.
4. **Millisecond timer**: yes. `playTimer_.setInterval(schedulerIntervalMs_)`.
5. **Fractional error**: **not accumulated.** `frameDurationMs` is a double, the
   accumulator is fed `nsecsElapsed()` and carries its residue forward (capped
   at 4 frames). The tick is a *bound*, not the rate. Residual error is the
   double's representation of 24000/1001 — ~1e-16 relative, microseconds per
   hour.
6. **Owns the clock**: two mutually exclusive owners. With audio at 1x forward
   and `clockReady()`, the disciplined audio clock is the **only** scheduler and
   decides both when and which frame. Otherwise `playbackClock_` +
   `playbackAccumulatorMs_`.
7. **Present**: `Present(0, 0)` — sync interval **0**. Not vsync-throttled, not
   phase-aligned. Presents are driven by the playback timer via Qt's paintEvent;
   DWM composites at refresh, so the display shows at most one per refresh, but
   nothing in Trace is aware of refresh phase.
8. **Which gate**: **GATE E** (§8 item 11, "add DXGI presentation timing").

**So `7b924be` is foundation only.** It must not be described as frame-rate
lock: no scheduling path consumes the rational today, and the rate error it
removes was already ~1e-16. What it buys is a *reference* — cadence jitter
cannot honestly be measured against a rate that is itself rounded.

---

## 19. Renderer-composited overlay spike (2026-08-09)

**Disposable.** `src/render/OverlayCompositor.*`, `src/render/shaders/OverlayQuad.*`,
`OverlayHooks.h`, enabled by `TRACE_OVERLAY_COMPOSITED=1` on the d3d11 path only.
Placeholder geometry; delete or promote once the approach is accepted.

Built because §18.4 closed the Qt-widget route: plain children over the child
HWND are neither visible nor hit-testable, and every native-window variant loses
translucency. The surface design stays as shipped — supported Win32 behaviour,
already through lifecycle and fullscreen testing — and the overlay moves into
the render pass instead.

### 19.1 What was drawn, and it all works

Translucent rounded panel, Play/Pause, Rewind, Fast-forward, timeline line and
handle, and a rate indicator. Alpha-blended after the video, in the same pass.
**Translucency is real** — the video reads through the panel, which is the thing
no native-window arrangement could do.

### 19.2 The caching design, which is the actual content of the spike

Only **three** things re-rasterise the atlas: surface size, DPI, theme. Nothing
else, ever.

| change | cost |
|---|---|
| play → pause | different **source rect**, constant-buffer write |
| timeline handle moves | different **destination rect**, constant-buffer write |
| hover / press | tint multiplier, constant-buffer write |
| fade 0 → 1 | tint alpha, constant-buffer write |
| rate text "PAUSED" → "1x" | its **own small texture**, so the panel and icons are untouched |

Every D3D resource — shaders, blend state, sampler, rasteriser, constant buffer,
both textures — is created once. Nothing is allocated in the draw path, no frame
is read back to the CPU, and `QWidget::render()` is never called.

The atlas is `ARGB32_Premultiplied` with `ONE / INV_SRC_ALPHA`, which is what
makes a single scalar multiply a correct fade.

**One coordinate space**: device pixels of the surface window. Atlas raster,
destination rects, hit regions and incoming `WM_MOUSE*` coordinates are all in
it, so there is no conversion to get wrong.

### 19.3 Cost — measured, not asserted

4K H.264, overlay held visible for a full 9s playback run by jiggling the
pointer, against the identical gesture with the overlay off:

| | presented | frames | rep/skip | paints |
|---|---|---|---|---|
| overlay **off** | 98.3% | 120/120 | — | 120/121 |
| overlay **on** | **98.3%** | **120/120** | 3 / 0 | 153/121 |

Identical playback. The only difference is paint count, from repaints requested
by pointer moves and fade ticks — and it moved the presented rate by nothing. A
per-frame rasterise or upload could not hide inside that.

### 19.4 Input — through the surface's own window procedure

`WM_MOUSEMOVE`, `WM_LBUTTONDOWN/UP`, `WM_MOUSELEAVE` (via `TrackMouseEvent`),
with `SetCapture` on press so a drag survives leaving the panel.

**`WM_MOUSEACTIVATE` returns `MA_NOACTIVATE`.** The surface never takes
activation, so keyboard stays with the Qt window by construction rather than
being restored afterwards — stepping, J-K-L, Escape and the fullscreen shortcut
are unaffected. Measured: after hovering, pressing, clicking three controls and
dragging the timeline, `GetForegroundWindow()` is still the Qt main window and a
subsequent Right-arrow steps the frame.

Verified states: hidden → reveal on pointer motion → hover → pressed → click →
drag → leave → auto-hide. Hover and press are measurable and *local*: the play
icon reads 122 (idle) / 127 (hover) / 106 (pressed) while the panel corner is
**96.4 in all three**, which is the per-quad tint working.

### 19.5 Commands stay in the application layer

The renderer owns geometry and hit regions. It owns **no playback state**.

- Play/Pause → `playPauseAction_->trigger()`, the same QAction the transport
  bar, the menu and the spacebar use.
- Rewind / Fast-forward → `prevFrameAction_` / `nextFrameAction_`.
- Timeline drag → drives the **real QSlider's** `setSliderDown(true)` /
  `setValue()` / `setSliderDown(false)`.

That last one is why the drag is worth looking at: the HUD during an overlay
drag reads `worker LEASED`, `posted 104`, `dst RGB32/BGRA 640x360`,
`shuttle 1.53ms/f lag 0f`. The overlay inherited the entire async scrub path —
drag shuttle, preview resolution, exact landing, and the step 5.6 play-state
restore — without reimplementing any of it. The main transport slider tracks it
frame for frame, because both read one state.

### 19.6 Recompute on DPI, resize and fullscreen — verified

`QT_SCALE_FACTOR=1.5`: panel, icons and text re-rasterise at 1.5x and are
**crisp, not upscaled**. Resize to 1750x980 and fullscreen at 2560x1440 both
re-lay-out and re-centre correctly; `display 1751x985` stays exactly 16:9, so the
video viewport and aspect maths are untouched by the overlay.

### 19.7 Accessibility and tooltips — the plan, not the implementation

Renderer-drawn controls have **no accessible object**, and nothing above changes
that. This is the one place where the approach is genuinely weaker than Qt
widgets, and it must not be waved through.

The credible path is to keep a **parallel tree of input-transparent
`QAccessibleWidget`s** — one zero-painting Qt widget per control, positioned on
the same rects the compositor lays out, with `Qt::WA_TransparentForMouseEvents`
so the surface keeps the hit-test:

- **names** — `setAccessibleName()` / `setAccessibleDescription()` per proxy;
- **shortcuts** — already on the QActions the hooks call, so
  `QAccessible::Action` can be reported from the action, not invented;
- **focus order** — the proxies are real widgets in a real tab chain, so
  `setTabOrder()` works, and the compositor draws a focus ring from
  `QWidget::hasFocus()` (another tint, no atlas change);
- **tooltips** — `setToolTip()` on the proxy will not show, because the proxy
  never receives mouse events; the compositor must draw the tooltip itself from
  `hover_` plus a dwell timer, and the *text* comes from the proxy so there is
  one source;
- **checked/disabled** — read from the QAction (`isChecked()`, `isEnabled()`)
  and expressed as a tint, exactly as hover and press already are.

**Unresolved**: screen readers announce from the accessibility tree, which the
proxies supply, but a magnifier or high-contrast mode will not affect
renderer-drawn art. High-contrast in particular would need the atlas rebuilt on
`QEvent::ThemeChange` — cheap, since theme is already one of the three
invalidation triggers, but it has not been implemented or tested.

**Do not call the renderer-composited approach final until a proxy-tree
prototype has been driven by an actual screen reader.**

### 19.8 Open

1. Owner visual judgement of the overlay — placeholder art, so this is about
   the *mechanism* reading as viable, not about the design.
2. The accessibility proxy tree is a plan, not a prototype (§19.7).
3. The fade test list in the request was truncated mid-item; auto-hide, reveal
   on motion, hidden/visible/hover/pressed and the 150-180ms fade are all
   covered, and `kFadeMs` is 165.
4. GATE B still not signed off, and GATE C still not started.

---

## 20. Session close, 2026-08-09 — GATE B status and what carries forward

### 20.1 The overlay question is ANSWERED and the work is STOPPED

Architectural conclusion, settled:

- **Keep the child HWND.** Supported Win32 behaviour, already through lifecycle
  and fullscreen testing (§18.3).
- **`WA_PaintOnScreen` is NOT promoted.** It works on this Qt/Windows build
  (§17.2) and that remains useful evidence, but Qt documents it as X11-only and
  that is not a stable contract for the long-term viewer architecture.
- **Renderer-composited translucency is viable** — proven in §19, with real
  alpha over the video, full native input, and keyboard staying with Qt by
  construction.
- **No measured playback cost**: 98.3% and 120/120 frames with the overlay held
  visible for a whole 9s 4K run, identical to the same gesture with it off.
- **Final interface implementation happens after GPU integration is complete.**

**Development on the overlay is stopped here.** No further work on artwork,
transport controls, fades, tooltips, accessibility proxies, high-contrast
assets, menus or shuttle behaviour. What exists is a disposable spike, disabled
by default, and it announces itself on stderr when switched on.

The accessibility plan in §19.7 is a *plan* and is preserved for the interface
phase. It has not been prototyped and must not be treated as a commitment.

### 20.2 GATE B — PASSED (owner sign-off, 2026-08-09)

Both blocking items are closed.

1. ~~**Human visual review.**~~ **PASSED.** CPU and D3D11 are visually
   equivalent in fit-to-window and in fullscreen, and the 150% case is accepted
   with no meaningful softness, scaling artifacts, colour difference or framing
   difference. ProRes 4444 scrub passed as well. Verdict: **proceed with
   D3D11.** See §17.5 item 2.
2. ~~**The HUD logical/device-pixel correction.**~~ **FIXED** (`58ec879`), and
   the divergence behind it explained and mostly fixed (`ddb38ca`) — see §21.1
   and §21.2. `RenderStats::lastDrawSize` is device pixels on both backends and
   both fit the video rect with one shared expression.

Everything else GATE B asked for had already passed: non-black frames render,
CPU/GPU frame identity matches, aspect is correct, resize and fullscreen are
reliable, performance is at the CPU baseline, there is no CPU-path regression,
and shutdown is clean.

**`cpu` remains the default renderer until GATE E.** The sign-off is on the
rendering being correct, not on changing which backend ships enabled.

Everything else GATE B asked for has passed: non-black frames render, CPU/GPU
frame identity matches, aspect is correct, resize and fullscreen are reliable,
performance is at the CPU baseline, there is no CPU-path regression, and
shutdown is clean.

### 20.3 The 150% scaling difference — EXPLAINED (§21.2) and ACCEPTED by the owner

At `QT_SCALE_FACTOR=1.5`, CPU and D3D11 differ on **3.9% of the sampled video
band, max channel delta 75**, bounding box entirely inside the video. Same
position, same size, same frame. At dpr 1 the two are effectively identical
(max delta 1-2).

It is a difference between Qt's raster bilinear and the D3D11 sampler under a 4x
downscale. It is **not** a geometry, identity or colour-conversion fault. It
needs an eye rather than a number, and it belongs in the GATE B visual A/B.

Worth flagging as genuinely unexplained: the divergence is *larger* at 1.5x (4x
downscale) than at dpr 1 (6x downscale), which is the opposite of the expected
direction. Do not accept a hand-wave about filter quality without re-measuring.

### 20.4 Real mixed-monitor DPI is UNTESTED

The test box has a single display (`\.\DISPLAY1`, 2560x1440, primary). High-DPI
was exercised only via `QT_SCALE_FACTOR`, which scales Qt without changing the
panel. **Untested, and none of it is implied by the passes above:**

- moving the window between monitors;
- monitors at different DPI;
- the per-monitor DPI-change message arriving mid-session;
- fullscreen on a secondary monitor.

The code paths exist (`devicePixelRatioF()` is read every paint, the surface is
resized from it, the overlay atlas invalidates on DPI change) but have never run
against a real transition.

### 20.5 Source-rate audit — preserved verbatim

1. **Exact rational metadata IS stored**: `VideoMetadata::fpsNum`/`fpsDen`,
   set from `av_guess_frame_rate` (`VideoDecoderFFmpeg.cpp:825-827`).
2. **Presentation is NOT frame-rate locked.** Nothing reads the pair. Every
   consumer goes through `FrameSource::fps()`, a `double`. The tick interval is
   `floor(1000.0/fps)` — an integer millisecond QTimer, 41ms for 23.976.
3. **`Present(0, 0)` is NOT display-synchronized.** Sync interval 0: not
   vsync-throttled, not phase-aligned. Presents are driven by the playback timer
   through Qt's paintEvent; DWM composites at refresh, so at most one present is
   seen per refresh, but nothing in Trace is aware of refresh phase.
4. **Cadence and refresh synchronization remain GATE E** (§8 item 11).

Note the accumulator does **not** drift: `frameDurationMs` is a double, the
accumulator is fed `nsecsElapsed()` and carries its residue forward, so the tick
bounds the rate rather than setting it. The rational's value is as an *unrounded
reference* for measuring cadence at GATE E, not as a rate correction.

### 20.6 Defaults, confirmed at runtime

Launched with no environment variables at all:

- `renderer cpu` in the HUD — **CPU remains the default renderer**;
- no overlay drawn — **the composited overlay is disabled by default**;
- stderr carries only Qt's own FFmpeg version notice — **no Trace diagnostic
  runs unless explicitly enabled**.

Every experimental gate is off unless its variable is set:
`TRACE_RENDERER` (default cpu), `TRACE_OVERLAY_COMPOSITED`,
`TRACE_OVERLAY_SPIKE`, `TRACE_D3D11_CLEAR_DIAG`, `TRACE_D3D11_HOSTHWND`.

### 20.7 Next session — GATE B visual A/B, then GATE C

Rendering validation, not interface development. Same machine, same monitor:
**4K ProRes 422 HQ**, CPU versus D3D11, at 1:1, fit-to-window, 150% scaling and
fullscreen, judging colour, black and white levels, text, diagonals and fine
detail.

If the picture is acceptable: fix the HUD units (§20.2), sign off GATE B, then
start GATE C — planar YUV upload and GPU colour conversion.

**Measure scrubbing separately in GATE C.** Full-resolution planar upload could
silently make interactive previews worse: the scrub path currently converts to
*display size* in swscale (`b5a56af`), and a GPU path that uploads full-res
planes instead would move that cost rather than remove it. Playback throughput
would not show it; the scrub harness would.

---

## 21. GATE B close-out (2026-08-09, second session)

The two items §20.2 left open are done, and §20.3 is answered. **Human visual
review is the only thing still outstanding**, and it is the owner's to give.

### 21.1 HUD units — fixed (`58ec879`)

`RenderStats::lastDrawSize` is **device pixels** on both backends, and the unit
is now stated on the field. The CPU backend draws in logical coordinates but
QPainter carries the dpr transform, so the rectangle actually sampled into is
`fitted * dpr`; that is what it reports. Verified at `QT_SCALE_FACTOR=1.5` on
4K H.264: both backends read `display 960x540 filtered`, against 640x360 vs
960x540 before.

The resample *test* moved to the device size with it, which is a behaviour
change rather than a reporting one: at dpr 1.5 a frame fitted to a logical rect
of its own size is being upscaled by half again, and the logical comparison
called that `1:1` and switched filtering off for it.

### 21.2 §20.3 answered — the downscale ratio was a confound (`ddb38ca`)

The recorded puzzle was that CPU and D3D11 differ at 1.5x (a 4x downscale) and
not at dpr 1 (6x), which is backwards for a filter-quality story. **The ratio
was never the variable.** The window opens at a fixed *logical* size, so raising
the scale factor also makes the video band physically bigger — DPI and ratio
moved together.

Sweeping the window at dpr 1 across ratios 5.6x → 2.28x, **4.33x included**, the
two backends are identical (0% differing, max channel delta 1–3). Holding one
window and varying only the scale factor:

| scale factor | differing | max Δ |
|---|---|---|
| 1.00 | 0% | 2 |
| 1.25 | 8.9% | 146 |
| 1.50 | 5.8% | 75 |
| 2.00 | 0% | 2 |

Integer ratios agree exactly, fractional ones do not. **The same-renderer
control reads exactly 0 at every scale factor**, so this is a backend difference
and not capture noise — that control did not exist before and no A/B number
here meant anything without it.

Cause: duplicated arithmetic. The CPU path fitted in logical pixels and let the
dpr transform land the rect where it may; D3D11 fitted in device pixels. At a
fractional ratio the two rectangles end up a fraction of a pixel apart. A
parabola fit to the whole-pixel shift search puts the offset at about
**(−0.25, +0.5) device pixels** — sub-pixel, which is why a whole-pixel search
called the pictures aligned and nearly closed the investigation early.

`hostDeviceSize()` and `fitDeviceRect()` are now the one expression both
backends use, truncation included. **dpr 1.25: 8.9% → 2.6%, max 146 → 50.**
dpr 1.5 is unchanged and its output is byte-identical across builds, so the
rects already coincided there and the residual is a genuine filter difference
between Qt's raster bilinear and the D3D11 sampler under a fractional-scale
transform. That is the part that needs an eye.

**This box runs at 100% scaling, where all of it is zero.**

### 21.3 Visual A/B set — produced, awaiting the owner

`scripts/measure/gateb_visual.ps1`, 4K ProRes 422 HQ (the bar) at frame 40,
matched native-resolution pairs plus stacked 5x crops, in
`Desktop\Trace_GateB_Visual`:

| condition | pixel diff |
|---|---|
| fit-to-window (1400x1000) | **identical**, max Δ 2 |
| maximized | **identical**, max Δ 2 |
| fullscreen (2560x1440 client) | **identical**, max Δ 2 |
| 150% scaling | 8.3% differing, max Δ 51 |

Three of the four conditions are the same picture. Only fractional DPI differs,
per §21.2, and on inspection the crops show equal colour, black level and edge
sharpness — the difference reads as a sub-pixel phase shift, not a quality
regression. **None of that is the sign-off**; 4K ProRes 422 HQ against the CPU
path, judged by eye, still is.

**True 1:1 is not reachable for 4K sources on this panel** and was not tested as
such: 3840 wide needs a video rect the 5120x1440 display cannot give once chrome
is accounted for. Maximized and fullscreen are the closest conditions available.

*The fullscreen row nearly passed on a windowed capture.* The harness sent F11;
the shortcut is **Ctrl+Return** (`MainWindow.cpp:593`), so nothing happened and
the capture was of the 1400x1000 window, reported as a fullscreen pass. It is
asserted now — the script fails if the window did not actually grow. Same shape
as the black-first-frame trap in §18.1: a check that cannot fail is not a check.

### 21.4 Deferred scrub tripwires — re-run, neither got worse

§15.5 item 3 required these to be re-run after the surface landed, on the
grounds that a tripwire never re-run is just a deferred bug.

| gesture | cpu | d3d11 |
|---|---|---|
| 4K H.264 reversals | stalls 46/44/44, rev-hit 98.3% | stalls 67/44/46, rev-hit 98.1% |
| 4K H.264 playback | 98.3 / 97.5 / 98.3% | 97.4 / 98.3 / 97.4% |
| ProRes 4444 reversals | stalls 92 of 111, rev-hit 21.1% | stalls 93 of 104, rev-hit 18.9% |
| ProRes 4444 snap release | stalls 62 of 62, `target 261 shown 261` | stalls 57 of 57, `target 261 shown 261` |

All rows `delta 0`, `detach 0.00`, `stale-blocked 0`, `recov 0`. The d3d11
`67 / ui-over-16ms 21` is a first-run-of-session warm-up outlier; the two
repeats read 44 and 46 against CPU's 44.

**Two things to carry, neither caused by this session's changes:**

1. **The absolute stall count is ~20x the figure recorded in §17.4** (44 of 351
   against "2 of 394") on the *same* file and gesture — and it is on **both**
   renderers. A control build of the previous commit reads `stalls 44 of 351`,
   `ui over 16ms 1 of 988`, so it predates the device-grid change and is not a
   regression from it. It is either machine state (the panel now enumerates as
   5120x1440 @ 239Hz, and Parsec virtual display adapters are installed) or
   something between `8a7cdb3` and here. **Unexplained, and worth an hour before
   GATE E**, because stalls are the metric the scrub complaints live in.
2. Run any renderer comparison **twice**. The first run of a session on d3d11
   carries a warm-up cost large enough to look like a regression.

---

## 22. GATE C — planar YUV upload and shader conversion (`e8566a4`)

Step 7 of §8. Full-resolution frames reach the D3D11 backend as three planes and
are converted in the pixel shader. **`cpu` is still the default**;
`TRACE_PLANAR_UPLOAD=0` restores the BGRA path on d3d11 for an A/B.

### 22.1 One shader, not a family

Subsampling is carried by the **size of two textures** and resolved by the
sampler, so 4:2:0, 4:2:2 and 4:4:4 differ in nothing else and the shader never
asks. Bit depth, range and the 3x3 arrive as constants, so 8/10/12-bit and
BT.601/709/2020 are not compiled variants (§11 asked for exactly this).

`R8_UNORM` at 8 bits, `R16_UNORM` above, LSB-aligned as FFmpeg gives them — so
the CPU side is a memcpy and the depth becomes a shader constant rather than a
repacking pass.

**The range terms are computed at the actual depth**, not by reusing 16/255 and
128/255. At 10-bit black is code 64 of 1023 — 0.062561 against 0.062745 — and
the difference is a lift of the black point across the whole picture, which is
precisely the global level shift §11 warns about.

**Matrices Trace has no exact coefficients for are declined**, and the decoder
declines the same set it does. Fcc and Smpte240m keep taking swscale, which
applies them properly. An approximation here would be a colour difference
between the two backends that no A/B could attribute to anything.

### 22.2 The scale factors are confirmed, at three depths

§11 said to confirm empirically rather than inherit. Against the CPU path, same
frame, same window:

| clip | source | differing | max Δ |
|---|---|---|---|
| 4K H.264 | yuv420p **8-bit** | 0.006% | 3 |
| ProRes 422 HQ | yuv422p **10-bit** | 0.002% | 3 |
| ProRes 4444 | yuv444p **12-bit** | 0% | 2 |
| 1080p H.264 | yuv420p 8-bit | 0% | 2 |

Three depths confirmed **independently**, which is more than a single test
pattern would have given: a wrong 65535/1023 would leave 10-bit shifted while
8-bit stayed correct, and that is visible here as three separate passes.

### 22.3 Playback — measured against the BGRA control, not against cpu

Comparing with `cpu` would re-book the presentation win §17.4 already recorded
and explicitly says not to count twice.

| clip | sws → copy | handler | first frame |
|---|---|---|---|
| ProRes 422 HQ | 14.58 → **3.52ms** | 20.89 → **10.50ms** | 38 → 14ms |
| ProRes 4444 | 16.97 → **5.60ms** | 35.20 → **25.22ms** | 60 → 35ms |
| 4K H.264 | 3.07 → **1.25ms** | 5.29 → **2.30ms** | 94 → 58ms |

**Presented rate is unchanged everywhere (98.3–99.6%), and that is the honest
summary**: none of these files was conversion-bound at 24fps. What the change
buys is *headroom* — 4444 now sits at 25ms of a 41.67ms budget instead of 35 —
and first-frame latency. Headroom is what GATE E spends.

Note for §11's benefit: **planar is not always fewer bytes.** 4:4:4 12-bit is
56.6MB of planes against 37.7MB of BGRA. It is still much cheaper, because a
memcpy is not a colour conversion, but "planar means less data" is only true for
4:2:0 and 4:2:2.

### 22.4 Scrub — unchanged, which is the requirement (§20.7)

Previews stay on swscale by design. Over **three runs each** on 4444, decoder
throughput is identical: bgra 42.0 / 44.8 / 46.8 f/s against planar 45.8 / 46.0
/ 42.7. That is the "no change" §20.7 asked to verify.

Two reproducible differences on 4444, consistent across all three runs:

- **worst UI-thread block 53.5/53.4/53.6 → 30.7/30.0/30.5ms** — better, because
  the landing is now a plane copy rather than a full-res 4444 swscale;
- **release latency 6.1/7.9/5.1 → 33.9/31.3/34.5ms** — worse.

~~The second is recorded, not explained.~~ **WRONG, and corrected below in
22.4a. The release comparison was measuring two different gestures.**

### 22.4a The release-latency "regression" was a measurement artefact

The reading that "the BGRA release was being served without a fresh full-res
decode" was the right suspicion, and it is now confirmed — which makes the
regression disappear.

`-Reversals` does not guarantee a landing. In the BGRA config it ended with
`dst RGB32/BGRA 640x360` — **preview** resolution — and `dec 0.00 | sws 0.00`,
i.e. no decode at all; in the planar config the same gesture ended on a full-res
landing. The 6ms was not a fast release, it was **no release work happening**.
Decomposing against the existing HUD fields confirms it arithmetically:

    planar   dec 15.61 + copy 10.45 + handoff 6.85 = 32.9ms  (measured 33)
    bgra     dec  0.00 + sws  0.00 + handoff 6.46 =  6.9ms  (measured 7)

Re-run with **`-SnapRelease`**, which is the gesture that reliably ends in a
release, 4K ProRes 4444, two runs each — every row `delta 0`, `target 261 shown
261`, and a full-resolution `dst` on all three:

| config | dst | release |
|---|---|---|
| cpu | `RGB32/BGRA` | 65.3 / 57.2ms |
| d3d11 BGRA | `RGB32/BGRA` | 65.6 / 55.4ms |
| **d3d11 planar** | `YUV444P12 planar` | **46.7 / 33.7ms** |

**GATE C improved release latency by roughly 20ms**, which is what its cheaper
conversion predicts. There was never a regression.

**Consequences worth keeping.** Step 8 must not be justified on release latency
— there is no target there, and shipping an upload optimisation against this
number would have booked a win that did not exist. And the general lesson is the
one this project keeps relearning: *before comparing two numbers, check the two
runs did the same work.* `dst` and `dec`/`sws` said they had not, on the same
line as the figure being compared.

### 22.5 The pool bug, worth keeping

Sharing one recycling pool between BGRA and planar buffers is right — one drag
holds both kinds, since previews stay BGRA — but the original eviction pass
dropped every unreferenced non-matching entry on **each acquire**. Harmless
while BGRA was the only kind; with two alternating it threw away and
reallocated ~56MB per landing on 4444, and measured as the shuttle going
7.8 → 18.2ms/frame while every per-frame cost stayed flat. Eviction now runs
only when the pool is actually full.

The general shape: **a policy that is a no-op under one workload becomes a
thrash under two**, and nothing about the first workload predicts it.

### 22.6 Correctness

`delta 0`, `detach 0.00`, `stale-blocked 0`, `recov 0` throughout.

| check | result |
|---|---|
| step ±5 × 3, 4K H.264 | landed **61** → ends **61** |
| step ±5 × 3, ProRes 4444 | landed **133** → ends **133** (the §15.4 frame) |
| PlayThroughDrag | 41.5% (H.264) / 20% (4444) moved |
| PausedThroughDrag (control) | **0.0%** both |
| KillMidDrag | 167ms / 162ms |
| SwitchMedia | clean |
| PNG sequence, still image | present correctly — `adopt()` gives BGRA, GATE B path |

### 22.8 The 4K H.264 stall number — settled, and it is measurement conditions

§21.4 carried "~44 stalls of ~375 against §17.4's `2 of 394`, unexplained" as
something to resolve before GATE E. Resolved. **It is not a regression, and the
larger half of it is window size.**

**It is not the code.** The prompt for this session said the range
`8a7cdb3..be8ef63` was docs and chore only; that is wrong — `5499006` changed
`MainWindow.cpp` (+72), `ViewerWidget.cpp` (+10) and `VideoRenderer.h` (+6). But
the conclusion survives inspection: on the CPU path none of it is per-frame.
`installOverlaySpike` is env-gated, `setOverlayHooks` is an empty virtual on the
CPU backend so `installOverlayHooks()` costs one struct at startup, and
`WA_PaintOnScreen` is set to *false* when there is no native surface. **Check
the diff, not the commit subjects.**

**Cache depth is a function of window size, and it dominates.** Previews convert
to the size they will be drawn at (`b5a56af`) and the cache is budgeted in bytes,
so a bigger window means bigger entries, fewer of them, and more misses. 4K
H.264 reversals, cpu, one build, two runs each:

| window | cache cap | rev-hit | dec f/s | supply | **stalls** |
|---|---|---|---|---|---|
| 900x854 | **76/76** | 98.2% | 154 | 98% | 46/375, 45/374 |
| 1300x1106 | **41/41** | 97.1% | 124 | 79% | 56/315, 62/306 |
| 1700x1354 | **27/27** | 94.9% | 79 | 49% | 140/228, 133/220 |
| 2100x1460 | **22/22** | 92.0% | 76 | 47% | 136/217, 136/213 |

Monotonic across every column. **A stall count quoted without the window it was
taken in is not a number anyone can check**, which is the actual defect, and it
is fixed: the HUD now carries `win WxH` in device pixels on the colour line, so
every capture is self-documenting. `scripts/measure/stalls_vs_window.ps1` is the
sweep.

**What window size does NOT explain is the floor.** At the smallest geometry the
app will take, stalls are 46–51, not 2. Note the window has a **minimum height of
~854** with this media, so §14.10's recorded `1296x812` is not reproducible —
requesting 812, 760 or 700 all clamp to 854, and 900/1280/1296 wide all give
46–51 stalls. So the 2 → 46 gap is not geometry.

**The remaining suspect is machine state, and it is documented rather than
tested**, because testing it means closing the owner's applications. This box is
now running `parsecd` (227s CPU), `sunshine`, Steam with several web helpers
(117s on one), Adobe Desktop Service and Creative Cloud — and the display is
**5120x1440 @ 239Hz** where §18.3 recorded `\.\DISPLAY1 2560x1440`. §"Display
refresh rate is NOT the remaining smoothness gap" in CLAUDE.md explicitly notes
its measurement was taken *with Parsec off*.

**Do not re-open this as a bug.** If a clean-environment number is wanted, take
it deliberately: Parsec and Sunshine closed, one display, and the window size
written down. Until then, compare stalls only against runs from the same session
at the same `win WxH`.

## 23. ProRes 4444 playback cadence — characterised (2026-08-09)

Owner report: 4444 playback is not locked to real time, slight perceptible
stutter. Measurement only, no fix — the project has three reverted scheduler
experiments on record precisely because the measurement came second.

### 23.1 Why the rate metric could not answer this

Presented rate reads 98–99% under two unrelated faults, so a file can measure
99.6% and still stutter. The new HUD `cadence` line reports the distribution
instead: percentiles, buckets as multiples of the frame budget, the **spacing
between long frames**, and a count of handlers that exceeded the budget.

The spacing is what separates the causes. A tick beat is *regular*; cost overrun
is not. Sampling intervals between **presents** rather than between ticks is
deliberate: a held frame produces no present, so a doubled interval only exists
on that timeline.

### 23.2 Exact rates, taken first

`TRACE_OPEN_LOG=1`: all three files are **`fps=24.000000`, `fpsQ=24/1`** — not
23.976. So the beat period is `41.667 / (41.667 − 41) = 62.5 frames`, i.e. one
doubled frame every **2.6s**.

**4444 has no audio track**; 422 HQ and the 1080p clip both do. That matters more
than it looks — see 23.4.

### 23.3 The answer: cause A dominates, and it is not 4444-specific

11s runs, `win 1280x815`, cpu unless stated:

| run | ~1x | 1.5–2.5x | >2.5x | long-gap min/med/max | handler>budget | max handler |
|---|---|---|---|---|---|---|
| **4444** rep1 | 253 | **4** | 0 | **61/61/63** | 1 of 265 | 52.6ms |
| **4444** rep2 | 252 | **5** | 0 | **56/62/63** | 1 of 265 | 53.4ms |
| 422 HQ, `TRACE_NO_AUDIO` | 164 | **3** | 0 | **60/62/62** | 0 of 170 | 34.1ms |
| 1080p, `TRACE_NO_AUDIO` | 235 | **4** | 0 | **60/62/62** | 0 of 243 | **3.8ms** |
| 1080p, with audio | 234 | 5 | 0 | 43/61/62 | 0 of 245 | 4.3ms |
| 422 HQ, with audio | 162 | 5 | 0 | 7/61/62 | 0 of 172 | 34.0ms |

**Median spacing is 61–62 on every row against a prediction of 62.5.** Nothing
lands in the 1.1–1.5x or >2.5x buckets on any run — the distribution is a tight
spike at 1x plus a handful of clean doublings, `max ≈ 2 × p50`. That is the
integer-tick beat and nothing else.

**The 1080p row is the proof it is not about cost.** Its worst handler is
**3.8ms** against a 41.67ms budget — a 10x margin — and it shows the *same* four
doubled frames at the *same* 62-frame spacing. Decode cost cannot explain a
defect that is identical on a file with ten times the headroom.

**Audio does not remove it either**, which is worth recording because it is
easy to assume otherwise: the audio clock decides *which* frame, but a frame can
still only be presented on a tick, so a held frame still produces a doubled
interval. `rep`/`skip` stay 0 while the presented interval doubles.

### 23.4 Cause B existed on 4444, and GATE C has already removed it

The 4444-specific signal is not in the buckets, it is in tick jitter:

| 4444 config | `sws` | **jitter max** | handler>budget | max handler |
|---|---|---|---|---|
| cpu (the default) | 16.9ms | **11–12ms** | **1** of 265 | 52.6 / 53.4ms |
| d3d11 BGRA | 17.0ms | **14ms** | **1** of 265 | 55.6 / 55.1ms |
| **d3d11 planar** | **5.6ms** | **2–3ms** | **0** of 265 | **37.6 / 38.6ms** |

A 25ms handler delays the timer itself; a 10ms one does not. GATE C took the
worst handler from 55.6ms — *over* the 41.67ms budget — to 37.6ms, inside it,
and tick jitter from 14ms to 2ms, which is what the light files read.

**So the owner's report almost certainly includes a cause-B component that the
planar path no longer has**, because `cpu` is still the default and that is what
they were running. The beat is unchanged on every path, as expected: GATE C
supplies headroom, not phase.

### 23.5 What this means for ordering — owner's call, TAKEN 2026-08-09

> **The call has since been made: GATE E is pulled ahead of items 8-10.** The
> reasoning below is what it was taken on; the design is at §24. The rest of
> this section is preserved as written.


Cause A is essentially all of what is left, it is universal, and **GATE E is the
only thing that fixes it**. Items 8 and 9 will not: they buy headroom, and the
measurement says headroom is no longer the binding constraint on 4444 once the
planar path is on.

That is an argument to **pull GATE E ahead of items 8–10**. It is recorded here
rather than acted on, per the plan's own rule about not reordering unilaterally.
Two things worth weighing with it:

- The fix is cheap to state and has a written composition rule already (§9):
  **audio stays the rate and position authority; vsync becomes the phase
  authority.** The flip-model swapchain that makes it possible landed at GATE B.
- Until GATE E or a default renderer change, the owner sees beat *plus* the
  cause-B component, because `cpu` is the default. Making `d3d11` the default is
  a separate decision the plan defers to GATE E, but it is now a decision with a
  measured benefit attached.

### 23.6 What was NOT established

Why the owner notices this on 4444 specifically. The beat is identical on 1080p
and 422 HQ, and no number here says 4444 is worse in that respect. The cause-B
component (jitter 11–14ms on the default renderer) is the only measured
difference, and whether that is what they are seeing needs their eye, not
another run. Do not assume content or resolution explains it without evidence.

### 22.7 Open after GATE C

1. **The 4444 release latency above.** Needs the owner, not another harness run.
2. ~~**Textures are recreated on any geometry change**~~ — **step 8 is CLOSED as
   answered-no, §27.** Both halves of this note are true as written and neither is
   a cost: "any geometry change" means the *frame's* geometry, and per-frame
   `Map/WRITE_DISCARD` is the upload rather than an allocation. Measured `tex 3`
   across 261 frames and `tex 4` across a 406-paint drag.
3. ~~**GPU scaling is still not done** (step 9).~~ **DONE, §28** — and the note
   understated it. It was not only the landing: every full-resolution frame went
   through one 2x2 sampler tap, so playback undersampled too. The CPU backend is
   still at a 2x2 tap and is now the softer path.
4. **No 10-bit output path.** The swapchain is `B8G8R8A8_UNORM`, so 10- and
   12-bit sources are converted to 8-bit for display. §9 warns not to conflate
   high-bit-depth *processing* (done now) with 10-bit *output* (step 10).
5. **BT.2020 still has no tonemap**, exactly as on the CPU path. Known gap,
   carried forward unchanged.

---

## 24. GATE E — presentation timing (DESIGN, NOT IMPLEMENTED)

**Status: written for review, 2026-08-09. No code has been changed.** §23.5
recorded the argument for pulling GATE E ahead of items 8–10 and declined to act
on it; **the owner has now taken that decision**, on the grounds that locked
real-time playback is priority #1, that §23 measured the residual stutter as the
universal integer-tick beat which only GATE E fixes, and that §23.4 measured
headroom as no longer the binding constraint on 4444 once the planar path is on.
Items 8, 9 and 10 are **deferred, not cancelled**; §22.7 items 2–5 stand.

There is no technical dependency from 8, 9 or 10 into 11 — the flip-model
swapchain GATE E needs landed at GATE B (§17, `DXGI_SWAP_EFFECT_FLIP_DISCARD`,
two buffers, `MakeWindowAssociation` already taken off DXGI).

**Read §24.7 before agreeing to any of this.** The project has three reverted
scheduler experiments on record and one of them is a near-relative of the first
half of this design.

### 24.1 The fault, restated on the timeline it actually lives on

§23 measured it at the presented-frame level: median spacing between long frames
of **61–62 on all six runs**, `max ≈ 2 × p50`, nothing in the 1.1–1.5x or >2.5x
buckets. This section restates the mechanism in terms of *when a present
happens*, because that is what a fix has to move and it is not the same quantity
as *when a tick happens*.

A present today lands at:

```
present_k  =  tick_at_or_after(due_k)  +  handler_k  +  dispatch_k
```

with `due_k = k × 41.667ms`, ticks on a fixed 41ms grid
(`MainWindow.cpp:1130`, `floor(1000/fps)`), and `dispatch_k` the queued
`update()` → `paintEvent` latency, because `ViewerWidget::setFrame` calls
`update()` and the paint — and therefore `Present()` — runs after the handler
returns.

Two independent errors are stacked in that line, and neither is visible to the
presented-rate metric:

- **Tick-grid quantisation (cause A).** Presents land on the 41ms grid, so the
  interval between two of them is 41ms or 82ms and **never 41.667ms**. The mean
  is correct, every individual interval is wrong: 61 frames run 1.6% fast, then
  one is held double. That is the §23 signature, and describing it as "a dropped
  frame every 2.6s" undersells it — the 61 fast frames are part of the artefact.
- **Handler-time modulation.** `present_k − present_{k−1}` carries
  `handler_k − handler_{k−1}` directly. 4444's handler runs 25→37ms on the
  planar path (§23.4), so presents are modulated by a ~12ms spread; 1080p's
  worst handler is 3.8ms, so they are not.

**The consequence that shapes the whole design: locking the wake does not lock
the present.** Anything that only reschedules the timer inherits `handler_k` as
present jitter. On this box a refresh period is ~4.2ms, so a 12ms handler spread
is ±3 refreshes of presentation error on the one file the owner reported.

### 24.2 What is being locked to what

**Nothing in Trace is aware of refresh phase today** (§20.5 item 3;
`Present(0, 0)`, sync interval 0). DWM composites at refresh so at most one
present is *seen* per refresh, but which refresh is an accident.

A display-synchronised player makes the number of refreshes a frame occupies a
controlled integer. At 24.000fps:

| refresh | refreshes per frame | result |
|---|---|---|
| 240.000 Hz | 10.000 | exact, no residual beat |
| 120.000 Hz | 5.000 | exact |
| 60.000 Hz | 2.500 | 2:3 cadence — imposed by the display on every player |
| 239.760 Hz | 9.990 | a 9-refresh frame every ~100 frames |

**Measure the panel's true refresh before believing any of these rows.** §22.8
recorded this display as 5120x1440 **@ 239Hz**, and a "240Hz" mode reported as
239 is very often 240000/1001 = 239.76 — on which 24.000fps content cannot be
presented at constant cadence by any player, while 23.976 content maps exactly.
The nominal mode number is not evidence; the phase source reports the real
period and that is the number to use.

> **MEASURED, 2026-08-09** (`scripts/measure/refresh.ps1`). This panel runs
> **239999/1000 = 239.999 Hz**, from `QueryDisplayConfig`, which is the only
> API that reports the rate as an exact rational — `EnumDisplaySettings` says
> "240" and cannot separate the two candidates, which is why §22.8's "239Hz" was
> unusable as evidence either way.
>
> | source fps | refreshes/frame | residual |
> |---|---|---|
> | **24.000** | **10.0000** | one slip per **24,000 frames** (~17 min) |
> | 23.976 | 10.0100 | one per **100 frames** (~4s) |
> | 30.000 | 8.0000 | one per 30,000 |
> | 25.000 | 9.6000 | non-integer; pulldown, the display's |
>
> So for the 24.000fps test set the display imposes **effectively nothing**, and
> the pass condition can be strict. Note the 23.976 row: that is the display's
> beat, not Trace's, and it is why criterion 3 exists.

So the honest claim GATE E can make is: **it replaces a 62-frame beat that Trace
creates with its own integer-millisecond tick by whatever residual beat the
display's refresh imposes, which is typically far longer and is the same one
every other player has.** At an exact 240 or 120Hz mode the residual is zero.
Do not promise zero without the measured refresh.

### 24.3 The composition rule, applied

§9, unchanged and not negotiable: **audio stays the rate and position authority;
vsync becomes the phase authority.** Concretely, per presented frame:

| question | owner |
|---|---|
| at what instant does a present happen | the phase scheduler (§24.5) |
| which frame is presented at that instant | the audio clock when it drives, otherwise the deadline index |

**The wall-clock accumulator is removed from the gating decision, not layered
underneath it.** `cd79d49` is the precedent and the warning: when the audio
clock and `playbackAccumulatorMs_` both decided when to present, they beat
against each other and produced matched hold/skip pairs, and the fix was to make
the audio clock the *only* scheduler. Adding a third opinion about *when* is how
this becomes revert number four. The gate at `MainWindow.cpp:350` goes away for
video under the phase scheduler; it is not made conditional and then left in.

What replaces the accumulator's position role is the deadline index `k` measured
from the run start, which is a stronger reference because it is computed from
the exact rational rather than accumulated. **Late presents slip the timeline;
they never skip a frame** — ordering over rate is unchanged, and the existing
4-frame backlog cap becomes a re-phase rule (§24.5 step 5).

The audio clock is not touched. `advanceClock()` stays the one place it is
stepped, `peekClock()` stays the observer, the discipline loop, slew, snap and
latency EMA are all out of scope.

### 24.4 Phase source — four candidates, one recommendation

| candidate | what it gives | cost | verdict |
|---|---|---|---|
| `DwmGetCompositionTimingInfo` | `qpcVBlank`, `qpcRefreshPeriod`, `rateRefresh` — an absolute vblank time and the measured period | one call, never waits; needs `dwmapi` | **recommended primary** |
| `IDXGISwapChain::GetFrameStatistics` | `SyncQPCTime`, `SyncRefreshCount`, `PresentCount`, `PresentRefreshCount` | one call per present; d3d11 only | **recommended as cross-check and late-present detector** |
| waitable swapchain (`FRAME_LATENCY_WAITABLE_OBJECT` + `GetFrameLatencyWaitableObject`) | a HANDLE signalled when the swapchain will accept another frame | blocks, or needs `QWinEventNotifier` | **declined — see below** |
| `IDXGIOutput::WaitForVBlank` | blocks until the next vblank | a dedicated thread, plus a cross-thread post per refresh | declined |

**The waitable swapchain is declined and the reason is worth writing down so it
is not re-proposed.** It answers "may I queue another frame", not "when is the
next vblank". At sync interval 0 it signals almost immediately and carries no
phase information at all; at sync interval ≥1 it becomes a refresh-rate
heartbeat, which means presenting every refresh — 240 draws per second of the
same picture, against a standing priority that says no feature may compromise
lightweight playback. And **blocking on it from the UI thread is the same class
of mistake as the synchronous remote read `8b47e08` fixed**: the app would be
unable to deliver a mouse move between frames. `QWinEventNotifier` makes the
wait non-blocking but does not make the signal informative.

**Why DWM first rather than DXGI first.** It decouples GATE E from the
default-renderer question. §24 would otherwise only work on `d3d11`, which the
prompt for this session correctly flagged: the cadence fix and the default flip
would then be one decision taken at sign-off. With a renderer-independent phase
source, the CPU path — still the default, and what the owner is running — gets
the fix too, and the §23.3 1080p control (which was taken on `cpu`) is directly
comparable before and after.

> **MEASURED, 2026-08-09 — and the recommendation did not survive.**
> `DwmGetCompositionTimingInfo` **fails on this machine**, returning
> `0x88980090` for a NULL HWND, the desktop HWND and the shell HWND alike, at
> three different `cbSize` values, from an interactive process on
> `WinSta0\Default` with `DwmIsCompositionEnabled` reporting **true**. It is a
> deprecated API and it is entitled to refuse; what matters is that it does.
>
> Two consequences. **The renderer-independent phase source does not exist**, so
> the grid snap can only come from the swapchain
> (`IDXGISwapChain::GetFrameStatistics`) and is therefore d3d11-only after all —
> §24.11 Q2 is answered by measurement rather than by the review.
>
> **But E1 needs no phase source at all**, and E1 is renderer-independent. So
> the owner's "yes, the CPU path too" is still honoured for the part that
> removes the beat; what the CPU path cannot have is the sub-refresh tightening.
> §24.13 measures how much that is actually worth.
>
> The first attempt at this probe got `0x88980090` from a struct declared 184
> bytes instead of 320 and concluded nothing — the same error for two unrelated
> reasons. Rule out marshalling before reading an HRESULT as an answer.

**Two things to verify on the box before committing to it**, because this
document's rule is to confirm rather than inherit:

1. `DwmGetCompositionTimingInfo` takes an HWND that is **ignored on Windows 8+**
   — it is one composition clock for the desktop, not per-monitor. On a
   multi-monitor setup with mixed refresh rates it describes the primary. This
   box has one display, so the limitation is *untested*, not *broken* — the same
   status as §20.4, which GATE E promotes from "untested" to "untested and now
   load-bearing".
2. That `qpcRefreshPeriod` on this panel reads the true period and not a rounded
   one. If it disagrees with `GetFrameStatistics`' implied period on the d3d11
   path, that disagreement is the finding, and the cross-check exists to expose
   it.

If neither source is available, the scheduler degrades to §24.6's E1 — unsnapped
deadlines against the exact rational. **It must never degrade to a wait.**

### 24.5 The scheduler

State per playback run: the QPC time at the run's first present `t0`, the frame
index `k` from the run start, the exact period

```
T = 1000 × fpsDen / fpsNum        (ms, double, never rounded)
```

read from `VideoMetadata::fpsNum`/`fpsDen`. **This is the first thing in the
codebase to read the rational** (`7b924be` is foundation only, §20.5 item 2);
every other consumer keeps `FrameSource::fps()`. When the pair is absent or
zero, `T = 1000/fps` from the double and everything else is unchanged.

Per frame:

1. **Ideal due** `D_k = t0 + k × T`. Computed in QPC ticks as a double; nothing
   here is rounded to a millisecond.
2. **Grid snap** `V_k` = the vblank nearest `D_k` on the measured grid
   `vblank0 + n × P`, with `vblank0` and `P` re-read from the phase source (see
   staleness, §24.8). Skipped when there is no phase source; `V_k = D_k`.
3. **Wake** `W_k = V_k − lead`, where `lead` is a measured EMA of the
   prepare-to-present cost (upload + `update()` dispatch + `Present`) plus a
   jitter margin, clamped below one refresh period. Being early is safe —
   `Present(0,0)` hands the frame to DWM, which shows it at the next composition
   — and being late costs a whole refresh, so the margin is deliberately
   asymmetric.
4. **Arm** a single-shot `Qt::PreciseTimer` at `max(0, round(W_k − now))`.
   Rounding here does **not** accumulate, because the next arm is computed from
   the absolute `D_{k+1}` and not from this one.
5. **Re-phase on overrun.** If the handler ran long and `now > V_k`, the next
   deadline is `max(D_k + T, the first grid point after now)` rather than a
   banked debt. This is the analogue of today's 4-frame backlog cap
   (`MainWindow.cpp:367`) and it exists for the same reason: a run that stalled
   must resume *at rate*, not fast-forward through the arrears. **Never more
   than one frame presented per wake**, exactly as today (`steps = 1`).

With audio driving, step 1–4 are unchanged and the *frame* comes from the audio
clock as it does now (`MainWindow.cpp:387-432`), including the hold branch and
the bounded 3-frame catch-up. Under GATE E the wake rate and the audio rate are
both real time, so `delta` should be 1 nearly always and `rep`/`skip` should
fall to genuine sound-card-versus-display drift, which is a ppm effect. **If
`rep` does not fall, the phase scheduler is not doing what this section claims**
— that is the sharpest available check and it costs nothing to read.

### 24.6 Two commits, separately revertable

Plan §8 item 11 is one line; this design is two changes with different blast
radii, and folding them into one commit would make it impossible to say which
one bought what. **Splitting item 11 is a decision for the review** (§24.11).

**E1 — `perf(playback): schedule presents against the exact source rate`.**
Steps 1, 4 and 5 of §24.5 without the grid snap. Replaces the fixed integer
periodic tick and the accumulator gate with an absolute-deadline single-shot.
Renderer-independent, so it lands on the default path. Expected effect: the
41/82 quantisation is gone, present intervals become ~41.667 ± handler jitter,
the 62-frame signature disappears. Expected non-effect: handler-modulated
jitter on 4444 is untouched.

**E2 — `perf(gpu): add DXGI presentation timing` (GATE E).** Adds the phase
source, the grid snap, the late-present telemetry, and the one change that
§24.1 says is structurally necessary:

> **The present and the decode swap places inside the handler.** Today the wake
> decodes frame `k` and then requests the paint, so the present carries the
> decode cost. E2 presents the frame prepared during the previous interval
> *first*, then decodes the next one into a pending slot. Present time becomes
> wake + `lead`, independent of decode cost; a decode that overruns the interval
> makes the *next* present late, which is the genuine cost-overrun case (cause
> B) reported honestly instead of smeared into every frame.

This is a small change and it is important that it stays one: **same thread,
same synchronous decode, same one frame per interval, same linear-forward
order.** It is not the async decode thread, and it does not reopen that
question — the only new state is one pending `VideoFrame`, which is a refcount.
It costs **one frame interval of added display latency** at the start of a run,
and it needs care at three points, all listed in §24.8.

### 24.7 The three reverted scheduler experiments — E1 is a near-relative of one

The comparison table at `MainWindow.cpp:1109-1117`, on the 4K ProRes 4444
benchmark, 261 frames, Release:

| scheduler | wall | fps | rate |
|---|---|---|---|
| periodic precise @ frame interval *(shipped)* | 11.00s | 23.74 | **98.9%** |
| 6ms poll + accumulator gate | 11.34s | 23.01 | 95.9% |
| **adaptive single-shot per frame** | 11.07s | 23.57 | **98.2%** |

**The third row is E1's ancestor and it was rejected.** Three things are
different now, and all three are claims to test rather than assert:

1. **The metric that rejected it cannot see the fault it addresses.** Every
   number in that table is presented rate, and §23.1 established that presented
   rate reads 98–99% under both causes and therefore cannot distinguish them.
   Nothing in the table says whether the beat differed between the three rows —
   the instrument that could say so (the `cadence` distribution) was built four
   months later. **The re-run is judged on the distribution, not the rate.**
2. **The starvation argument was about a 35ms handler.** The recorded reasoning
   is that "the ~35ms blocking handler starves the event loop, so a short tick
   is only delivered every ~13ms". That is a sound objection to the *6ms poll*
   row. GATE C has since taken 4444's worst handler to 37.6ms with tick jitter
   of 2–3ms on the planar path (§23.4), and **the 1080p control — 3.8ms worst
   handler, ten times the headroom — has never been run against a single-shot
   scheduler at all.** Run 1080p first for exactly this reason.
3. **A checkable hypothesis for the 0.7%.** E1 re-arms from an *absolute*
   deadline. If the reverted implementation armed a rounded `round(T) = 42ms`
   instead, that is a 0.8% systematic rate deficit — which matches
   98.9% → 98.2% almost exactly, and is the same `round`-versus-`floor` error
   already documented at `MainWindow.cpp:1119-1128`. **The reverted code is not
   in git history** (searched: no `-S"singleShot"` hit on `MainWindow.cpp`, no
   matching commit), so this cannot be settled by reading. It is stated as a
   hypothesis so that a re-run either confirms it or refutes it, and so that
   nobody argues E1 down from a table whose implementation is unknown.

The 6ms-poll row stays rejected and E1 is not it: E1 arms one wake per frame at
a computed deadline; it does not poll.

### 24.8 Failure modes, edge cases and what each must degrade to

- **Non-integer refresh:frame ratio.** 24fps at 60Hz is 2:3; at 239.76Hz it is a
  9-refresh frame every ~100. Both are the display's cadence, not a defect, and
  the scheduler produces them naturally by snapping to the nearest grid point.
  **This is expected output and must be written into the pass condition
  (§24.9), or the first 60Hz run will look like a regression.**
- **Refresh changes mid-session** (mode change, monitor move, DWM restart). The
  grid must be re-acquired, never cached at open: a stale period is a slowly
  drifting phase, which reads exactly like the fault being removed. Re-read on a
  cheap cadence and on window move; treat a period that has changed by more than
  a small tolerance as a re-acquire, not a filter input.
- **Multi-monitor.** §20.4 records that real per-monitor DPI transitions have
  never run on this one-display box. GATE E promotes that from a gap to a
  load-bearing gap, because a DWM composition clock describes the primary
  display. Say so at sign-off rather than discovering it.
- **Device loss / renderer fallback.** Losing the swapchain must degrade to E1's
  unsnapped deadlines. Never to a wait, never to a hang, never to the old
  integer tick silently — the HUD must say which phase source is live, for the
  same reason `renderer` names the backend: a path that quietly never engages
  while the app looks fine is the failure mode this whole boundary is built
  against.
- **Media with no rational.** `fpsNum`/`fpsDen` absent → `T` from the double.
  Unchanged behaviour otherwise.
- **Image sequences** keep today's path (fixed 24, multi-step catch-up).
  Out of scope, and the `isVideo` branch already separates them.
- **J-K-L off-speeds and reverse playback.** `T` becomes `T/speed`; the grid
  snap still applies. Audio is 1x-forward only, so these are wall-clock, and
  they are **not** part of the sign-off — they must merely not regress.
- **`storageBusy_` deferral.** A wake dropped because a remote read is in
  flight must not lose the timeline; §24.5 step 5's re-phase covers it, and it
  is the case to write a test for because it is the one that only appears on
  LucidLink.
- **E2's swap, three care points.** (a) The first interval of a run has nothing
  prepared — present the frame already loaded by open/step and decode the next.
  (b) `playbackAtEnd_` and the exhausted-decoder branch (`MainWindow.cpp:441`)
  now fire one interval after the decode that discovered the end; the rewind on
  Play (`c3335ec`) must still be correct. (c) Audio catch-up may want `k+2` or
  `k+3` when only `k+1` is prepared — decode forward within the same interval,
  which is what the existing bounded catch-up already does.

### 24.9 Success criteria — defined before building

**Pass conditions.**

1. **The 62-frame signature is gone.** §23 measured median spacing between long
   frames at 61–62 on all six runs, `max ≈ 2 × p50`, nothing in the 1.1–1.5x or
   >2.5x buckets. That signature disappearing is the pass condition. Report the
   **whole distribution**, not a replacement median: a beat that moves from 62
   to some other tight value is the same fault at a different period.
2. **The 1080p control improves first.** Worst handler 3.8ms against a 41.67ms
   budget, the same four doubled frames at the same 62-frame spacing (§23.3). It
   has ten times the headroom, so if the fix is real it works there before
   anywhere else. If 1080p does not improve, stop — the mechanism is wrong and
   no amount of 4444 tuning will show it.
3. **A residual display-imposed beat is allowed and must be reported as such**,
   with the *measured* refresh period beside it (§24.2). At an exact 240 or
   120Hz mode the expectation is none.
4. **No regression on the audio-mastered path**: 1080p H.264 99.1%, 4K H.264
   98.3%, 4K ProRes 422 HQ 98.4%, skips 0. And `rep` should *fall* (§24.5).
5. **Refresh sweep at ~60 / 120 / 240 Hz** on the Odyssey G95SC, as a
   regression check only. The prior measurement at 59 / 119.98 / 240 Hz had flat
   counters (99.1 / 99.1 / 98.7%) and an owner verdict of "about the same" and
   "hard to tell", with two runs at one rate spanning the same range as all
   three rates. **Do not expect a win.** The point is that a display-synchronised
   path must not be *worse* at any of them, including the non-integer 59Hz case.
6. **Owner sign-off on 4K ProRes 4444 playback.** The harness says the mechanism
   works; only the owner says the stutter is gone. This project has recorded
   that split six times.

**Protocol.**

- **`TRACE_NO_AUDIO=1` on every control, and this is not optional.** 4444 has no
  audio track while 422 HQ and the 1080p clip both do, so as shipped they run on
  different schedulers; comparing them directly would "prove" 4444 is uniquely
  bad when the only difference is which clock is driving.
- **Quote `win WxH` with every number** (§22.8). The HUD carries it.
- **Run both renderers** for every row, since E1 is renderer-independent and E2
  is not.
- **A negative control is required.** A build with the phase source forced off
  must still show the old 62-frame signature, so the harness is proven able to
  see the thing it is claiming to have removed. §16.6 is the precedent: a check
  that can only report success proves nothing.

**One new instrument is needed and does not exist.** The honest cadence answer
under GATE E is the distribution of **refreshes occupied per presented frame** —
an integer, ideally constant — plus a count of presents that missed their
intended refresh. On d3d11 that comes from `GetFrameStatistics`
(`PresentCount` against `PresentRefreshCount`); renderer-independently it comes
from present QPC times against the measured grid. `cadence.ps1` extends to read
it. Without this the sweep in criterion 5 cannot be judged, because presented
rate is blind here for exactly the reason §23.1 gives.

### 24.10 Scope — what GATE E is not

Presentation timing only.

- **Not** items 8 (texture reuse), 9 (GPU scaling) or 10 (10-bit output).
- **Not** the scrub path. §22.4 established it is unchanged by GATE C and it
  must stay that way; nothing in §24 touches `flushVideoScrub`, the worker, the
  lease or the sampled preview.
- **Not** the audio clock. It keeps the rate and position question entirely.
- **Not** the default-renderer flip. That is a separate product decision, now
  with a measured benefit attached (§23.4) and a live A/B in front of the owner
  (§24.12).
- **Not** image sequences, exclusive fullscreen, or any interface work.

### 24.11 Open questions for the review

1. **Split plan item 11 into E1 and E2?** It is one line in §8 and two changes
   with different blast radii here. Recommendation: split, because E1 is
   renderer-independent and lands on the default path while E2 does not, and
   because a single commit would make it impossible to attribute the win.
2. **Phase source: DWM (renderer-independent) or DXGI-only?** DWM decouples the
   cadence fix from the default-renderer question and reaches the CPU path the
   owner is running; DXGI-only keeps the entire blast radius off the default
   renderer. Recommendation: DWM primary with `GetFrameStatistics` as
   cross-check on d3d11 — but this is the decision to take at review, not one to
   discover at sign-off.
3. **Is E2's present/decode swap acceptable?** It adds one frame interval of
   display latency at run start and reshapes the tick handler. It is the only
   thing that decouples present time from decode cost, so declining it caps GATE
   E at E1's precision — which may be enough for 1080p and is measurably not
   enough for 4444.
4. **What is the panel's true refresh period?** 239 vs 239.76 vs 240.000 changes
   what "exact cadence" can even mean for 24.000fps content (§24.2). Measure
   before promising.
5. **Does the CPU path get E1?** It is the default and it is what the owner is
   running today. Saying yes means the default renderer changes behaviour in
   this gate; saying no means the owner sees no improvement until the default
   flips.

### 24.13 E1 — IMPLEMENTED AND MEASURED (2026-08-09)

`FrameSource::fpsRational` + the absolute-deadline scheduler in `MainWindow`.
Renderer-independent, on by default, `TRACE_DEADLINE_SCHED=0` restores the old
fixed-interval tick and its accumulator gate **in the same binary** — which is
what §24.9's negative control asked for, and it is a better control than a build
of the previous commit because the two runs differ in one branch rather than in
a compile.

All runs `win 1280x815`, `TRACE_NO_AUDIO=1` unless stated, 11s, `cpu` unless
stated. Two repeats each; both are shown where they differ.

**The negative control reproduces the fault, and predicts its own period.**
The 1080p clip is **23.98fps**, so the predicted beat is
`41.71 / (41.71 − 41) = 58.7` frames — not the 62.5 of a 24.000 file.

| 1080p, `Universe_rc07` | control (`TRACE_DEADLINE_SCHED=0`) | **E1** |
|---|---|---|
| ~1x bucket | 269 / 266 | **275 / 270 — all of them** |
| 1.5–2.5x | **5 / 4** | **0 / 0** |
| long-gap spacing | **57/58/59** | **none** |
| p50 | 41.0 *(the 41ms grid)* | **41.9** *(the true 41.71)* |
| p99 / max | 81.5 / 82.9 | **43.3 / 43.9** |
| present-late avg/max | 20.94 / 41.26 | **0.62 / 1.73** |
| drift | −11.2 / −13.0 ms | **−1.5 / −0.0 ms** |
| real time | 99.9% | **100.0%** |

**4K ProRes 4444 — the owner's file.** The control matches §23.3's recorded
baseline within noise (252/253 at ~1x, 5/4 long, spacing 58/61/62, one handler
over budget at 52.9ms), which is the check that the two sessions are comparable.

| 4444 | control, cpu | E1, cpu | **E1 + d3d11 planar** |
|---|---|---|---|
| 1.5–2.5x | 5 / 4 | 1 / 1 | **0 / 0** |
| long-gap spacing | 58/61/62 | none | **none** |
| p50 | 41.0 | 41.8 | **41.7** |
| max gap | 82.6 | 62.5 | **45.9** |
| handler > budget | 1 of 265 | 1 of 260 | **0 of 260** |
| rephase | 0 | 1 | **0** |
| real time | 99.4% | 99.3% | **99.8%** |
| drift | −64 ms | −74 ms | **−25 ms** |

The one long frame left on `cpu` is the one handler over budget — **cause B,
which GATE E does not fix and is not being asked to.** GATE C's planar path
already removes it (§23.4), and the right-hand column is the two together.

**Audio-mastered non-regression — every file improved, and `rep` fell.**
§24.5 named `rep` as the sharpest available check: under a deadline schedule the
wake rate and the audio rate are both real time, so holds should collapse to
genuine drift.

| file | baseline | **E1** | rep | skip |
|---|---|---|---|---|
| 1080p H.264 | 99.1% | **99.6%** | 4–5 → **1** | **0** |
| 4K H.264 | 98.3% | **99.1%** | **1** | **0** |
| 4K ProRes 422 HQ | 98.4% | **99.2%** | **1** | **0** |

`clk-upd 1/1` throughout, so telemetry is still not stepping the audio clock.
Lifecycle: `PlayThroughDrag` 12.8% moved, `PausedThroughDrag` **0.0%**,
step ±5 cycles and play-after-release both clean.

**A metric of mine was wrong and the fix is worth recording.** The first 4444
run read `sched tick 9ms | jitter 34.00/33.08/52.00` and looked like a disaster.
It was not: the timer is re-armed at the *end* of the handler, so the armed
interval excludes the handler's own 33ms while the wake-to-wake delta includes
it, and `tickDelta − armedInterval` had silently become a measure of decode
cost. Jitter is measured against the **frame period** now, which is what it
always meant — before GATE E the armed interval *was* the period. Same run after
the fix: **0.65 avg / 2.49 max**. A derived metric whose inputs changed meaning
reads as a catastrophic result, not as a broken metric, and there is nothing in
the number itself that says which.

### 24.14 GATE E — PASSED at step 1. E2 is NOT being built.

**Owner sign-off, 2026-08-09: "wow, Playback is great!"** — and the important
detail is *what they ran*. Asked which build, the answer was **"just
double-clicked the app", i.e. `TRACE_RENDERER` unset, i.e. the CPU default.**

So the sign-off is on **E1 alone, on the default renderer, with no GPU path
involved.** That is a stronger result than the design anticipated and it settles
several things at once:

- The stutter the owner reported was **cause A, the tick beat, essentially in
  full.** §23.4 measured a cause-B component on `cpu` (one handler over budget,
  jitter 11–14ms) and §23.5 predicted the owner was seeing beat *plus* that.
  They were not seeing enough of it to matter: removing the beat alone cleared
  the complaint on the very configuration that still has the cause-B component.
- **§23.6 stays open and is now unlikely to be resolvable.** Why 4444 stood out
  when the beat was identical on 1080p and 422 HQ was never established, and the
  fault is now gone, so the evidence for it is gone with it. Do not re-open it
  speculatively.
- The GPU path's remaining advantage on 4444 (0 vs 1 doubled frame, 0 vs 1
  over-budget handler, 99.8% vs 99.3%) is **real but below the owner's
  threshold**. It is an argument for flipping the default, not a requirement.

**E2 is stopped by owner decision.** Every measurable criterion in §24.9 was met
by E1, so E2's remaining value was the removal of an occasional one-refresh
wobble at the cost of one frame of display latency and a reshaped tick handler.
That was not the expectation: §24.1 argued E2 was structurally necessary because
present time carries `handler_k − handler_{k−1}` and 4444's spread is ~12ms,
three refreshes. **The premise was overstated for the planar path** — on d3d11
the 4444 handler is ~21.6ms total and the present-to-present spread is
`p50 41.7 → max 45.9`, about one refresh, with nothing in the doubling bucket.

The design at §24.4–24.6 is **retained, unbuilt**. If a cadence complaint returns
on a specific file, it is ready to pick up — and note that its phase source is
settled by measurement: `GetFrameStatistics` on the swapchain, d3d11 only,
because §24.4's renderer-independent route does not exist on this machine.

**Criterion 5, the refresh sweep, is MOOT for E1 and was deliberately not run.**
The criterion was written for a display-synchronised path — "must not be worse at
any refresh rate". E1 is not display-synchronised: it schedules against the
source rational and knows nothing about refresh, so changing the monitor's rate
**cannot move its counters**, which is exactly the finding CLAUDE.md already
records from the 59/119.98/240Hz sweep ("expected, since nothing in the current
path is display-synchronised, so the counters *cannot* move"). Running it would
have cost three display-mode changes to reproduce a known null result. **It
becomes required the moment E2 is built and not before.**

### 24.12 Before any code — the zero-cost A/B, put to the owner

§23.4 measured that the planar d3d11 path already removes 4444's cause-B
component: tick jitter **11–14ms → 2–3ms**, worst handler **55.6ms (over budget)
→ 37.6ms (under)**, zero handlers over budget. `cpu` is still the default, so
the owner's stutter report includes a component the planar path no longer has.

The ask: play 4K ProRes 4444 twice, once at the default and once with
`TRACE_RENDERER=d3d11`, and say whether the d3d11 run feels better.
`Run Trace (cpu).bat` and `Run Trace (d3d11).bat` in the repo root do this; the
HUD `renderer` field confirms which one actually engaged.

It costs nothing and buys two things: it may improve the owner's experience
immediately, and it is **the only available evidence for §23.6, which is still
open — why the owner notices this on 4444 specifically**, when the beat is
identical on 1080p and 422 HQ. Do not assume content or resolution explains it.

A yes makes flipping the default to `d3d11` a live option to take *with* GATE E
rather than after it. **It is not to be flipped unilaterally** — it is a product
decision, and §5's "`TRACE_RENDERER=cpu` stays the default until Gate E" is a
statement about when the question opens, not an instruction to answer it.

---

## 25. `d3d11` is the default renderer (2026-08-10, owner decision)

**The owner tested both backends side by side and chose `d3d11`.** This is the
product decision §24.12 and §5 both deferred to this moment, and it is taken
here rather than inferred from the numbers, which is what those sections asked.

Every gate that held it back has passed: **GATE B** with visual sign-off (§20.2),
**GATE C**'s planar upload confirmed against swscale at 8, 10 and 12 bits (§22),
and **GATE E**'s cadence work (§24.13). §5's "`TRACE_RENDERER=cpu` stays the
default until Gate E" has run its course.

### 25.1 The measured case, on 4K ProRes 4444

| | `cpu` | **`d3d11` + planar** |
|---|---|---|
| doubled frames per 11s | 1 | **0** |
| handlers over budget | 1 of 260 | **0 of 260** |
| worst present gap | 62.5ms | **45.9ms** |
| tick jitter max (§23.4) | 11–14ms | **2–3ms** |
| conversion (`sws`) | 16.6ms | **5.6ms** |
| real time | 99.3% | **99.8%** |

Note what this is **not**: a playback-rate win. §22.3 established presented rate
was unchanged at 98.3–99.6% by GATE C because none of these files was
conversion-bound at 24fps. What the GPU path buys is **headroom** — and GATE E
turned that headroom into the last doubled frame going away, because a 25ms
handler delays the timer and a 10ms one does not.

### 25.2 What changed, and what the escape hatch is

`createRenderer()` returns `D3D11VideoRenderer` when `TRACE_RENDERER` is unset,
on any build that has it (Windows + MSVC + fxc). **`TRACE_RENDERER=cpu` is now
the control and the escape hatch, and it is the first thing to try if anything
about the picture looks wrong.**

Three properties that made this a one-line change rather than a project:

- **Fallback already lives in the host.** `ViewerWidget`'s constructor adopts
  `createCpuRenderer()` if the selected backend fails `initialize()`, and warns
  which one is presenting. A GPU backend fails for reasons that only exist once
  there is a device and a window, so this was always the right place for it
  (§17.3) — and it is what makes flipping the default safe on a machine nobody
  has tested.
- **The HUD names what is actually presenting**, not what was requested. That is
  the check that the flip took, and it is the reason "a GPU path that quietly
  never engages while the app looks fine" (§12) is not a risk here.
- **A non-Windows build has no d3d11 to default to**, so the `#else` arm returns
  the CPU backend for an unset variable and lets an explicit `d3d11` request
  fall through to the warning. A build that cannot honour the request says so.

### 25.3 Verified after the flip

Launched with **no environment variables at all** — what a double-click gets:

- HUD reads **`renderer d3d11`**, `sws 5.05ms`, `dst` planar;
- 4444: **99.8% of real time**, 0 in the doubling bucket, `handler>budget 0 of
  260`, `rephase 0`, max gap 45.5ms;
- `TRACE_RENDERER=cpu` still reads **`renderer cpu`** with `dst RGB32/BGRA`;
- lifecycle on the new default: `PlayThroughDrag` 13.5% moved,
  `PausedThroughDrag` **0.0%**, step ±5 cycles clean.

### 25.4 What this obliges the next session to do

**Every scrub and playback baseline in this document was taken on `cpu`**, and
most are not tagged with a renderer because there was only one default. They are
still valid as *records*; they are no longer valid as *comparisons* against a
run taken today. Re-tag as you re-measure — and per §22.8, quote `win WxH` too,
since stall counts are a function of window size and dominate.

The two open items §20.4 and §24.4 leave behind are now **more** load-bearing,
not less: real mixed-monitor DPI has never been tested and the box has one
display, and the display's mode was observed to change mid-session on
2026-08-10 (5120x1440 @ 239.999Hz in the morning, 1920x1200 @ 60Hz in the
afternoon). A default GPU path makes both of those the shipping path.

## 26. Scrub stalls — the metric was half the problem (2026-08-10)

Picked up as the oldest live owner-facing scrub complaint (§15.5 item 1, and
item 1 of CLAUDE.md's "Known open items"). Two findings, and the first one
changes how every earlier number in this document reads.

### 26.1 `stalls` is measured against the display, and the display moved

`scrubPaintStalls_` counts paint gaps over `2 × refresh interval`. Nothing in
the HUD said so, and the refresh is not a constant on this box: 5120x1440 @
239.999Hz on the morning of 2026-08-10 and 1920x1200 @ 60Hz that afternoon. The
threshold is therefore **8.3ms in one session and 33.3ms in another** — a factor
of four, from the monitor, applied to the metric that defines the problem.

Measured on **one run**, so there is no confound at all: 4K H.264 reversals,
`win 1284x1067`, d3d11 —

    stalls 51 of 363 (>8.3ms) | hitch 3 (>33ms)

Same paints, same build, same gesture. 51 or 3 depending only on the threshold.

**This is most of the §21.4 mystery.** That entry carried "~44 stalls of ~375
against §17.4's `2 of 394`, unexplained" as a pre-GATE-E blocker. §22.8 resolved
it as window size plus machine state — window size is real and its sweep stands
— but it *recorded* the display changing from `2560x1440` (§18.3) to "5120x1440
@ 239Hz" and filed that under machine state, when it was the metric's own
denominator. `2 of 394` is what this distribution looks like at a 33.3ms bar.

`hitch` is **added**, not a redefinition, because the two answer different
questions and both are worth having: `stalls` is "slower than the panel could
have shown it" and is the right companion to `wasted`; `hitch` is "the picture
visibly stopped". `stalls` now prints its own threshold, so a figure copied out
of a screenshot carries the unit it was taken in.

**The general rule this is the third instance of:** a derived metric whose
input changed meaning does not announce itself. GATE E's `jitter` read 34ms on
a schedule within 1.8ms of its deadline (§24.13). Here a metric read 51 on a
run with 3 real hitches. Check what a number is measured *against* before
believing it, and never compare one across sessions without its denominator.

### 26.2 §15.5 item 1 is answered, and the answer is no

The nominated untried lever was "convert Step and cache-fill conversions to
display size, as scrub previews already are", to multiply cache depth against
the byte budget. Its premise was that nothing is halved at 1080p, so 24 full-res
entries exhaust the budget and bytes bind.

**GATE C had already collected most of it.** A full-resolution 1080p entry is a
`yuv420p` plane set at **3.11MB**, not an 8.29MB BGRA frame, because full-res
frames stop going through swscale on the planar path. Depth was already **64**
entries and the hit rate **96.8%**, not the weak case §15.5 describes. What
display-size conversion adds on top is 3.11 → 2.54MB — **eighteen percent** —
and it buys that by replacing a 0.25ms plane copy with a multi-millisecond
swscale resample, on the one path whose entire per-frame cost is the round trip.
It was a winning trade when it was written and is a losing one now.

This is the §25.4 obligation biting exactly as predicted: the note was correct
against the `cpu` default it was written under, and stale against the shipping
one. **A deferred item's premise expires; re-derive it before building it.**

### 26.3 What the misses actually needed: bytes

The question was settled by measurement rather than by the arithmetic above,
which is the right way round — quadrupling the budget is a strictly stronger
version of anything a cheaper entry representation could buy, so hitches falling
when it was raised is what proved they were misses at all. `TRACE_REVERSE_CACHE_MB`
exists for that experiment and stays as the control.

Reversal drags, `win 1284x1067`, d3d11, two reps where shown:

| file | budget | cache | rev-hit | seeks | **hitch** | max gap | WS |
|---|---|---|---|---|---|---|---|
| 1080p H.264 | 192MB | 64/64 | 96.8% | 11, 11 | **8, 8** | 60, 68ms | 396MB |
| 1080p H.264 | **384MB** | 129/129 | 98.7, 99.0% | 4, 3 | **3, 2** | 66, 67ms | 598MB |
| 1080p H.264 | 768MB | 258/258 | 99.2% | 2 | **1** | 51ms | — |
| 4K H.264 | 192MB | 67/67 | 97.7% | 5 | **3** | 170ms | 677MB |
| 4K H.264 | **384MB** | 146/146 | 98.3, 98.2% | 3, 3 | **1, 1** | 80, 91ms | 902MB |
| 4K H.264 | 768MB | 194/268 | 98.6% | 2 | **1** | 73ms | 1110MB |
| ProRes 4444 | 192MB | 75/75 | 9.9% | 98 | **7** | 169ms | 572MB |
| ProRes 4444 | **384MB** | 103/108 | 19.5% | 97 | **5** | 48ms | 710MB |

384MB is the knee and is now the default. 768MB buys 1080p another hitch and
another ~400MB.

**4444 moves least and that is structural, not a shortfall.** Every frame is a
keyframe, a seek lands on the target, no intermediate frames are ever produced,
so there is nothing to cache — §15.1 measured `rev-hit 0.0%` and `walk max 0f`
there. Its improvement is the worst gap, not the hit rate. **Do not try to fix
4444's hit rate with more bytes.**

**Cost is memory and only memory.** Playback is untouched: 4444 with
`TRACE_NO_AUDIO=1` reads 99.8% of real time, **0 doubled frames**, `handler>budget
0 of 260`, max gap 45.2–45.3ms over two runs — the §25.1 record exactly. Step ±5
after a release returns to the landed frame with `delta 0`, `stale-blocked 0`,
`recov 0`, and the drag exercised cancellation cleanly (`abandoned 0 stale 1`,
`cancel 0.34ms`).

**The memory figures are a product decision the owner has not taken.** ~900MB of
working set on a 4K file is normal for a review tool on a workstation and is not
playback cost, but "lightweight" is one of the three pillars. `TRACE_REVERSE_CACHE_MB`
makes it one number to change either way.

### 26.4 Open after this

1. ~~**Owner validation.**~~ **DONE — passed, 2026-08-10, see §26.6.** Every
   figure here said misses are rarer and the worst gap is shorter; the owner has
   now confirmed the drag feels good on the shipping build. §26 is closed.
2. **The convert pool is sized in pre-GATE-C currency.** It prices the smallest
   entry as `w*h*4` at ≤1920 and `w*h` above, both BGRA assumptions, so at 1080p
   it provisions ~50 buffers for a cache that now holds 129. `alloc` is
   0.61–0.65ms/frame at 4K/384MB against a 32ms total — 2%, visible but not
   binding. **Left alone deliberately**: changing it in the same commit as the
   budget would have confounded the measurement above.
3. **§15.3 still holds.** Directional prefetch remains declined — supply is
   55–67% on the files that hitch, so the worker is saturated and has no idle
   time to spend speculating. Nothing here changes that.

### 26.5 Verification of the 384MB default — owner-requested, 2026-08-10

Owner kept the default and asked for three confirmations and no further work.
No code changed for this; all three are measured on the shipping build.

**(1) It does not grow without bounds.** Bounding is structural: there is exactly
one insertion point (`pushReverseCache`'s `push_back`) and the eviction loop runs
immediately after it, until `bytes <= budget` or one entry remains. The
documented exception is a single frame larger than the whole budget, which is
kept — bounded by one frame. The replace-in-place branch above it can overshoot
by (new − old) size for one frame and is corrected by the next insert.

Measured rather than asserted: six consecutive multi-gesture reversal runs on 4K
H.264 in one process, working set sampled between each —

    before any scrub  429MB
    round 1  875MB    round 2  928MB    round 3  907MB
    round 4  916MB    round 5  922MB    round 6  920MB

It fills on the first run and then plateaus; rounds 2–6 vary ±20MB with no
trend. The HUD after round 6 reads `cache FIFO | 111/111 (382.2/384 MB)` after
**1357 inserts and 1245 evictions** — the budget was never exceeded.

The convert pool is separately bounded by `convertPoolLimit` entries (clamped,
max 132); when full it returns null and the converter allocates a private buffer
the frame owns, so a full pool costs an allocation rather than growth.

**(2) It is discarded on a file change.** `close()` clears the cache and its byte
count, and `open()` calls `close()` first, so changing files cannot carry entries
across. `open()` clears the convert pool too. `setScrubPreviewSize` (window
resize) and `setPlanarOutputEnabled` also clear it, because an entry describes a
decision and does not survive the decision changing.

Measured by opening a second file in the same process after the six rounds
above: working set **920 → 254MB**, and the HUD reads `cache FIFO | 1/129 (3.0/384
MB) | hit 0.0% (0/1) | ins 1 evict 0` — empty, and the capacity re-derived for
the new source. Note there is **no explicit close-file action in the UI**; opening
another file and quitting are the only routes out, and both are covered.

**(3) It does not affect playback.** A/B at 384 against `TRACE_REVERSE_CACHE_MB=192`,
two runs each, `win 1280x829`/`1280x815`, d3d11. Note the display had changed to
1920x1200 @ 59.999Hz by this point, so 24fps carries the display's own 2:3
cadence in every row — that is imposed on all players equally and is not Trace's.

| file | budget | real time | frames | `>2.5x` | `handler>budget` |
|---|---|---|---|---|---|
| 1080p H.264 (audio) | 192 | 99.6% | 240/240 | 0 | 0 of 240 |
| 1080p H.264 (audio) | **384** | 99.6%, 99.6% | 240/240 | 0 | 0 of 240 |
| 4K H.264 (audio) | 192 | 99.1%, 99.1% | 120/120 | 0 | 0 of 120 |
| 4K H.264 (audio) | **384** | 99.1%, 99.1% | 120/120 | 0 | 0 of 120 |
| ProRes 4444 | 192 | 99.8%, 99.8% | 261/261 | 0 | 0 of 260 |
| ProRes 4444 | **384** | 99.8%, 99.8% | 261/261 | 0 | 0 of 260 |

Rate, frame count, doubling bucket and over-budget handlers are identical
throughout. **One figure is not identical and is reported rather than smoothed**:
4444's `1.5-2.5x` bucket reads 0,1 at 192 and 2,2 at 384. The within-config
spread overlaps, `>2.5x` is 0 in all four runs and the presented rate does not
move, so this is at or below the noise floor — but it is two counts, not zero,
and a future 4444 cadence question should know it was seen.

**The display mode changed mid-session again**, 5120x1440 @ 239.999Hz to
1920x1200 @ 59.999Hz, this time between the §26.3 sweep and this verification.
The §26.3 table is unaffected: every capture in it reads `(>8.3ms)`, so the whole
sweep was taken at 240Hz, and `hitch` is threshold-independent by construction.
**This is the second time in one session that the hazard §26.1 describes has
fired live.** Quote `hitch`, and check the printed threshold.

### 26.6 Owner sign-off on the finished build — §26 CLOSED (2026-08-10)

The owner ran the subjective scrub test on the shipping build — 384MB reverse
cache, `d3d11` default — and **it passed: the picture feels good.** That closes
§26.4 item 1, and with it the whole scrub-stall pass and item 1 of CLAUDE.md's
"Known open items". Nothing about scrub stalls carries forward as open.

This is the fourth time the project has needed the split between "every figure
improved" and "the bar holds", and the fourth time only the owner could supply
the second half. The figures were §26.3: `hitch` 8 → 2 at 1080p, 3 → 1 at 4K
H.264, worst gap 169.6 → 80ms. They were necessary and they were not sufficient.

**One caveat recorded rather than glossed:** this session did not establish
whether the test was taken at the machine or over Parsec. §25.2/CLAUDE.md now
hold that subjective feel judgements are not valid over Parsec, because it
re-times and re-encodes the screen. The sign-off is recorded as given; if it was
a remote session it should be re-taken at the panel before being leaned on
against any future regression.

---

## 27. Step 8 — texture and upload-resource reuse. ANSWERED: NO (2026-08-10)

Taken up under an explicit owner condition: state what step 8 is expected to buy
and how it will be known, before implementing it; if it measures as noise, say
so and move to step 9 rather than shipping an optimisation with no number behind
it. That condition is the entire reason this section exists rather than a commit.

**Session conditions**, since §26.1 is the reason to state them: physical panel,
`5120x1440 @ 239999/1000 = 239.999Hz`, confirmed with `refresh.ps1`. Not a Parsec
session. `d3d11` default, `win 1280x829`/`1280x815`.

### 27.1 What the section it came from actually claimed

§22.7 item 2: *"Textures are recreated on any geometry change — step 8 is where
that and a staging-buffer upload belong. Today every plane is `Map/WRITE_DISCARD`
per frame."*

Both halves are true as written and neither is a cost. "Any geometry change"
means the **frame's** geometry, not the window's: `releaseSizeDependent()` drops
the render target view only, and the textures are untouched by a resize. And
`Map/WRITE_DISCARD` per frame is not an allocation — it is the upload.

The code already does what step 8 proposes. `ensureTexture` early-returns when
size matches; `ensurePlaneTextures` early-returns when the format and all three
plane sizes match; and `clearFrame()` deliberately **keeps** the plane textures,
with a comment saying why — it runs on every media change and between drag and
landing, and rebuilding three textures each time is the allocation that path
exists to avoid. The header has claimed this since GATE B ("built lazily and only
when the size changes, so a steady stream of frames at one resolution reuses
everything"). Nothing had ever checked it.

### 27.2 The measurement

`tex` is cumulative `CreateTexture2D` calls since launch; `upload` is the CPU→GPU
transfer for one frame. Both new in `e88d002`.

| run | frames / paints | `tex` | `upload` last/avg | budget |
|---|---|---|---|---|
| 4K H.264 playback, `win 1280x829` | 120 | **3** | 0.59 / **0.76ms** | 41.67ms |
| ProRes 4444 playback, `win 1280x815`, no audio | 261 | **3** | 3.44 / **3.47ms** | 41.67ms |
| 4K H.264 reversal drag, `win 1280x829` | 406 paints | **4** | 0.73 / 0.13ms | — |
| ProRes 4444 playback, **`cpu` control** | 261 | **0** | 0.00 / 0.00 | 41.67ms |

**Three is the three planes. Four is the three planes plus the one BGRA texture
the previews share.** Nothing is ever recreated: not across 261 frames of
playback, and not across a multi-gesture reversal drag that alternates BGRA
previews and full-resolution planar frames 406 times. **There is no texture churn
to remove, so the "reuse textures" half of step 8 has nothing to reuse.**

The drag row is the one that could have gone the other way, and it is the reason
it was run: previews are BGRA at display size and landings are planar at full
resolution, so a naive implementation would tear one set down to build the other
on every transition. `clearFrame()`'s decision not to is what makes it 4.

### 27.3 The residual is memory bandwidth, and no API change reaches it

4444 is the worst case and the only file where the upload is visible at all:
**3.47ms of a 41.67ms budget, 8.3%.** Its planes are 3 × 4096 × 2304 × 2 bytes =
**56.6MB**, so 56.6MB in 3.47ms is **16.3 GB/s** — a single-threaded CPU write
into the driver's mapped region, at bandwidth. It is not allocation, not API
overhead, and not a texture lifetime.

The two mechanisms step 8 names both fail against that:

- **Texture reuse** is already complete (§27.2). It could not have helped anyway:
  the copy happens whether the texture is new or reused.
- **A staging-buffer upload** is strictly more work — the same 56.6MB CPU memcpy
  into a staging resource, *plus* a GPU-side `CopySubresourceRegion` of another
  56.6MB. Its usual justification is that a `DYNAMIC` texture lives in system
  memory so the shader samples it across PCIe, while a `DEFAULT` texture is in
  VRAM. That justification requires the draw to be the constraint, and the draw
  reads **`draw 0.01ms`, `paint tot 0.13ms`**. There is nothing there to win.

The only remaining micro-optimisation is real and is quantified rather than
dismissed: `uploadPlanes` loops row by row unconditionally, while `uploadPixels`
has a whole-buffer fast path for the case where source stride, destination
`RowPitch` and row bytes all agree. Adding the same fast path to the planar side
would remove ~2300 `memcpy` call overheads per 4444 frame — on the order of
**35µs**, which is **1% of the upload and 0.08% of the frame.** That is noise by
any definition, and shipping it would be exactly the optimisation-with-no-number
the owner's condition rules out.

### 27.4 The conclusion, and the one thing worth keeping

**Step 8 is closed as answered-no.** It is the second deferred item in two
sessions whose premise had expired by the time it came up — §26.2 was the first,
and the rule it produced applies verbatim: *a deferred item's premise expires;
re-derive it before building it.* Here the premise expired because GATE B's own
lazy-creation code already satisfied it and nobody had counted.

The telemetry stays, and it is the part with lasting value. It is also step 9's
"and telemetry" half, delivered early:

- `tex` is a **regression tripwire**. If a future change to the frame path makes
  textures churn — a preview size that moves per frame, a format that alternates,
  a `clearFrame()` that stops keeping the planes — this number starts climbing
  and says so. Before this commit that failure would have shown up only as an
  unexplained few milliseconds inside the tick handler.
- `upload` closed a real hole in `total`. `total` had never included it, so on
  the shipping default it under-reported every frame by up to 3.47ms. **A d3d11
  `total` from before `e88d002` is a smaller number for the same work.**

Charged for the upload it always had, d3d11 on 4444 still reads `total 24.63`
against `cpu`'s `total 32.89` in the same session — so GATE C's win survives
being measured honestly, which was worth confirming.

---

## 28. Step 9 — GPU scaling. A REAL defect, measured, fixed, SIGNED OFF (2026-08-10)

Unlike step 8 (§27), this one had something behind it. Same discipline applied:
state what it should buy, measure before building, measure after.

**Session conditions**: physical panel, 5120x1440 @ 239.999Hz, `refresh.ps1`
confirmed. Not Parsec. `win 1280x815` / `1280x829`, `display 640x360`.

### 28.1 §9's framing was right about the target and wrong about the scope

§9: *"What is genuinely left for the GPU here is the landing frame (Step mode),
which still converts full-res and lets Qt scale it — a 6.4x downscale in the
validation window. Measured local contrast between preview and landing is within
0.7%, so this is not currently a visible defect."*

Two corrections. **It is not just the landing** — every full-resolution frame is
drawn through the same sampler, so playback undersamples identically. And **the
0.7% was the wrong instrument**: preview and landing agreed because they were
equally undersampled, and local contrast is precisely the statistic aliasing can
preserve while moving the detail around.

### 28.2 The instrument, and why it does not compare Trace to Trace

`abfilter.ps1` places a capture on an axis between two references generated by
ffmpeg at the exact size Trace drew at — `area` (every source pixel contributes,
position 0) and `neighbor` (one source pixel, position 1) — using mean
|Laplacian| as the ordinate. High-frequency energy is the metric that identifies
aliasing rather than mere difference: undersampling converts detail it cannot
represent into spurious local contrast, so it reads *higher* than a correct
reduction at the same size.

**The references are external on purpose.** §20.3 spent a session on a
CPU-vs-D3D11 difference in which both sides were 2x2 taps; two of Trace's own
paths can be wrong together, and here they were.

**The axis is calibrated** against known scalers on the same frame, which is what
makes a reading on it mean something:

| scaler | position |
|---|---|
| `area` | 0.00 |
| `bicubic` | −0.01 |
| `lanczos` | 0.06 |
| `bilinear` | **−0.20** (swscale widens its filter when reducing; slightly softer than area) |
| `fast_bilinear` | **0.74** |
| `neighbor` | 1.00 |

Two supporting instruments were needed and neither existed. `croprect.ps1` cuts
the video rect out of a window capture and **asserts its size** against the HUD's
`display` field — a one-pixel crop error on a 6x reduction moves every sample onto
a different source neighbourhood and reads as a filtering difference, which is
§21.2 exactly. It took two wrong versions to get right, both caught by that
assertion rather than by inspection: a plain bounding box caught the title bar,
and so did runs-through-the-centre, because the title bar is a **full-width** band
of (32,32,32) so every column in the scan is lit. The fix is to find the stage
first, by the exact value (0,0,0) rather than a brightness floor — the left edge of
the test picture measures luma 33, between the chrome's 32 and the HUD's 18.
`previewshot.ps1` captures with the mouse button still **down**, because the
release is what lands a full-resolution frame.

`-Sensitivity` runs the reference pair alone and refuses material whose two
references agree. It rejected the 4K milk splash (0.22 mean delta) and the 60fps
drone plate; a test that cannot resolve anything would otherwise have passed
silently. 4444 (bubbles and fine label text, hf ratio 1.92x) and 422 HQ are the
usable sources, and 422 HQ is also the owner's declared quality bar.

### 28.3 The measurement: all three paths are `fast_bilinear`

| path | what reduces 6.4x | position |
|---|---|---|
| d3d11 landing **and playback** (planar) | D3D11 sampler, 2x2, no mips | **0.74** |
| cpu landing (BGRA) | Qt raster bilinear, 2x2 | **0.73** |
| drag preview (Scrub) | swscale `SWS_FAST_BILINEAR` | **0.76** |
| d3d11, 4K ProRes 422 HQ | sampler | **0.89** |

Three unrelated mechanisms landing on one number, because a 2x2 tap is a 2x2 tap.
The preview's value is not a coincidence at all: `swsFlagsFor(fast)` returns
`SWS_FAST_BILINEAR`, and that filter measures 0.74 on this very frame.

**This re-reads §20.3 and §21.2.** Their finding — CPU and D3D11 agree at integer
DPI and differ by a few percent at fractional DPI — stands. But their *agreement*
was never evidence that either was correct, and the owner's GATE B visual
sign-off (§20.2) was taken on that comparison. Nothing was hidden; the question
was not asked.

### 28.4 The fix

A box average over the destination pixel's footprint, in the pixel shader, in
**normalised** source coordinates. That last detail is what keeps GATE C's design
intact: a chroma plane is smaller, so the identical uv offset spans
proportionally the same area of it, and 4:2:0/4:2:2/4:4:4 continue to differ in
nothing but texture size. Per-plane texel steps would have reintroduced exactly
the subsampling special-casing §22.1 removed.

Taps per axis are `ceil(ratio/2)` — each tap is itself a bilinear 2x2, so N taps
reach 2N source texels and N need not reach the ratio — clamped to 4. The cap is
not a performance guess about this box (4x4 at 640x360 is ~11M fetches, nothing
for any GPU that runs this app); it is there because the backend also has to work
on **WARP**, which CI renames itself to and which still has to pass.

**Averaging happens before range normalisation and the matrix.** Both are affine,
so it is identical to transforming every tap and averaging after, and costs one
matrix multiply instead of sixteen. It would **not** be identical with a
non-linear step in between — if BT.2020 tonemapping ever arrives (§22.7 item 5),
this ordering has to be revisited. The shader says so.

### 28.5 Results

**Quality**, same frames, same references:

| file | before | after | delta vs `area` |
|---|---|---|---|
| 4K ProRes 4444, f120 | 0.74 | **0.02** | mean 1.32 max 46 → mean 0.47 **max 2** |
| 4K ProRes 422 HQ, f60 | 0.89 | **0.00** | mean 1.45 max 96 → mean 0.88 max 68 |

Max channel delta 2 against an external area reduction is the same order as
GATE C's colour verification (§22.2).

**Cost: none measurable.** This is what the draw being idle buys — `draw` was
0.01ms of a 41.67ms budget with the whole frame spare.

| run | real time | frames | doubled | `handler>budget` | max gap |
|---|---|---|---|---|---|
| 4444, `TRACE_NO_AUDIO=1` | 99.8%, 99.8% | 261/261 | 0, 0 | 0 of 260 | 44.3, 44.8ms (45.3 before) |
| 4K H.264 (audio) | 99.1%, 99.1% | 120/120 | 0, 0 | 0 of 120 | 83.2, 84.7ms (83.9 before) |

Scrub, 4444 reversals, two reps each: `hitch` **5, 9** with the reduction on
against **9, 7** off; release 5.1/4.1ms against 5.3/5.1ms; `ui gap` max 31.6/31.1
against 30.9/30.2. Overlapping ranges, no systematic difference — expected, since
4444's gaps are seek-and-decode (`rev-hit 20%`, 97 seeks) and the draw is not the
constraint anywhere. Both configurations bracket §26.3's recorded `hitch 5`.

**The control is exact, not approximate.** `taps == 1` collapses the shader loop
to one `Sample` at `input.uv`, so `TRACE_GPU_REDUCE=0` is bit-identical to the
previous shader — and it re-measures **0.74, delta mean 1.32, max 46**, the
pre-change figures to the digit. It is deliberately a separate knob from
`TRACE_PLANAR_UPLOAD`: the reduction lives in the YUV shader only, so without its
own control the planar-vs-BGRA A/B would differ in two ways at once, which is
§22.4a.

The HUD reports `display WxH filtered xN`. 4444 reads `x4` (ratio 6.4), 4K H.264
`x3` (6.0), and a drag preview still reads `1:1`.

### 28.6 Open after step 9

1. ~~**Owner visual sign-off. Fifth time.**~~ **PASSED — owner sign-off
   2026-08-10**, on the 4x-zoom before/after/reference comparison of 4444 frame
   120 at the shipping 6.4x downscale. Fifth time the project has needed the split
   between "every figure improved" and "the picture is right", and the fifth time
   only the owner could supply the second half. **Caveat, as with §26.6: the
   session did not establish whether it was viewed at the machine or over Parsec**,
   and §25.2 holds that a sharpness judgement over a re-encoded stream is not
   valid. The concern was raised before the sign-off was given and the sign-off
   was given anyway, so it stands as the owner's decision — but if it was remote,
   re-take it at the panel before leaning on it against a future regression.
2. ~~**The drag preview is still 0.76 and is now the odd one out.**~~ **ACCEPTED
   AS-IS — owner sign-off 2026-08-10.** Preview and landing used to match (§9's
   0.7%) and no longer do: the picture *sharpens* on release. The owner has
   accepted that behaviour, which is the direction of travel and is normal for a
   review tool — previews are previews.

   **What was signed off is the behaviour, NOT a mandate to change the flag.** The
   owner confirmed that reading explicitly when asked, and gave the reason, which
   is worth more than the decision because it generalises:

   > **Smooth, responsive scrubbing takes priority over matching final-frame
   > scaling quality during motion.**

   Treat that as a standing rule for the drag path, not a one-off ruling on one
   swscale flag. It resolves the whole class in advance: preview resolution,
   preview filtering, sampling stride, paint pacing — anything that would buy
   fidelity *during* a gesture with responsiveness. Fidelity is owed to the frame
   the user stops on. This is the same order of preference §15 already established
   when sampling was allowed to skip frames on all-intra media during an active
   drag and nowhere else; now it is stated as a principle rather than inferred from
   one exception.

   **The condition that reopens it is named**: the quality change on release
   becoming *visibly objectionable in normal use*. Not a measurement, not a
   threshold on the numbers below — an observation, by someone reviewing with it.
   Until then **no further measurement or implementation is wanted**, and this is
   not a deferred item awaiting work.

   For the record if it ever is: `swsFlagsFor(fast)` returns `SWS_FAST_BILINEAR`,
   and plain `SWS_BILINEAR` measures −0.20 on the same frame. Its cost is
   unmeasured and it is the dangerous kind — previews are the drag path, where
   §15.1 measured supply at 19% on 4444. Measure the shuttle rate first and put
   the trade to the owner; do not treat the entry above as pre-authorisation.
3. **The CPU backend is unchanged at 0.73.** `TRACE_RENDERER=cpu` is the escape
   hatch, so falling back to it now costs picture quality as well as GPU
   presentation. Worth stating in any instruction to try it.
4. **The BGRA d3d11 path is unchanged (`x1`).** It only carries full-resolution
   frames under `TRACE_PLANAR_UPLOAD=0` or for a format/matrix the planar path
   declines; previews and the placeholder are 1:1 and need nothing.
5. **Upscaling is deliberately untouched.** A box average of a magnified frame
   would blur pixels someone is inspecting, which is the opposite of the job. The
   guard is `fitted < content` on both axes.
6. **Step 10 (10-bit output) is unaffected and still deferred.** §9's warning
   holds: high-bit-depth *processing* has been done since GATE C; 10-bit *output*
   needs an `R10G10B10A2` swapchain and a display in 10-bit mode.
7. **The tap cap is a precaution, not a verified bound.** It is set at 4 because
   WARP has to run this backend, but CI's `--renderer-selftest=d3d11` deliberately
   never `show()`s and never draws a video frame, so **the loop has never executed
   on a software rasteriser**. The selftest confirms the shader *compiles* there
   (run 65 on `e2bf799` is green at step 12, `planar=1`, no fallback) and that is
   what it was built to catch — but nobody has measured a 4x4 reduction on WARP,
   and a machine that falls back to it would be the first to find out. If the
   selftest is ever extended to paint a frame, this is the reason to do it.
