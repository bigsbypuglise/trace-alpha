> # SUPERSEDED — DO NOT IMPLEMENT
>
> This is the **OpenGL / QOpenGLWidget** conversion spec. It is kept only for its
> colorimetry and bit-depth reference tables, which have been ported into
> `docs/gpu-initiative-plan.md`. The active design is **native D3D11 + DXGI**.
>
> Its premises are also stale as of 2026-08-07:
>
> - "SmoothPixmapTransform off ... nearest-neighbour rescale on every paint" —
>   fixed; filtering is on whenever the frame is resampled and off at 1:1.
> - "4K ProRes 4444 measures dec ~13ms + cvt ~25ms" — that figure was dominated by
>   the `QImage::bits()` detach bug, since fixed. Measured now: 261/261 frames,
>   98.3% of real time.
> - "Playback smoothness is a throughput problem" — **no longer true.** Every
>   validated file now holds 98-99% of real time.
>
> Read `docs/gpu-initiative-plan.md` instead.

# GPU color conversion — implementation spec

**Status:** proposed, not started
**Goal:** move YUV→RGB conversion and scaling off the CPU and into a fragment
shader, so playback holds rate on 4K ProRes 4444 and full-res frames become
affordable during scrub.

## Scope

**In scope (this step):**

- `ViewerWidget` becomes a `QOpenGLWidget`
- Decoder hands YUV planes to the viewer instead of swscale'd BGRA
- YUV→RGB, bit-depth normalization, and fit-to-window scaling all happen in the shader
- Env kill switch to fall back to the current CPU path

**Explicitly NOT in this step:**

- **No async decode thread.** That is the next step and it must land separately.
  Mixing them makes a revert impossible to scope, and frame ordering is the
  invariant that killed the previous two attempts.
- **No hardware decode (D3D11VA/NVDEC).** Different change, much smaller payoff
  for these formats: ProRes has no hardware decode path at all, and for H.264 the
  GPU→CPU readback can eat the win unless frames stay on the GPU end to end.
- **No OCIO.** This makes OCIO possible later; it isn't OCIO.

## Why this first

Playback smoothness is a throughput problem — 4K ProRes 4444 measures dec ~13ms +
cvt ~25ms against a 41.7ms budget. Conversion is the larger half and it is pure
CPU waste: the GPU does this work essentially for free.

It also shrinks the async work that follows. Once conversion is off the CPU, a
decode thread only has to hide decode, and the cross-thread handoff becomes an
`av_frame_ref` refcount bump instead of shipping a full RGB buffer. Smaller
surface area, easier to get ordering right.

**Note that this does not fix scrubbing.** At 1080p H.264 conversion is already
~0.7ms — it is not what makes the slider feel clunky. Scrub feel is a latency and
scheduling problem (seek cost, GOP walk, and the inability to abandon stale
requests) and it gets fixed in the two steps after this one. Don't expect this
change to deliver it.

## Current state (as of 1f8c1d8)

- `ViewerWidget` is a plain `QWidget`; `paintEvent` does `QPainter::drawImage`
  of a `QImage::Format_RGB32` into a fitted rect, `SmoothPixmapTransform` off —
  so the CPU also does a nearest-neighbour rescale on every paint.
- `VideoDecoderFFmpeg` swscales to `AV_PIX_FMT_BGRA` on the UI thread.
- `ImageSequenceFrameSource` / `StillImageLoader` produce `QImage` directly.

## Design

### Viewer

`ViewerWidget` inherits `QOpenGLWidget` and `QOpenGLFunctions_3_3_Core`, and
keeps **two** input paths:

```cpp
void setImage(const QImage& image);      // stills / image sequences — unchanged behaviour
void setVideoFrame(const AVFrame* frame); // new — planar YUV upload
```

Image sequences still go through the QImage path. Don't unify them in this step.

`setVideoFrame` uploads planes and triggers a repaint. **Upload synchronously
inside the call, then return** — the caller keeps owning the frame and no
lifetime tracking is needed. Do not try to defer upload to `paintGL` in this step.

### Textures

One texture per plane. Y, U, V only — **never sample plane 3**, which makes the
existing `alphaStrippedFormat()` workaround unnecessary for the GPU path.

| Source format | Codec | Planes | Texture format | Shader scale |
|---|---|---|---|---|
| `yuv420p` | H.264 8-bit | Y, U/2, V/2 | `GL_R8` | 1.0 |
| `yuv422p10le` | ProRes 422 | Y, U/2 w, V/2 w | `GL_R16` | 65535/1023 |
| `yuva444p12le` | ProRes 4444 | Y, U, V (+A ignored) | `GL_R16` | 65535/4095 |

`GL_R16` normalizes to [0,1] by dividing by 65535, but the stored value occupies
only the low 10 or 12 bits — hence the scale factor in the shader.

**Stride handling is the classic bug here.** `AVFrame::linesize[i]` is not
`width`. Set `GL_UNPACK_ROW_LENGTH` to `linesize[i] / bytesPerSample` before each
`glTexSubImage2D`, and reset it to 0 afterwards. Getting this wrong produces a
sheared or diagonally-skewed image — if that's what you see, this is why.

Reuse textures across frames; only reallocate when dimensions or pixel format
change. Per-frame `glTexImage2D` will undo much of the win.

### Colorspace — the part that must be right

**This is the highest-risk part of the change.** Trace is a review tool; wrong
colour is a worse failure than slow playback. Do not hardcode a matrix.

Read `AVFrame::colorspace` and `AVFrame::color_range`:

- `AVCOL_SPC_BT709` → BT.709 coefficients
- `AVCOL_SPC_BT470BG` / `AVCOL_SPC_SMPTE170M` → BT.601
- `AVCOL_SPC_UNSPECIFIED` → fall back on height: `>= 720` is BT.709, else BT.601
- `AVCOL_RANGE_JPEG` → full range; anything else → limited range

Normalization before the matrix:

```
limited:  y = (Y - 16/255) / (219/255)      c = (C - 128/255) / (224/255)
full:     y =  Y                            c =  C - 128/255
```

Matrices (`u` = Cb, `v` = Cr, both centred at 0):

```
BT.709   R = y + 1.5748*v
         G = y - 0.1873*u - 0.4681*v
         B = y + 1.8556*u

BT.601   R = y + 1.4020*v
         G = y - 0.344136*u - 0.714136*v
         B = y + 1.7720*u
```

Pass the 3x3 matrix and the two normalization terms as uniforms rather than
compiling shader variants per colorspace.

### Scaling

Fit-to-window is now a GPU sampler operation — draw a quad sized to the fitted
rect and let the sampler do the work. Use `GL_LINEAR`. This replaces the CPU
rescale in `paintEvent` and looks better than the current nearest-neighbour.

### Kill switch

Follow the existing env-var convention (`TRACE_PERF_FAST_CONVERT`,
`TRACE_KEEP_ALPHA`):

```
TRACE_GPU_CONVERT=0    # force the old CPU swscale path
```

This matters more than usual — if a specific file renders with wrong colour, this
turns a blocker into a note. Keep the CPU path compiled and working; do not
delete it in this change.

## Build changes

`QOpenGLWidget` lives in a **separate Qt6 module**. This is the most likely
first-failure point:

```cmake
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)
target_link_libraries(Trace PRIVATE Qt6::OpenGL Qt6::OpenGLWidgets)
```

Request a desktop GL 3.3 core context via `QSurfaceFormat::setDefaultFormat()`
**before** the `QApplication` is constructed, in `main.cpp`.

`windeployqt` will pick up `Qt6OpenGL.dll` and `Qt6OpenGLWidgets.dll`
automatically. Consider adding them to the CI "verify package is launchable"
required-file list so a missing GL DLL fails the build rather than the app.

## Cleanups this unlocks

Do these **after** the GPU path is validated, not alongside it:

- Half-res scrub previews at ≥1920px — full res should now be affordable
- `alphaStrippedFormat()` — the shader simply never samples plane 3
- Mode-aware sws conversion quality (`SWS_FULL_CHR_H_INT|SWS_ACCURATE_RND` for
  Step) — irrelevant once the GPU does the conversion

## Validation on Windows

Build Release. Compare each item against a `TRACE_GPU_CONVERT=0` run of the same
build — that's the controlled A/B.

**Colour (do this first — it gates everything else):**

1. 1080p H.264, GPU vs CPU path, same frame, screenshot both. They should be
   visually identical. Any shift in black level or saturation means the range or
   matrix selection is wrong.
2. Repeat for ProRes 422 and ProRes 4444.
3. Check a frame with a hard black and a hard white. Blacks lifted or whites
   clipped = limited/full range confusion.

**Performance:**

4. 4K ProRes 4444: HUD `cvt` should drop to near zero. `dec` should be unchanged
   (~13ms). Playback should now hold 24fps.
5. HUD `draw` (paint time) should stay low — if paint time absorbed the
   conversion cost, the upload is happening in the wrong place.
6. 1080p H.264 scrub: expect **little or no improvement**. That's correct and
   expected; scrub is fixed in the following steps.

**Correctness:**

7. Frame stepping forward and back still lands on the right frames.
8. Image sequences and stills still display (the QImage path must be untouched).
9. Window resize, fullscreen toggle, and letterboxing still behave.

## Rollback

The change is contained to `ViewerWidget`, the decoder's display output path, and
`CMakeLists.txt`. With the CPU path still compiled in, `TRACE_GPU_CONVERT=0` is
the instant mitigation; a straight revert of the commit is the full one.
