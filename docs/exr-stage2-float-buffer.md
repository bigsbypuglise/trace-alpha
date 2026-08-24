# EXR stage 2, part 1: the float display buffer

Record of what was built, measured and found. 2026-08-24. Commits `28cca98`
(the float path) and `2da029e` (Copy Frame), on branch
`exr-stage0-dependencies`.

**This is the CHECKPOINT the owner asked for: the float `PixelLayout` lands and
video is proven unmoved, BEFORE the pass cycling is built on top of it.** What is
deliberately NOT here: `[` / `]` pass cycling, the `C` bypass binding, the pass
overlay, the View-menu pass list, and the root-versus-Beauty duplicate label.

---

## READ THIS FIRST: the display this was measured on

**This session ran over Parsec, on a 1920x1080 @ 59.999Hz virtual display, not
the physical panel.** `parsecd` was running and `QueryDisplayConfig` reports the
active path as 1920x1080 @ 59.999Hz; the RTX 4090's own panel was at 5120x1440
@ 59Hz.

The consequence is precise, and it cuts one way only:

- **NO ABSOLUTE NUMBER IN THIS DOCUMENT IS A PANEL BASELINE.** Not the cadence
  percentages, not the handler times, not the release latencies, not the memory
  figures. Every one of them was taken on the Parsec display; none of them may be
  compared against a figure recorded at the panel, and none may be quoted as a
  new baseline. The only claims here that survive the display are the ones made
  as a **difference between two binaries measured beside each other**.
- **The panel itself was also in the wrong mode: 5120x1440 @ 59Hz, not
  239.999Hz.** So a session that merely closes Parsec is still not on the
  configuration the records were taken on. **Check both before measuring:**
  `scripts/measure/refresh.ps1` for the active path, and
  `Get-CimInstance Win32_VideoController` for the physical adapter's mode —
  `refresh.ps1` describes the ACTIVE display and will not say the panel changed
  underneath it.
- **The control comparison is still decisive**, which is what the owner's
  constraint actually asked for ("video playback must be provably unmoved.
  Measure it, do not assert it"). A control binary was built from `34b039c` and
  run beside every leg on the same display in the same session, so the display
  is a constant and cannot explain a difference between the two.
- **Nothing subjective was judged here.** Smoothness and feel are not judgeable
  over Parsec and were not assessed.

**Outstanding: the full regression should be re-taken at the panel** before this
work is called finished. What is recorded below is the A/B, which is what the
checkpoint needed.

---

## The design decision, stated so it can be argued with

The owner's instruction was to make `PixelLayout` capable of float and use it
only on the EXR path, with video untouched, and to **stop and report if both
renderers could not carry a second layout without disturbing the measured present
path**.

**They do not have to, and that is the design rather than a dodge.**

```
loadExr  ->  RGBAF32 (scene-referred, unclamped)
                |
                |   ViewerWidget::setFrame  -- the display stage
                |     OCIO f32->uint8   when a transform is active
                |     display mapping   when it is not
                v
             BGRA8  ->  renderer  (exactly what it received before)
```

Float runs from the loader to the display stage and no further. Neither backend
ever receives a float frame:

- `qtFormatFor(RGBAF32)` returns `Format_Invalid`, so `VideoFrame::toQImage()`
  gives a null image and the CPU backend cannot draw one;
- the D3D11 backend's `setFrame` takes planar YUV or BGRA8 and calls
  `clearFrame()` for anything else.

`ViewerWidget::applyColorTransformToRenderer()` is the **single** function that
hands a frame to a backend, and it now guarantees a float source is always
converted before it gets there. So "the measured present path is unchanged" is a
property of one choke point, not a convention to be remembered at call sites —
and the renderers were not touched at all.

**What this does and does not buy.** It recovers the precision that was being
thrown away *before the colour transform*: the clip to 8 bits now happens at the
END of the chain, where the display imposes it, instead of inside `loadExr`
before OCIO ever saw the pixels. It does **not** make the output more than 8-bit.
10-bit output and HDR remain formally deferred behind their own two external
gates, and a float texture upload is still stage 5's problem.

**If the owner wants float all the way to the GPU, that is a different and
larger change** — a float `PixelLayout` in both backends, float textures, and the
transform in the shader — and it lands on the present path this checkpoint exists
to protect. It is not needed for correct ACEScg viewing on an 8-bit display.

---

## What was measured on the source, before any code was written

`scripts/measure/exrprobe` — a standalone probe built against the same pinned
vcpkg OIIO. It exists because the grouper had to be written against the channel
order **OpenImageIO actually presents**, not against EXR's alphabetical storage
order.

`icecream_passes0000.exr`, 1920x1080, 27 channels, half, DWAA, no
`chromaticities`, `alpha_channel = -1`:

```
[ 0] R                [ 3] Beauty.red        [ 6] Cryptomatte.red
[ 1] G                [ 4] Beauty.green      ...
[ 2] B                [ 5] Beauty.blue       [15] P.red  [16] P.green  [17] P.blue
```

Nine groups of three, **contiguous and already in R,G,B order** — OIIO reorders
them. That is what makes the fast read path below possible, and it is measured
rather than assumed.

Per-channel ranges that decide the design:

| channel | min | max | mean | above 1.0 |
|---|---|---|---|---|
| `R` | 0.01398 | 2.45508 | 0.98234 | **48.56%** |
| `G` | 0.00715 | 2.46094 | 0.91398 | **42.23%** |
| `B` | 0.00288 | 2.51953 | 1.08611 | **65.64%** |
| `P.red` | −44.25 | +44.25 | −0.00838 | 47.71% |
| `P.green` | −0.00000 | 33.25 | 11.09 | 70.77% |
| `P.blue` | −14.84 | 44.00 | 23.49 | 73.58% |

**Roughly half the beauty pass is above 1.0**, and `loadExr` clipped every one of
those samples before the colour stage existed. That is the whole justification
for the float buffer, and it is a number rather than a principle.

**Root RGB and Beauty are the same render written twice, and the difference is
compression, not content.** Measured channel against channel: mean absolute
difference 0.0035 / 0.0028 / 0.0048 on values whose mean is ~1.0, max 0.041 /
0.029 / 0.053, with only 5–7% of pixels bit-identical — which is exactly what
independent DWAA (lossy) compression of two identical channel sets produces.

---

## What was built

**`src/core/VideoFrame.{h,cpp}` — `PixelLayout::RGBAF32`.** Four 32-bit floats
per pixel, R,G,B,A, unclamped. Four components rather than three so one layout
serves every pass; the 25% that costs against RGB-only buys a single code path
through the transform, the mapping and the frame cache.

**`src/core/ExrChannels.{h,cpp}` — the grouper.** Case-insensitive on
`{R, red, G, green, B, blue, A, alpha}`, split at the **last** dot. Three naming
conventions live in one asset set and a grouper written for any one of them finds
nothing in the other two:

```
root layer      R / G / B
Redshift AOVs   Beauty.red / Beauty.green / Beauty.blue      lower-case words
Cryptomatte     CryptoMaterial.R / .G / .B / .A              upper-case, with A
```

Anything whose suffix is not in the set becomes its **own single-channel pass**
rather than being dropped, so no channel in the file is invisible. Classification
is by exact name match against small closed sets, never `contains()` —
"Specular" contains a *p* and "Reflections" contains an *n*, and a substring test
would send both through a data mapping.

**`src/core/DisplayMapping.{h,cpp}` — how a float pass is made visible**, chosen
per pass CLASS and never per pass name: `Gamma22` for colour, `Normalise` for
position and depth, `SignedUnit` for normals, `Raw` for data. The mapping, the
measured input range and the fraction above 1.0 are **always on the HUD**.

The gamma conversion is a **binary search over 255 output thresholds**, not
`powf` per sample. `powf` at three components over 1920x1080 is ~124ms, three
frame budgets; and a table indexed on the *input* cannot be used, because the
curve is near-vertical at the bottom — the linear values separating output levels
0, 1 and 2 all sit below 1.5e-5, which a 16-bit input table cannot resolve, and
that is precisely the deep shadow detail a scene-linear render is reviewed for.
The threshold search gives exactly the rounding `powf` would have produced.

**`src/core/ParallelBands.h`** — the row-band split, extracted from
`ColorTransform::apply` so the colour stage and the display mapping cannot drift
apart on it. Both are per-pixel, so a row band is exactly independent and no tile
seam is possible.

**`ColorTransform` gains a second CPU processor**, `f32 -> uint8`, built beside
the `uint8 -> uint8` one from the same `OCIO::Processor`. **Video keeps the 8-bit
processor byte for byte.** If the float processor fails to build it is not fatal
— video still works — but `hasFloatProcessor()` is what the EXR path asks, so it
cannot show an untransformed picture while the HUD claims a transform is on.

**`loadExr` reads only the pass being displayed.** When the pass's components are
contiguous and in order — measured true for every pass in the test set — OIIO
scatters straight into the RGBA float buffer through the pixel stride, with **no
intermediate allocation at all**. A scatter-by-name fallback covers everything
else.

**Three carried traps close with it.** Channel index 3 is no longer taken as
alpha regardless of what it is (it was `Beauty.red` on the 27-channel file).
Alpha is no longer pushed through the colour gamma. And the HUD's `ch:` field now
means the SOURCE's channel count — it read `4` on a 3-channel EXR because the
frame-handoff path hard-coded the display buffer's.

---

## TWO PRE-EXISTING DEFECTS FOUND BY MEASURING

### 1. EXR display had red and blue transposed. On every EXR, since stage 0.

`loadExr` filled a `QImage::Format_RGBA8888` — memory order R,G,B,A — by writing
`qRgba()` values through a `QRgb*`. `qRgba()` packs `0xAARRGGBB`, which on
little-endian is **B,G,R,A in memory**. So blue landed in the red slot.

It survived because the file it was developed against is a near-neutral cream
ice-cream render, where an R/B swap looks almost right.

**Proven, not argued.** Against the control build, on the same still:

| comparison | differing | max channel delta |
|---|---|---|
| new vs control, as captured | 107,330 of 212,544 (**50.50%**) | 100 |
| new vs control, **control's R and B swapped** | **0 of 829,184 (0.0000%)** | **0** |

An exact zero under exactly one channel permutation is the diagnosis. Sample
pixel: new `(153,140,119)`, control `(119,140,153)`.

**That same zero also proves the rewritten gamma path is bit-exact against the
old one** — the 255-threshold search reproduces `powf` + round on every pixel of
a real frame. One measurement, two results.

*(The first version of this comparison read 0.11% differing at max delta 116. The
offenders were at x < 16 — `GetWindowRect` includes Windows 11's invisible resize
border, so the first columns are whatever is behind Trace. The recorded
`transitions.ps1` trap, arriving in a new harness.)*

### 2. A sequence pattern is printf-shaped and `QString::arg` read it as its own placeholders.

`icecream_passes%04d.exr` printed on the HUD as `icecream_passes27d.exr`: `%04`
was substituted with the channel count, 27. Pre-existing, and visible on the
control build in this session's captures.

Both media HUD lines now put their text fields in **last, together**, through the
multi-arg overload, which substitutes in one pass and never rescans what it
inserted. That also covers a file name containing a percent sign.

---

## Copy Frame now copies what is on screen

Owner decision. **A deliberate behaviour change, and it belongs in the release
notes rather than only in a commit message.**

Measured on the Alexa clip, with its own negative control:

| | mean RGB | stddev |
|---|---|---|
| control, LUT ON | 108.93 / 110.35 / 113.66 | 7.69 |
| **new, LUT OFF** | **108.93 / 110.35 / 113.66** | **7.69** |
| new, LUT ON | 183.95 / 188.39 / 194.05 | 16.92 |

The middle row is the important one: with no transform loaded the clipboard is
**identical to the control to two decimals**, so the ordinary case is unmoved.
Full source resolution throughout — 4608x3164 on all three. On a 1920x1080 EXR
the clipboard reads 1920x1080, mean RGB 230.40/227.68/231.51 against the
on-screen picture band's 230.69/227.89/231.60.

**The view transform (rotate/flip) is a DIFFERENT SEAM and was not touched.** It
lives in the renderer — `ViewerWidget` hands it to the backend, which applies it
in the vertex shader's texture coordinate or in QPainter's matrix — so it is
downstream of `displayFrame_` and was never in this buffer. Phase 10's decision
stands untouched and needed no defending.

---

## Video is unmoved — measured against a control

Control built from `34b039c` in a worktree. **DLL sets made byte-identical by
hash** so the executable is the only variable, and **the two binaries proven
distinct by their own strings** (`Gamma 2.2`, `Signed unit`, `Normalise`,
`pass %1/%2` present in one and absent in the other) rather than by a hash alone.

| leg | new | control |
|---|---|---|
| 4K H.264 cadence x2 | **100.0 / 100.0%**, 120 frames, `drop 0`, `rephase 0`, buckets `~1x 119` | **100.0 / 100.0%**, same |
| 4K H.264 handler avg/max | 1.88/2.29, 1.77/2.32 | 1.81/2.26, 1.80/2.25 |
| 4444 cadence x2 | **99.8 / 99.8%**, 261 frames, `drop 0`, `rephase 0` | **99.8 / 99.8%**, same |
| 4444 handler avg/max | 18.97/19.48, 19.10/19.43 | 19.61/19.79, 19.56/19.60 |
| 4444 `-SnapRelease` | `target 261 shown 261 delta 0`, `dst YUV444P12 planar`, `release 21.0ms`, `hitch 0`, `land 0`, `kf-land 0` | same, `release 21.7ms` |
| `scrubbar.ps1` full pool | **PASS — 22 files, 88 legs, `delta 0` throughout** | — |

`xform none` on every cadence run: the colour stage is off, which is the shipping
default and the configuration every existing measurement in this repo was taken
in. GATE C intact on both binaries (`dst YUV444P12 planar`).

Selftests, new binary: `renderer=d3d11 fellback=0 planar=1` · `trace-ocio:
version=2.5.2 ... moved=1` · `OK - 11 shapes x 4 scale factors` ·
`verify_trace_assets --strict --no-pillow` exit 0 at **33 embedded files**.

The 4444 `<0.9x` bucket reads 2/1 new against 1/1 control, inside that file's
recorded 1–10 span. Do not chase it.

---

## What the EXR path costs

**Per float frame: `width x height x 16` bytes.**

| resolution | float frame | three-frame window cache |
|---|---|---|
| 1920x1080 | 33.2 MB | 99.5 MB |
| **3840x2160 (4K)** | **132.7 MB** | **398 MB** |
| **7680x4320 (8K)** | **530.8 MB** | **1.59 GB** |

The 4K and 8K rows are **arithmetic**, labelled as such: there is no 4K or 8K EXR
in the asset set. The 1080p row is corroborated by the measured working-set delta
below. `FrameCache` is a window cache of radius 1, so at most three frames are
held.

**The 8K row is worth saying out loud.** A 7680x4320 EXR sequence would want
~1.6 GB of frame cache before anything else. Nothing in the asset set exercises
it — `12_8K_ProRes4444` is a ProRes plate on the video path, which this change
does not touch at all — but if 8K EXR ever arrives, the window cache is the thing
to bound by bytes rather than by frame count.

**Measured, process working set on open:**

| file | new | control |
|---|---|---|
| 27-channel multilayer | 254.9 MB, **peak 339.6 MB** | 199.8 MB, **peak 523.3 MB** |
| 3-channel beauty-only | 254.1 MB, peak 274.2 MB | 200.3 MB, peak 237.1 MB |

**Peak on the pathological file falls 523.3 → 339.6 MB**, because the 224 MB
whole-file read is gone. Resident cost is +55 MB, which is the three-frame window
holding float instead of 8-bit.

**Read cost, standalone** (`exrprobe --read`, best of 5, on the 27-channel DWAA
file): all 27 channels **75.40 ms / 213.6 MB**; the root RGB pass **37.98 ms /
23.7 MB**; the `P` pass **41.40 ms / 23.7 MB**. So reading one pass is about half
the time and a ninth of the memory — not a ninth of the time, because DWAA
decodes in blocks and there is fixed overhead.

**Sequence playback is unchanged**, and honestly so: on the 27-channel DWAA
sequence over a 2.5s window the new build reaches frame 61 and 63 against the
control's 65 and 63, and the 217-frame PIZ sequence plays through to `216/216` on
both. Both keep real time at 1080p, because prefetching hides the decode — **the
channel-reading fix shows up as peak memory, not as frame rate, on this
material.** The sequence path exposes no cadence counters, so no rate figure is
claimed.

---

## What is NOT done, and is next

1. **`[` and `]` pass cycling, and the `C` bypass binding.** To be decided and
   tested **together as one keyboard surface**, against phase 7's text-field
   guard (Qt's `ShortcutOverride` through `QLineEdit`, printable keys only) and
   against `barekeys.ps1`'s menu-bar-focus case. Note `[` and `]` are not
   letters, so the menu-bar mnemonic path does not apply to them and the
   `QLineEdit` guard does — both need testing rather than reasoning.
2. **The pass overlay and the View-menu pass list.**
3. **`ExrPass::duplicateOf` is declared and never filled.** The mechanism is
   settled and cheap — read a band of scanlines from both channel ranges at open
   and compare, which is a few hundred KB rather than a whole frame — but it only
   becomes visible once the pass list exists, so it lands with it. **It must land
   with it**: an unfilled field that the HUD would print is exactly the kind of
   thing that quietly never gets done.
4. **The full regression at the physical panel.**

One thing found in passing and not acted on: `R2_OP_Stacks_01_00000.exr` carries
`framesPerSecond = 24/1` and `smpte:TimeCode = 00:00:00:00` in its header, and
Trace reads neither — image sequences still get the nominal 24fps and no source
timecode. Recorded, not built.
