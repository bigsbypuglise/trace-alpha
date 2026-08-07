> **Role of this document:** requirements and phase definitions for the GPU
> initiative, as authored. **Decisions already made live in
> `docs/gpu-initiative-plan.md`** (Gate A), which takes precedence where the two
> differ — see in particular the commit ordering and the "why async comes after
> GPU conversion" argument in Phase 12, which Gate A revisits.
>
> The `docs/gpu-conversion-spec.md` this file says to delete is archived at
> `docs/archive/gpu-conversion-spec-SUPERSEDED.md`.

# Trace GPU / smooth-presentation initiative

**Status:** proposed, not started.
**Supersedes:** `docs/gpu-conversion-spec.md` — written against a stale checkout, delete it.

This is a major architectural pass. Work carefully, measure every stage, and protect the
validated CPU playback path.

> **Note on phase numbering:** the architecture audit now runs *before* baseline capture.
> The audit is pure code reading and needs nobody at the keyboard; baseline capture needs
> Anj launching the app against eight media files. Running the audit first means it
> produces the exact metric checklist for the baseline session instead of blocking on it.

---

## Why we are doing this

User feedback is consistent: Trace is functional and accurate, but it does not feel as
buttery as QuickTime.

Many CPU-side bottlenecks are already gone. On an easy local 1080p H.264 file Trace spends
roughly decode ~0.1ms, conversion <1ms, paint <1ms against a ~41.7ms budget. **Raw CPU
throughput does not explain the remaining perceived difference.**

What's left:

1. Presentation cadence
2. CPU/QImage presentation architecture
3. Scaling quality
4. Exact fractional-frame-rate scheduling
5. Synchronous random-access decode during aggressive scrubbing
6. Obsolete scrub requests staying expensive after the mouse has moved on

GPU integration solves the presentation side. A later async decode / stale-request-dropping
stage solves the scrub scheduling side. **Both are required.**

## Core principle

We are not adding GPU support to increase GPU utilization. We are adding it to make Trace
smoother, more responsive, better at scaling, more predictable in cadence, positioned for
10-bit/HDR, ready for future hardware decode, and capable of eventually matching
QuickTime-style interaction quality.

The existing CPU/QImage renderer is **reference, fallback, and A/B control.** It stays
selectable and compiled. Do not delete it.

## Validated baseline — do not destabilize

Deterministic frame stepping · frame-exact scrub landing · H.264 EOF decoder drain ·
random-access frame cache · swscale context cache · source colour-matrix/range handling ·
filtered fit-to-window scaling · audio and A/V sync · responsive LucidLink / high-latency
I/O · buffering state · developer performance HUD · timeline absolute click-to-seek ·
latest-target overwrite in the existing scrub timer · local-file playback performance.

Do not rewrite unrelated systems merely because the renderer is changing.

## Architectural direction

Windows-first. First backend: **Direct3D 11**. Do not begin with CUDA, NVDEC, D3D12,
Vulkan, or a Qt UI rewrite.

### Native D3D11 vs QRhi — the requirements are not neutral

**Option A — QRhiWidget / Qt RHI on D3D11.** Easier Qt integration, Qt owns the
presentation plumbing, less HWND/swapchain work, easier future cross-platform. But it
abstracts the DXGI presentation layer, makes waitable-swapchain and frame-latency control
hard or inaccessible, and may foreclose the presentation-clock work.

**Option B — native D3D11 + DXGI surface hosted in the Qt Widgets app.** Explicit swapchain
and vsync control, DXGI frame-latency APIs, waitable swapchain path, stronger HDR/10-bit
control, better foundation for exact fractional presentation timing. Costs more
implementation work and HWND/resize/device-lifetime responsibility.

Because exact presentation cadence is a verified product goal, strongly evaluate whether
native D3D11/DXGI should be the long-term architecture. **Do not choose QRhiWidget only
because it is easier if it creates a dead end for the presentation-clock work.**

---

## Phase 1 — Architecture audit

Pure code reading. No implementation. Report:

1. Exact `AVFrame` → displayed-pixel path
2. Where swscale creates BGRA
3. Where QImage ownership begins
4. `ViewerWidget` paint path
5. QImage lifetime
6. Decoder / playback / UI / audio thread ownership
7. Frame cache representation
8. Interfaces that currently require QImage
9. Minimum frame abstraction supporting both CPU and GPU representations
10. Qt window embedding requirements
11. Resize / fullscreen behaviour
12. Renderer-device lifetime
13. Device-loss fallback requirements
14. **Frame representation for the cache under the GPU path** — see below

### 14 is not cosmetic

Nothing in this plan currently decides whether cached frames stay CPU-side, become GPU
textures, or hold `AVFrame` references. That decision has consequences:

- The reverse cache is already **sized by cost**, not frame count — a ~192MB footprint
  budget giving ~7 frames at 4K and ~32 at 1080p. If cached frames become GPU-resident,
  that budget is against VRAM, not system RAM, and the sizing maths changes completely.
- It collides with async scrub (Phase 12): stale-request dropping needs an unambiguous
  ownership story for in-flight frames. "Who owns this texture and when is it safe to
  recycle" is precisely the kind of unanswered question that turns attempt three into
  another revert.

Recommend the representation and how it interacts with the existing cost-based sizing.

**Also produce the exact metric checklist for Phase 2** so the baseline session is one
focused pass rather than improvised.

## Phase 2 — CPU reference baseline

Before any GPU implementation. This is the immutable comparison baseline.

Files: 1080p H.264 · 4K H.264 · 4K ProRes 422 HQ · 4K ProRes 4444 · vertical 1080×1920 ·
heavy fit-to-window downscale · local media · LucidLink warm media if convenient.

Record: decode ms · swscale/conversion ms · prep/handoff ms · paint ms · actual fps ·
target fps · cadence jitter · present-late · audio repeat/skip corrections · CPU usage ·
memory · scrub latency · cold random scrub latency · cache-hit scrub latency.

## Phase 3 — Minimal renderer abstraction

Only the minimum needed to support `CPU_QIMAGE` and `D3D11_GPU`. Conceptually
`VideoRenderer` → `CPUImageRenderer` / `D3D11Renderer` if it fits the codebase.

Playback logic must not know D3D implementation details. Frame identity must be completely
independent of renderer choice. Preserve stepping, scrub landing, cache behaviour, seek
behaviour, EOF behaviour, audio timing.

## Phase 4 — Minimal native D3D11 prototype

One pixel format: `yuv420p` H.264, 1080p first.

Persistent device, context, and shader resources · texture reuse · **no GPU object creation
per frame** · resize-safe · correct aspect ratio · letterbox/pillarbox · clean shutdown ·
CPU fallback. Unsupported formats fall back to `CPU_QIMAGE` automatically.

### Stride handling — read this before debugging the first image

`AVFrame` and D3D textures have independent row strides. Source is `AVFrame::linesize[i]`;
destination is `D3D11_MAPPED_SUBRESOURCE::RowPitch`. **Neither may be assumed to equal
`width * bytesPerPixel`.** Copy row by row using both pitches.

If the first GPU image appears diagonally sheared, slanted, offset per row, or corrupted
near edges — check stride first.

## Phase 5 — Native YUV plane upload

Avoid `AVFrame → swscale BGRA → QImage → GPU upload`. Instead: native Y/U/V planes → GPU
textures → shader → RGB target.

Measure CPU prep, upload, GPU conversion, present. **Do not claim improvement based only on
lower swscale timing if texture upload becomes equally expensive.**

## Phase 6 — GPU colour conversion

Use existing Trace colour metadata. Support BT.601, BT.709, BT.2020, limited range, full
range. **Do not assume BT.709 silently.** Pass matrix and normalization parameters as shader
constants rather than compiling permutations where practical. CPU and GPU output must
visually match.

## Phase 7 — GPU scaling / image quality

Viewport scaling on the GPU. A user-visible goal in its own right.

Test 1:1 · mild downscale · >2× downscale · 4K into a small viewer · vertical 1080×1920 into
a landscape viewer · baked-in text · diagonals · thin lines · moving fine detail.

At 1:1 preserve source pixels, no unnecessary softening. At reduced size minimize stair
stepping and shimmer/crawl, preserve readable baked-in typography. Start with good linear
GPU sampling; no exotic reconstruction filters yet.

## Phase 8 — A/B renderer switch

```
TRACE_RENDERER=cpu
TRACE_RENDERER=d3d11
```

Easily comparable on the same source file. GPU failure falls back to CPU automatically.

## Phase 9 — GPU telemetry

HUD reports: renderer · CPU decode ms · CPU prep ms · upload ms · GPU draw ms · present ms ·
total frame cost · upload bytes/frame · texture reallocations · presented frames ·
late/dropped · actual fps · target fps · A/V sync · renderer fallback count.

**Do not label CPU submission duration as GPU execution time.** Use D3D timestamp queries
for true GPU timings; if unavailable, label the measurement honestly.

## Phase 10 — Image fidelity validation

CPU vs GPU on identical frames: skin tones · saturated colours · black level · white level ·
baked-in text · diagonals · fine detail · full-range vs limited-range · tagged BT.709 ·
untagged/inferred source · 1:1 · fit-to-window · heavy downscale.

**Do not reintroduce the previous BT.601-default bug.**

## Phase 11 — ProRes GPU presentation

After `yuv420p` works. Priority: `yuv422p10le` (ProRes 422), then `yuva444p12le` (4444).

CPU ProRes decode → native high-bit-depth planes → GPU upload → GPU colour conversion → GPU
presentation. Avoid full-resolution CPU BGRA conversion where practical. Preserve bit depth;
do not silently truncate.

If using 16-bit normalized textures, document the exact sample mapping. **Verify whether
FFmpeg's samples are LSB- or MSB-aligned before applying normalization** — do not blindly
assume `65535/1023` or `65535/4095`. (FFmpeg's `p10le`/`p12le` are LSB-aligned, so those
factors should be correct, but confirm rather than inherit the assumption.)

ProRes 4444 alpha: preserve where required; if playback output is opaque the shader may
intentionally ignore alpha. Document the behaviour. **Do not automatically port CPU-only
alpha-strip workarounds to the GPU path.**

## Phase 12 — Critical scrub architecture pass

Essential to the "buttery" goal. **GPU presentation alone will not fix synchronous scrub
decode.**

Current scrubbing is constrained because long-GOP random access requires seek → decode many
GOP frames → finish current request → only then service a newer position. QuickTime-like
interaction requires **newest target wins**; old scrub work must be allowed to become
irrelevant.

This was attempted and reverted before: `a171e3a`, `1d280eb`, reverted by `9cd2a0c`,
`a2f7999`. **Read those commits first. Document exactly why they failed before designing a
third attempt. Do not repeat the same architecture.**

### Why async comes after GPU conversion

Once CPU BGRA conversion is gone, the decoder worker can hand off an `AVFrame` reference or
native plane-backed frame instead of a large converted QImage. Smaller cross-thread payload
makes stale-result dropping safer.

### Requirements

Every scrub request gets a monotonically increasing sequence ID:

```
request 101 -> frame 20
request 102 -> frame 40
request 103 -> frame 85
request 104 -> frame 140
```

If 101 finishes after 104 exists, 101 is stale — discard it, never present it.

Invariants: stale result never reaches the viewer · out-of-order frame never reaches the
viewer · the exact mouse-release request always wins · stepping remains deterministic ·
playback remains ordered · reopening media invalidates all prior requests · renderer/device
changes invalidate stale GPU frames safely.

### Scrub UX policy — explicit product decision

**A.** Exact every preview frame, or **B.** approximate during drag, exact on release.

QuickTime is closer to B, and Trace already accepts a related compromise via reduced-
resolution scrub previews. Recommended behaviour to test:

- **Active drag:** newest target wins · stale work discarded · nearest cheap useful preview
  acceptable · UI playhead follows the mouse immediately
- **Release:** exact target frame · full quality · frame counter exact · stepping starts from
  the exact target

Do not silently make Trace approximate everywhere. Trustworthiness remains a core pillar.

## Phase 13 — Presentation / vsync

The architectural payoff of native D3D11/DXGI. Test 23.976 · 24.000 · 25 · 29.97 · 30 ·
59.94.

Investigate DXGI swapchain · vsync · frame latency · waitable swapchain · display refresh
rate · presentation timestamps · exact fractional source cadence.

The current CPU/QTimer path is not display-synchronized. GPU presentation should eventually
be driven by a real display/presentation clock. **Do not redesign audio-master behaviour
until GPU presentation metrics exist.**

## Phase 14 — NVIDIA hardware decode (deferred)

Not in this pass. Later evaluate for H.264, HEVC, AV1 via D3D11VA or NVDEC. Keep software
fallback. Do not assume GPU decode helps ProRes — ProRes stays CPU-decoded unless supported
hardware decode is proven on the target system.

---

## LucidLink / remote storage

Do not mix remote-I/O work into the GPU branch. The validated responsive-I/O fix stays
separate. Read-ahead remains experimental and unproven. GPU work must not regress remote
source responsiveness.

## Commit strategy

Small, independently reviewable:

1. `refactor(renderer): introduce CPU/GPU renderer boundary`
2. `feat(gpu): add experimental native D3D11 video surface`
3. `feat(gpu): add planar YUV upload and shader conversion`
4. `perf(gpu): reuse textures and upload resources`
5. `perf(gpu): add GPU scaling and telemetry`
6. `feat(gpu): add high-bit-depth ProRes presentation`
7. `refactor(scrub): add sequence-numbered async decode requests`
8. `perf(scrub): drop stale random-access decode results`
9. `perf(gpu): add DXGI presentation timing`

No giant GPU commit.

## Decision gates

Do not blindly execute all phases.

- **Gate A — after the audit.** Report native D3D11 vs QRhi recommendation, renderer
  boundary, frame/cache representation, risks, expected files changed. **If the architecture
  requires a major playback-core rewrite, stop and report.**
- **Gate B — after the first D3D11 image.** Verify correct frame, stride, aspect, CPU
  fallback.
- **Gate C — after shader conversion.** Verify CPU/GPU colour match and measurable reduction
  in CPU conversion work.
- **Gate D — before async scrub.** Read the previous failed implementations and explain why
  the new design avoids those failures.
- **Gate E — before promoting the GPU renderer.** Full CPU vs GPU benchmark and image-quality
  validation.

## Success criteria

Trace switches CPU ↔ D3D11 cleanly · preserves exact frame identity · matches CPU colour ·
improves scaling · reduces CPU presentation/conversion work · presents more consistently ·
supports high-bit-depth ProRes correctly · remains frame-exact on release · drops stale scrub
work safely · feels materially smoother during rapid navigation · preserves CPU fallback ·
preserves LucidLink responsiveness · preserves audio behaviour.

**Do not call GPU integration complete because a texture renders.** The actual product test:

> Does Trace finally feel substantially closer to QuickTime during playback **and** rapid
> back-and-forth navigation?
