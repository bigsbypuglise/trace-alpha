# Stage 1: the OCIO / Color Transform foundation

> **SUPERSEDED IN PART, 2026-08-24.** This is the stage 1 record and its "not
> started" list describes 2026-08-23, not today. Stage 2 is DONE (records
> `docs/exr-stage2-float-buffer.md`, `docs/exr-stage2-keyboard-surface.md`,
> `docs/exr-stage2-pass-model.md`) and **Cryptomatte is CUT BY THE OWNER, not
> deferred**. Everything below about the colour transform itself still stands.


Record of stage 1 (2026-08-23). Stage 0 is `docs/exr-stage0-dependencies.md`; the
assessment both implement is `docs/exr-ocio-plan.md`.

> **ACCEPTED BY THE OWNER, 2026-08-23, and the session closed here.** Read the
> acceptance at its stated width: what was accepted is **the OCIO-backed display
> transform stage, its View menu, LUT loading and the `--ocio-selftest`** -- on
> the LUT-on-display-referred-video workflow it was measured on. It is **NOT** an
> acceptance of a scene-linear ACEScg pipeline, which the 8-bit limit in section 4
> says this stage cannot serve; **NOT** of the transform's cost on the 8K plate,
> which is unmeasured; and **NOT** of a `C` shortcut, which was deliberately left
> unbound. The next session starts at **stage 2, multilayer EXR / channel
> grouping**, from the carry-forward at the end of this document.

**Not started, by instruction:** multilayer/AOV cycling, Cryptomatte, EXR channel
regrouping, the `Color Transform...` dialog, the GPU stage. **Unmoved:** the vcpkg
pin, decode and playback scheduling.

## 1. Architecture

**One stage. It is OCIO from the first commit, and a LUT is a configuration of
it rather than a parallel feature.**

```
enabled (bypass)  +  active transform configuration  =  displayed result
```

`src/core/ColorTransform.{h,cpp}` owns both terms and compiles the second into a
single `OCIO::ConstCPUProcessor`. The configuration is a tagged union:

| Kind | filled in by | status |
|---|---|---|
| `None` | Reset | the raw/default state |
| `Lut` | `Load LUT...` | shipped in stage 1 |
| `DisplayView` | `Color Transform...` | compiled and reachable, no UI yet |

Nothing downstream of `setConfig()` knows which kind it is -- everything sees only
the compiled processor. That is what makes the ACES dialog a **call site** later
rather than a redesign, and it is why there is deliberately no second LUT
pipeline to reconcile.

`enabled` and the configuration are **independent state**. Turning the bypass off
does not discard the configuration, which is what makes re-enabling a bool
becoming true and the next paint, with nothing recompiled and nothing re-read
from disk.

### Where the stage runs, and why exactly there

`ViewerWidget::setFrame()`, between the source frame and the renderer:

```
decoder ──► viewer_->frame_  ──►  ColorTransform::apply()  ──►  renderer_->setFrame()
              (SOURCE)                                            (DISPLAY)
              Copy Frame reads this
```

**That seam is forced by Copy Frame.** `copyCurrentFrame()` reads
`viewer_->frame()`, so applying the transform into that buffer would silently turn
Copy Frame into "copies the transformed image" -- the assessment's item 6, and the
thing the brief forbids. Keeping the transformed buffer downstream of `frame_`
answers it structurally rather than by remembering. Source pixels are never
modified: `apply()` takes its input `const` and allocates a new destination.

When the stage is inactive, `setFrame()` is exactly what it was before it existed
-- one pointer test, then the source frame by refcount.

### The one interaction with the rest of the engine

The stage works on BGRA8. Since GATE C the d3d11 default delivers full-resolution
frames as **planar YUV**, so `syncPlanarOutput()` -- already the single place that
decides the decoder's output layout -- now also asks whether the colour stage is
active:

```cpp
const bool colorStageNeedsBgra = colorTransform_.isActive();
videoDecoder_.setPlanarOutputEnabled(
    allowed && !colorStageNeedsBgra && viewer_->rendererAcceptsPlanarYuv());
```

**Nothing changes while the transform is off**, which is every existing
measurement in this repo -- GATE C's planar path is untouched. With it on,
full-resolution frames go back through swscale to BGRA, which is the pre-GATE-C
cost and is the honest price of a CPU display stage.

### Why CPU, and what it costs

The assessment's rule was CPU first, GPU later, and that the GPU stage does not
land without a CPU-vs-GPU comparison harness. OCIO can emit HLSL, but **OCIO's own
GPU unit tests cover GLSL and Metal only** -- there is no HLSL coverage upstream --
so the generated shader is unvalidated and Trace would be writing the
resource-binding half itself. Correctness first is the right order.

**OCIO's `CPUProcessor::apply` is single-threaded, and that mattered enormously.**
Measured on 4K H.264, LUT active, same clip and duration:

| | `handoff` | presented | `drop` |
|---|---|---|---|
| transform OFF | **0.76 ms** | 24.00 / 24.00 (**100.0%**) | 0 |
| ON, single-threaded | **77.28 ms** | 11.49 / 24.00 (**47.9%**) | 61, `media 97.4%` |
| **ON, parallel row bands** | **13.27 / 14.13 ms** | **23.93 / 23.92 (99.7% x2)** | **0** |

Single-threaded the stage measured **9.3 ns/pixel and linear in pixel count** --
the 4608x3164 Alexa clip read 135.7 ms at the same rate -- i.e. most of a 41.67 ms
budget spent twice over. A `ConstCPUProcessor` is immutable once built and safe to
apply from several threads at once (this is what OIIO's own colour path does), so
`apply()` splits the image into **row bands** over one shared processor: each band
is a contiguous sub-image, and a display transform is per-pixel so no seam is
possible. Bands are `min(hardware_concurrency, 16)`, dropped when there are fewer
than 64 rows each, and the calling thread takes the last band rather than idling.

**5.5x, and it is the difference between "unusable for playback" and 99.7% of real
time with zero dropped frames.**

Also recorded: the first cut `memcpy`'d the frame and transformed the copy in
place. OCIO takes separate source and destination descriptors precisely so a
caller need not, and at 4608x3164 that copy was a wasted pass over 58 MB.

## 2. `--ocio-selftest`

`Trace.exe --ocio-selftest[=<file>]`, and it is a step in CI.

Stage 0 linked OCIO and nothing referenced it, so the linker emitted no direct
import and `TRACE_WITH_OCIO=1` was a claim nothing tested. This is the
`--renderer-selftest` idea applied to the colour stage: five separate assertions
rather than one "did it throw", because each fails for its own reason.

| exit | means |
|---|---|
| 20 | not compiled with OCIO |
| 21 | no runtime version |
| 22 | the config did not load |
| 23 | no processor / no CPU processor |
| 24 | the transform compiled and **left the pixel unchanged** |
| 25 | the optional `=<file>` did not load |

**(24) is the one that matters and the one an "it did not throw" check would
miss.** A processor that compiles and then applies an identity is
indistinguishable from a working one by every other signal, and identity is
exactly what a mis-resolved colour space produces -- stage 0 measured
`getColorSpaceFromFilepath("x.exr")` returning `Raw` under the Redshift config,
which is that failure in the wild. So the pixel is compared before and after.

**The config is OCIO's own built-in ACES config (`ocio://default`), not a file.**
A CI runner has no colour configs and no test assets; a selftest needing one could
not run there, which is the whole point of adding it.

Result on this box:

```
trace-ocio: version=2.5.2 config=ocio://default input=ACEScg
            display=sRGB - Display view=ACES 2.0 - SDR 100 nits (Rec.709)
            rgb 0.18->0.34919,0.34919,0.34919 moved=1
exit 0
```

and with the real ARRI LUT passed as `=<file>`, additionally
`file=... kind=lut active=1`, exit 0.

**Proven able to fail**: a nonexistent LUT exits **25** (`LUT not found`), and a
real file containing garbage exits **25** carrying OCIO's own parser error
(`'iridas_cube' failed with: ... Malformed color triples`). Note the input space
the built-in config resolves is **ACEScg** -- the same role the upcoming workflow
will use, taken from `scene_linear` and never from the file rules.

## 3. View menu

```
Color Transform          [checkable]   master ON/OFF bypass
Color Transform...       [disabled]    the config/display/view dialog: stage 3
Load LUT...
Reset Color Transform
```

- **The checkbox is a bypass, not a delete.** Toggling it never touches the
  configuration.
- **Loading a LUT enables the transform in one action** -- a user who picked a
  file has asked to see it.
- **Disabling preserves the configuration; re-enabling restores it immediately.**
  Measured through the menu itself (Alt+V, then `c`) on the Alexa clip, mean luma
  over the picture band: **ON 187.07 -> bypass 109.88 -> ON again 187.07**. The
  bypass value equals the never-loaded value exactly, and re-enabling returns to
  the transformed value exactly.
- **Reset returns to a defined raw state**: no configuration, bypass off, the
  persisted keys removed. Deliberately *not* "turn the bypass off", which would
  leave a LUT loaded and invisible.
- **`Color Transform...` is present and disabled rather than absent.** The
  assessment fixed this menu's shape, and a row that appears later moves every
  item under it. Same choice the Share menu's LucidLink row already makes.
- **ON/OFF never reopens media.** `applyColorTransformChange()` re-syncs the
  planar decision and re-delivers the frame already on screen -- for video one
  exact `Step` re-request (the same landing path a slider release uses, so it is
  frame-exact by construction), for a still or a sequence just a re-run of the
  stage with no decoder involved.
- **Mnemonics**: `&Color Transform` (C), `Color Transfor&m...` (M),
  `Load L&UT...` (U), `&Reset Color Transform` (R). The first draft used T and L,
  which collide with `Always on &Top` and `&Lock Window to Media Aspect Ratio` --
  found before shipping, and `warnOnDuplicateMnemonics()` is the standing check.
- **No keyboard shortcut was bound.** `C` is free and the assessment reserves it
  for this, but the brief specified the menu only, and a new bare-key shortcut has
  to be checked against phase 7's text-field guard first. Left as an owner call.

## 4. LUT support

`.cube` is the target and is what was tested. `.3dl`, `.csp`, `.spi1d`, `.spi3d`,
`.clf` and `.ctf` come free with `FileTransform` and are offered in the file
filter; only `.cube` has a real file behind it in testing.

A LUT loads with **no OCIO config involved at all** -- `Config::CreateRaw()` is a
minimal config that exists only to own the `FileTransform`. Interpolation is
`INTERP_BEST`.

A failed load **leaves the previous configuration in force** and says why. A LUT
that silently became "no transform" would look identical to one that loaded and
did nothing.

**Verified on the real asset**: `ARRI_LogC4-to-Gamma24_Rec709-D65_v1-65.cube`
(`LUT_3D_SIZE 65`) on the Alexa ProRes clip turns the flat LogC4 picture into a
correctly contrasted Rec.709 one -- blue sky, teal water, white sand -- and the
HUD reads `xform ON ARRI_LogC4-to-Gamma24_Rec709-D65_v1-65.cube`.

### A stated limit: the stage is 8-bit

The processor is built `BIT_DEPTH_UINT8` in and out, because the frame reaching
this seam is already an 8-bit BGRA display buffer -- that is what both renderers
present. **This is a real limit for the coming ACEScg workflow, not a detail**: a
scene-linear EXR carries values above 1.0, and the EXR path already flattens to
8-bit with a `pow(1/2.2)` inside `loadExr` long before this stage sees it. Full
precision needs a float display buffer end to end, which is the GPU stage's
problem. Recorded rather than half-built.

## 5. Ready for ACEScg, without the UI

`Config::DisplayView` carries `configPath`, `inputSpace`, `display`, `view` and
`look` today, and `setConfig()` compiles all five -- including a `LookTransform`
composed ahead of the `DisplayViewTransform`. What is missing is only the dialog
that fills them in.

**The input space defaults to the config's `scene_linear` ROLE and never to
`Config::getColorSpaceFromFilepath()`**, which stage 0 measured returning `Raw`
for any `.exr` under the Redshift config's own file rules -- the wrong answer,
arrived at through an API call that looks correct. That decision is in the code
with the reason beside it.

## 6. Copy Frame

**Copy Current Frame copies RAW / SOURCE pixels. That is unchanged by stage 1,
and it was verified rather than assumed.**

With the ARRI LUT active and the vivid Rec.709 picture on screen, `Ctrl+C` put the
**flat LogC4 source** on the clipboard at the full 4608x3164 -- visibly the
untransformed image.

Two things it does not apply, both pre-existing and both deliberate: the user's
**view transform** (rotate/flip), and now the **colour transform**. The refusals it
already had -- no frame, and a preview-resolution frame mid-drag -- are untouched.

**This is a decision, not an accident, and it is the one worth re-opening
deliberately if ever.** A reviewer copying a frame to send to someone may well
want what they are looking at. Changing it is a one-line change at the seam
(`viewer_->frame()` -> the display frame) and would need saying out loud in the
release notes.

## 7. Regression

Physical panel 5120x1440 @ 239.999 Hz. **The default state is off** -- confirmed
by inspecting the real settings file, which carries no `color/` keys, so every
figure below with the transform off is the shipping configuration.

### The transform OFF -- the "did this break anything" half

- **`scrubbar.ps1` full pool: PASS -- 22 files, 88 legs, `delta 0` throughout.**
- **4K H.264 cadence x2: 100.0% / 100.0%**, 120 frames, `drop 0`, `rephase 0`,
  `tick-late 0 of 119`, `tick-stall 0`, `sizemove 0`, `handler>budget 0 of 119`
  (max 4.7 / 4.4), all 119 gaps `~1x`.
- **ProRes 4444 cadence x2: 99.8% / 99.8%**, 261 frames, `drop 0`, `rephase 0`,
  `handler>budget 0 of 260` (max 34.7 / 33.7).
- **4444 `scrub -SnapRelease`: `target 261 shown 261 delta 0`, `walk 0f`,
  `hitch 0`**, and `dst YUV444P12 planar` -- i.e. **GATE C's planar path is
  intact**, which is the specific thing the stage could have damaged.
- **All four selftests green**: `renderer=d3d11 fellback=0 planar=1`,
  `OK - 11 shapes x 4 scale factors`, `--scrub-selftest` over all 88 legs, and
  the new `--ocio-selftest`. `verify_trace_assets --strict` green at **33
  embedded files** (unchanged -- no asset moved).
- **`warnOnDuplicateMnemonics()` prints only the three pre-existing lines**
  (File/f, Edit/e, Help/h) that CLAUDE.md already records. No new collision from
  the four added View items.

### The transform ON -- the "does it work" half

Mean luma over the picture band, LUT off vs on, same clip and frame. The LUT is
`ARRI_LogC4-to-Gamma24_Rec709-D65`; applying it to non-LogC4 media is
colorimetrically meaningless, so **these rows test the mechanism reaching every
media class, not colour correctness**. Only the Alexa row is a real workflow.

| media | off | on | delta | |
|---|---|---|---|---|
| still (PNG) | 23.40 | 14.31 | 9.09 | CHANGED |
| **EXR sequence** (217-frame PIZ) | 59.44 | 85.47 | 26.03 | CHANGED |
| H.264 4K | 209.50 | 253.81 | 44.31 | CHANGED |
| ProRes 4444 | 100.88 | 147.13 | 46.25 | CHANGED |
| **Alexa ProRes + its own LUT** | 109.88 | 187.07 | 77.19 | CHANGED |

- **Both renderers**: d3d11 `109.88 -> 187.07` (delta 77.19), cpu
  `109.61 -> 186.75` (delta 77.14). The 0.3 between them is the video band's own
  recorded cross-backend difference, not the stage.
- **Bypass toggle through the menu**, no media reopened:
  **ON 187.07 -> bypass 109.88 -> ON again 187.07.** Bypass equals the
  never-loaded value exactly; re-enabling returns to the transformed value
  exactly.
- **Fullscreen keeps the transform**: picture-region luma **185.99** against the
  windowed **187.18** (0.6%, the different fit). Escape restores to **187.18**
  exactly.
- **Frame stepping through the stage**: `+1 -> 187.16`, `-1 -> 187.18` -- back to
  the starting value.
- **Scrub release exactness with the stage active**: `target 261 shown 261
  delta 0`, `walk 0f`, `hitch 0`, and `dst RGB32/BGRA` confirming planar stood
  down. `release 39.1ms` against 22.4ms with the stage off -- the transform's
  cost on the landing.
- **Reset**: returns the picture to **109.94** against the raw **109.88**, and
  clears the persisted keys.
- **Copy Frame with the LUT active** puts the untransformed 4608x3164 LogC4
  source on the clipboard.
- **A media change keeps the transform.** With the LUT active on the Alexa clip
  (luma 187.18), opening `Splash_1.mp4` through File > Open in the SAME process
  gives **253.81** -- the matrix's transformed value for that clip to the digit,
  against its raw 209.50. The stage is a viewing preference and survives the
  file change, like Loop; `syncPlanarOutput()` is already called on open, so the
  new media arrives as BGRA with no extra plumbing.

### Cost, stated plainly

With the stage **off** nothing moves -- the figures above are the recorded
standard. With it **on**, `handoff` goes from 0.76 ms to **13.3-14.1 ms** on a
4K frame and playback holds **99.7% of real time with `drop 0`**. The stage is
affordable at 4K on this box **because it is parallel**; single-threaded it was
77.3 ms and 47.9%.

**It has not been measured on the 8K plate and should not be assumed to hold
there**: at 9.3 ns/pixel single-threaded and ~33 Mpx, even the parallel stage is
of the order of a whole frame budget, and that file already fails to reach real
time without any colour work at all.

### One harness lesson

The first fullscreen check read luma **57.21** against a windowed 187.18 and
looked like the transform being lost. It was the **detector**: a 1.46:1 picture
pillarboxed on a 3.56:1 panel puts most of a full-width sample band on black
bars. Measuring the picture's own column range gives 185.99. **A luma detector
has to find the picture before it samples it** -- the same class as
`emptystate.ps1`'s stage-bound trap, in a new costume.

### CI

Run [32653491585](https://github.com/bigsbypuglise/trace-alpha/actions/runs/32653491585)
on `ce993993`, **green in 3.7 min** -- the vcpkg cache hit and the install step
was skipped, so stage 0's `--clean-after-build` decision is still paying.

```
OpenImageIO + OpenColorIO detected by CMake.
Package verified: 11 required files present, 118.3 MB total.
trace-ocio: version=2.5.2 config=ocio://default input=ACEScg
            display=sRGB - Display view=ACES 2.0 - SDR 100 nits (Rec.709)
            rgb 0.18->0.34919,0.34919,0.34919 moved=1
trace-selftest: renderer=d3d11 fellback=0 planar=1
trace-shape: OK - 11 shapes x 4 scale factors
```

**`Verify OpenColorIO links and executes` passed on the runner**, which is the
point of the whole step: `moved=1` there means a real ACES display transform
compiled and executed inside the SHIPPED binary on a machine that has no colour
configs and no test assets. Stage 0 could only say the library had been built.

## Deferred, with their reasons

- **The display path is 8-bit end to end -- the ACEScg / GPU-display-path issue.**
  Section 4 states it. Full precision needs a float `PixelLayout`, both renderers
  carrying it, and the transform applied in the shader. Stage 5's problem, and the
  reason stage 3's dialog cannot be called "correct ACES" on its own.
- **The transform's cost on the 8K plate is unmeasured.** 9.3 ns/pixel
  single-threaded, linear in pixel count; at ~33 Mpx even the parallel stage is of
  the order of a whole frame budget, and that file already fails to reach real time
  with no colour work at all (best recorded 56.9%). **Do not quote the 4K figure
  for it.**
- **The `C` shortcut is undecided and left so.** Free, reserved by the assessment,
  deliberately unbound because the brief specified the menu only -- and a new
  bare-key shortcut has to be checked against phase 7's text-field guard first.
  Owner decision.

## Stage 2 carry-forward, recorded here so they are not re-derived


- **Three channel-naming conventions**, not two: root `R G B`, named layers
  `.red/.green/.blue`, Cryptomatte `.R/.G/.B/.A` (uppercase, with alpha). A
  grouper written against any one finds nothing in the other two.
- **`loadExr` takes channel index 3 as alpha regardless of what it is** -- on the
  27-channel Redshift file that is `Beauty.red`. Invisible today because the draw
  path ignores alpha, which is what makes it a trap rather than a bug.
- **`loadExr` reads every channel**: `width x height x nchannels` floats, ~224 MB
  per frame on that file, of which 24 MB is used.
- Alpha is pushed through the same `pow(1/2.2)` as the colour channels in
  `loadExr`, which is wrong in principle and invisible for the same reason.
- **The dev HUD reads `ch:4` on a 3-channel file.** `MainWindow.cpp:6539`
  hard-codes `info.channels = 4` on the frame-handoff path -- honest about the
  display buffer (always RGBA after handoff), misleading as a label, and only
  visibly wrong now that EXR opens. What that field should say is a stage-2
  decision, which is why it was left alone.

**Nothing here was fixed in stage 1, deliberately.** Each is either invisible
today or a decision that belongs with the layer work, and touching them piecemeal
would have meant redesigning EXR channel handling inside a colour-transform
stage -- which the brief ruled out and which would have made both harder to
judge.
