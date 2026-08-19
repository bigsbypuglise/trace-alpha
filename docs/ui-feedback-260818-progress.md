# Owner interface feedback 2026-08-18 — the work, item by item

Companion to `docs/ui-feedback-260818.md`, in that document's order. Fifteen of the eighteen
items are DONE in this pass; the top-bar group (1's architectural half, 8, 11) is deliberately
stopped before, per instruction, with the choice written up at the end for the owner.

**Four items reverse or conflict with an earlier decision (4, 5, 6, 15). Each is recorded here
as the owner deciding again with the thing on screen — not as drift — and the superseded
decision is named at the code site as well.**

---

## Confirmed diagnoses — fixed

### 18. Console window — `WIN32` added, and the logs survive it

`add_executable(Trace WIN32 ...)`: Trace is a GUI-subsystem executable now and Windows creates
no console. Qt6::Core supplies the WinMain shim automatically, so `main()` is unchanged.

**The diagnostic knobs do not go quiet.** `attachParentConsoleForDiagnostics()` in `main.cpp`
runs before anything can print: if the std handles are already valid (a redirection — CI
capturing a selftest, a harness capturing stderr) it does nothing and the pipes keep working;
if they are not, it attaches the parent's console and binds the orphaned streams to it. From
Explorer there is no parent console and the logs go nowhere, which is correct for a
double-clicked GUI app.

**CI keeps working, verified rather than assumed.** PowerShell does not wait for a bare GUI-app
invocation — `& Trace.exe --renderer-selftest` returns immediately with no exit code — but both
workflow steps use the capture form `$out = & $exe ... 2>&1`, which forms a pipeline, waits,
and propagates the exit code. Confirmed locally: `exit=0`, `renderer=d3d11 fellback=0
planar=1`. **A harness that launches Trace bare and reads `$LASTEXITCODE` after this change is
reading garbage; use the capture form or `Start-Process -Wait`.**

The messages the owner saw were all benign and he should be told so: the 32-thread warning is
FFmpeg's generic advice colliding with Trace's own measured thread policy (knee at 32 on this
box); the QT chapter note is a harmless MOV demuxer aside; the "Qt multimedia with FFmpeg
7.1.2" line is Qt Multimedia's own bundled FFmpeg on the audio path, separate from the FFmpeg
Trace links for video.

### 13. Clicking the video now activates the window

`D3D11VideoRenderer`'s `WM_MOUSEACTIVATE` still returns `MA_NOACTIVATE` — the child surface
still never takes keyboard focus, which is what keeps Space/arrows/J-K-L alive after a click —
but it now calls `SetForegroundWindow(GetAncestor(hwnd, GA_ROOT))` first when the top-level is
not already foreground. The click is real input to the process, which is what entitles the
call to succeed. Verified behaviourally (below): click on the picture, then Space, still
toggles playback.

### 4. Opening media no longer recentres the window — SUPERSEDES §4 item 7 (owner deciding again)

`applyMediaWindowShapePass` anchors the **frame's top-left** on every shaping after the first:
opening new media keeps the window where the user put it and changes only the size, clamped
into the same monitor's work area. Centring survives for exactly one case — the first shaping
of a session, where the window sits at Windows' default placement and there is no position to
preserve. This supersedes §4's *"Center the resized window within the current monitor's
available work area"* and the phase 12 sign-off's centring behaviour, **as an owner decision
taken with the behaviour in front of him** (feedback item 4). Top-left rather than centre
because a centre-anchored resize moves the title bar out from under the cursor.

One consequence worth naming: the DPI reshape (`reshapeAfterDpiChange`) now also preserves
position rather than recentring on the new monitor — which is *more* in the spirit of "the
user moved it, they did not ask for it to be resized", but means `dpimove.ps1`'s round-trip
geometry may differ from the 2026-08-15 record in position (size is unchanged).

---

## The reveal loop (items 3 + 7) — found by instrument, and the offered hypothesis was wrong

**These were one bug, as the owner said. It was not the cursor-hide feedback loop the triage
document offered** — the windowed phases of the probe cycle identically with the cursor never
hidden. Fourth session in a row where the control corrected a plausible reading.

**The instrument**: `TRACE_REVEAL_LOG=1` (new, permanent) — one stderr line per `reveal()`
call with a source tag (`mousemove`, `mousedown`, `keypress`, `startup`, `fullscreen-toggle`,
`alt-mnemonic`, ...), per auto-hide decision (FIRED vs HELD, with which condition held), per
chrome show/hide transition, and per mouse-move sample with coordinates.
`scripts/../scratchpad` probe: five phases, pointer **parked** — paused/windowed,
playing/windowed, top-strip band, fullscreen centre, fullscreen strip band.

**The measurement**: 46 mousemoves in a run with zero physical pointer motion, and every
`chrome HIDDEN` followed within 2–5ms by a `mousemove` at the **identical pixel** (640,360 —
nowhere near the strip), which revealed the chrome again. A perfect ~2.2s blink cycle,
windowed and fullscreen alike.

**The cause**: Windows re-evaluates which window is under the cursor whenever any window's
visibility changes, and posts a synthetic `WM_MOUSEMOVE` at the unchanged coordinate. The
`TopChrome` strip is a **native window** (it must be — §18.4), so its own hide at the end of
the fade generated a "move", the move revealed the chrome, the idle timer restarted, and the
strip's next hide re-triggered itself. Fullscreen read worse only because the blink is more
visible there and the cursor came back with every reveal.

**The fix, two halves, both measured:**

1. **A move that does not move is not input.** `OverlayModel::onMouseMove` remembers the last
   delivered coordinate and treats a same-pixel move as no gesture — which removes the
   synthetic class exactly, because a real gesture cannot arrive at the pixel it is already
   on. Reset on mouse leave, so a genuine re-entry always reveals.
2. **The pointer resting on the strip holds the chrome**, through the existing
   `holdVisible` hook (`topChrome_->underMouse()`). The auto-hide's own rule is "never hide
   under the pointer"; the overlay could check that for its own controls but cannot see the
   strip. This also closes the one case the coordinate filter cannot hold across: the strip
   showing over a parked pointer hands the surface a real `WM_MOUSELEAVE`, which resets the
   filter's memory.

**After (probe re-run, same phases)**: parked at centre — chrome hides once and stays hidden,
every synthetic move logged as `SYNTHETIC (filtered)`; parked on the strip band — chrome stays
up, `auto-hide HELD ... hostHold 1` every 2s; every remaining `chrome SHOWN` in the log has a
real cause. Windowed and fullscreen.

---

## Bugs 16 + 17 — thumb

**17 (done)**: the 13px→16px grow-while-scrubbing thumb is removed — one cell, one size,
always a 1:1 blit. The handoff's 16px scrub variant left the atlas and the code entirely
(`aThumbScrub_`, `kThumbScrubLogical`, `thumbScrubPx_` all gone).

**16 (re-checked after 17, as instructed)**: the pop was the cell swap at the moment the drag
begins — the exact resample class step 5 measured. With one fixed cell there is nothing left
to resample: the thumb is a snapped 1:1 blit at every position. Verified by mid-drag capture;
if the owner still sees an artifact at the machine, his fallback (a plain white dot at fixed
size) is the recorded next step — the current cell is already a fixed-size dot-in-ring, so
that would be a one-cell art change.

---

## Mechanical

- **2 — rounded track ends (done)**: radius = half the track height, at both track heights
  (4px base, 6px hover). Implemented as half-circle end caps sliced 1:1 from four new atlas
  circle cells (two heights × two colours, alphas baked in like every other track cell),
  because a rounded rect stretched to an arbitrary width distorts its corners and draw-time
  alpha is the measured cross-backend divergence. The played portion's right end stays square
  where it meets the unplayed track or sits under the thumb; it gets its own cap at 100%.
- **15 — Go to Start / Go to End buttons removed (done; owner deciding again, reversing the
  step 5 decision)**: the two buttons left the strip, the hit test, the mouse-up dispatch,
  the `Region` enum, the accessibility proxy list (8 strip controls now, reading order
  unchanged), the `OverlayHooks` struct, the atlas, the `.qrc` and the asset tree
  (`go-to-start`/`go-to-end` PNGs + SVG masters deleted — artwork follows behaviour out as
  well as in; `verify_trace_assets.py --strict` green with no script edit). **Home and End
  stay bound** through their untouched QActions; the freed width goes to the timeline.
  `uiatree.ps1` now reports eight named transport controls, not ten.
- **17** — see above.
- **14 — open/close confirmations dropped (done)**: the `"Opened"` toast and the close path's
  `"No media open"` toast are gone — the picture appearing, and the empty state's own mark,
  already say both. **Copy Frame's confirmation stays** (the owner named it as the one that
  earns its place: its result is invisible without it). Every error and refusal still
  appears; only the two success messages went.
- **9 — frame counter jitter (done)**: tabular figures (`tnum`) on the strip's readout font
  and on the dev HUD's painter — Segoe UI Variable carries the feature — so every digit has
  the same advance and a running counter holds still. The position readout is additionally
  **left-aligned** (superseding the mockup's `text-align:right`) so a digit-*count* change
  (99→100) grows into the reserved slack toward the track instead of moving the value's
  leading edge. The eights-reservation stays: tnum makes digits equal to each other, not to
  the widest string, and the track's position still comes from the reserved cell width.
- **10 — mnemonic underlines honour Windows (done)**: `KeyboardCuesStyle` (a `QProxyStyle`
  over Fusion, installed in `Theme::apply` so appearance keeps one home) answers
  `SH_UnderlineShortcut` from `SPI_GETKEYBOARDCUES`, live per query: cues on → underlines
  always; cues off (the Windows default) → underlines only while Alt is held. Keyboard and
  screen-reader users keep their cues; the menus look the way the owner wants by default.
  `F` remains the Frames readout shortcut — untouched.

---

## Conflicts with the design package — owner deciding again

### 5. Empty-state mark — the INK is centred now (owner override of the delivered art)

The art itself balances the right-pointing triangle by eye, +9.5px right of its canvas
centre, and Trace reproduced that to within a pixel. The owner prefers it visually centred —
a legitimate disagreement with the designer, recorded as an override, not a correction.
Implementation: the mark's ink bounding box is **measured from the art's own alpha at
rasterisation time** (alpha ≥ 40, so the glow does not drag the box) and the ink centre is
what lands on the column centre — no hard-coded offset, so the renditions remain a drop-in
swap and replacement art re-centres itself. `emptystate.ps1`'s launch assertion updated: the
expected optical offset is now ~0 (was ~+9.5/+10.5), recorded in the script as the owner's
item 5.

### 6. Loop no longer starts highlighted — REVERSES the phase 14 persistence sign-off (owner deciding again)

Two findings:

1. **The poisoned-settings hypothesis was NOT confirmed on this box** — this machine's
   `trace.ini` (`%LOCALAPPDATA%\Trace Project\Trace\trace.ini`) has **no `playback/loop` key
   at all**, and the one portable deployment found (Downloads) carries no `trace.ini`. So
   what the owner most likely saw was the phase 14 persistence itself working as designed —
   he (or a harness before `overlay.ps1` got its scratch INI) left Loop on, and the next
   session started with it highlighted.
2. **The decision**: Loop starts **off** every session. It is no longer persisted; the
   settings key is gone and a stale `playback/loop` in any existing INI is simply never read
   again (which also retires the pre-`b2a901b` poisoned writes wherever they exist). Loop
   still survives a **file change within a session** — the half of "review preference" that
   survives the reversal: someone checking a cycling animation keeps it across versions of
   the same shot. This supersedes phase 14 sign-off part 1's "persistence across a file
   change and a restart was accepted with the feature."

---

## Item 1 interim — landed (approved)

The Trace prism mark and wordmark are gone from the top strip; the menus lead it now, at the
edge pad. The title bar already carries the app identity, so the strip's copy was the
redundancy — the two bars read as chrome plus content rather than a doubled header. The
`brand-mark-15/30` PNGs and their working-copy SVG master left the `.qrc` and
`assets/interface/branding/` with the labels (the master remains in the delivered package as
`trace-play-mark.svg`, byte for byte, so step 12 can bring it back). Asset verifier green
with no script edit. Reversible in one commit.

---

## Stopped before: the top-bar group (1's architecture, 8, 11)

See the decision write-up handed to the owner with this session's summary. Not started, per
instruction. **The backdrop ship decision interacts**: if the strip is rebuilt as composited
quads, `TRACE_STRIP_BACKDROP` is moot; if it stays native, the painted blur is the only route
to the design's look. Answer 8 and 11 before shipping the backdrop further.

### Item 11 — CLOSED in a later session (2026-08-18): the native strip fades, option 3 worked

The cheap experiment the write-up proposed — whole-window opacity on the native strip,
`WS_EX_LAYERED` + `SetLayeredWindowAttributes(LWA_ALPHA)` — was run and it passes on the
shipping renderer. Full record in the session summary; the durable facts:

- **The pass criterion was what is revealed mid-fade, and the answer is THE VIDEO.** Measured
  with a pinned alpha (`TRACE_TOPCHROME_ALPHA=128`) over bright footage with the backdrop
  forced off: the mid capture matches the per-pixel prediction `(strip + video)/2` at
  **MAE 0.16**, against 85.29 for the blend-toward-black failure mode. DWM composites the
  layered child against the sibling swapchain's pixels.
- **It only works because of a manifest nobody had ever needed.** Layered CHILD windows are
  ignored unless the app declares Windows 8+ `supportedOS`, and Trace.exe carried only a
  `trustInfo` manifest — the first run measured alpha 128 rendering byte-identical to alpha
  255 (MAE 0). `app/trace.manifest` adds the declaration; the linker merges it. This changes
  the app-wide OS compatibility context, which is why the regression below was run.
- **No second timer.** The strip's alpha is driven per animation tick from `OverlayModel`'s
  own fade opacity through `OverlayHooks::setChromeOpacity`, so top and bottom ramp in exact
  lockstep over the same `kFadeMs` — measured as a real ~165ms ramp with four intermediate
  frames between strip mean and video mean. `setChromeRevealed` stays the map/unmap edge.
- **`TRACE_RENDERER=cpu` keeps the pop, and that is graceful rather than broken.** The alpha
  is applied at the OS level identically (probed: both strips read `LAYERED alpha=128`), but
  Qt's native children share the top-level backing store, so the rows under the strip hold a
  baked copy of the strip itself — strip blended over strip reads opaque. The strip maps and
  unmaps at the same instants as before the change, so cpu is visually identical to the old
  behaviour. With no media the host pins alpha full, so the empty state cannot fade its menu
  access away.
- **Hit-testing and the reveal loop are unaffected.** A click on File and an `Alt+F` from a
  hidden strip both open the menu while layered; `TRACE_REVEAL_LOG=1` over a 10s parked
  pointer reads `SHOWN 0 / HIDDEN 1 / synthetic-filtered 1`, identical to a fade-off control.
- Knobs: `TRACE_TOPCHROME_FADE=0` is the rollback (never touches the window style),
  `TRACE_TOPCHROME_ALPHA=N` pins the alpha for measurement. Harness:
  `scripts/measure/topchromefade.ps1` (blend / anim / menus / loop).

**Item 8 is NOT closed by this**: at rest the strip is still opaque over the painted blur.
What changed is that a uniform resting translucency is now *possible* on d3d11 — an owner
option, deliberately not taken here because cpu cannot match it. **Taken the next day — see
the item 8 section below.**

### Item 8 — CLOSED (2026-08-19): the strip rests translucent, and the blur is REMOVED

**Owner decision, option B: the resting strip is partially transparent, accepting that d3d11
and cpu deliberately differ on the top strip.** The mechanism is item 11's — the fade's
`SetLayeredWindowAttributes(LWA_ALPHA)` ramp now tops out at **`TopChrome::kRestingAlpha =
215`** instead of opaque, so the fade is a 0 → 215 ramp and the settled strip shows the real
video through itself on the d3d11 default. One constant plus its plumbing, as predicted.

**THE ALPHA WAS PICKED FROM LEGIBILITY OVER BRIGHT BUSY FOOTAGE, NOT FROM THE DESIGN'S CSS**
(owner instruction — the CSS scrim and a uniform window alpha are different mechanisms).
Swept 155..255 × blur on/off over the two hardest bands in the asset set: the 4K milk splash
(`Splash_1.mp4`) and the Marinelaverse end tag's bright saturated high-frequency detail, with
the menus open, using `TRACE_TOPCHROME_ALPHA` pins on the pre-change binary — so the whole
sweep ran before a line of code moved. 230 barely reads as translucent; 200 goes marginal
where a near-white element crosses a label; **215 (~84%) keeps every label cleanly separable
on the worst frame and still reads as real translucency.** Captures in the session record.

**THE SWEEP ANSWERED THE BLUR QUESTION THE OPPOSITE WAY ROUND FROM THE INTUITION, AND THAT IS
WHY StripBackdrop IS REMOVED RATHER THAN COMPOSED.** The expectation was that translucency
alone might fail on busy footage — sharp detail under the labels is precisely what the
design's blur exists to suppress — and the blur would stay underneath. Measured, the
composition is inverted: **the painted blur is itself a bright copy of the video, so a
resting alpha under it counts the video twice** and washes the labels out by a215 and badly
by a200 — while the solid dark strip content under the same alpha composes into a uniform
dark scrim that is *more* legible over bright footage than the shipping blur-at-opaque was.
Translucency alone reads well; translucency plus blur reads worse than either alone. So:

- `src/ui/StripBackdrop.{h,cpp}` deleted; `TopChrome::setBackdrop`, the backdrop paint
  branch, `ViewerWidget`'s per-frame sampling/publishing (`refreshBackdrop`,
  `setBackdropSink`) and `TRACE_STRIP_BACKDROP` all left with it. The strip paints the design
  package's solid fallback gradient, always; the translucency is the window's, applied by DWM.
- **The Windows transparency honour SURVIVES the removal, re-purposed**: the
  `EnableTransparency` tri-state read moved into `TopChrome` and now gates the resting alpha —
  setting off → the strip rests opaque, which is exactly the package's "solid #14161A when
  transparency effects are disabled" case. `WM_SETTINGCHANGE` re-applies it live. The dev
  HUD's `backdrop` field is replaced by **`strip`**: `a215` / `a215 (unset)` /
  `opaque (windows)` / `aN (env)` / `opaque (fade off)` / `n/a` in bar mode.
- `scripts/measure/stripbackdrop.ps1` and `backdropcost.ps1` retired with the mechanism;
  `topchromefade.ps1` gains **`-Mode rest`**, which verifies the shipping resting state per
  renderer with the accepted divergence written in as the expectation: d3d11 must match
  `strip*(215/255) + video*(40/255)` and **cpu must read opaque** — measured **MAE 0.21** and
  **0.1** against their own predictions respectively.

**THE EMPTY-STATE STRIP DELIBERATELY RESTS OPAQUE** (`mediaTitle_` empty is the gate): there
is no picture behind the empty stage for translucency to show, and the empty-state window is
the one surface byte-comparable across backends — re-measured after the change at **0 of
972,800 px differing, max channel delta 0**, the step 11 standard preserved as a working
instrument. **The owner-accepted divergence is over VIDEO**: the strip band there reads 5434
of 5434 samples differing at max delta 37 across backends (08-mid-drag), while the video band
below it reads 25 of 61,864 — so any cross-backend comparison must exclude the strip band or
expect it, and `overlay.ps1`'s header now says so. **Record the strip-band difference as this
decision, never as a defect to reconcile.**

**One stale harness found in passing, defeated by item 15 rather than by this change**:
`overlay.ps1` still mapped the ten-control strip, so on the eight-control strip every
left-cluster aim landed one control off — its "go-to-end" tap toggled Loop and its "loop" tap
sampled the frame readout, reading two FAILs on a correct build. Re-pointed to the
eight-control offsets, and its Go to Start/End leg drives `Home`/`End` (where item 15 moved
the behaviour) with the same played-track observable: 0.817 → 0, loop accent 0/68/0 (the
recorded figure), all legs PASS on both backends.

**Regression flat** (1920x1200 @ 59.999Hz — the Parsec-class display, so no figure is
comparable to a physical-panel record; captures read the composited framebuffer, so pixel
judgements stand): 4K H.264 cadence ×2 **99.1/99.1%** `0 of 120` identical buckets · 4444 ×2
**99.8/99.8%** `0 of 260` · 4444 `-SnapRelease` **`target 261 shown 261 delta 0`** full-res
planar, `release 21.9ms`, `hitch 0`, `land 0` · 4K H.264 reversals `rev-hit 97.3%`, `seeks 6`,
`hitch 1`, `delta 0` · **25 of 25 transitions** (a first run on `Splash_1.mp4` failed
`F -> ffBtn` with `moved 0%` — the harness's own recorded 121-frame-clip artifact, clean on
the header's named clip) · lifecycle **83.5% moving / 0% control** · `emptystate` all four
modes both backends plus `-Bar` (641-row stage) · `topchromefade` rest/anim/menus/loop
(`SHOWN 0 / HIDDEN 1 / filtered 1`, identical to the item 11 record).

**One harness defect found and fixed on the way** (`emptystate.ps1 -Mode transport`): it
revealed with a single `SetCursorPos` to a fixed pixel, and every run parks the pointer on
that exact pixel — so back-to-back runs generated NO input at all (confirmed with
`TRACE_REVEAL_LOG=1`: no mousemove line, not even a filtered one) and failed a correct build
in both fade configurations. It jiggles through two points now; 4× back-to-back PASS after.
Third instance of the mouse-harness-inputs class. The `swap` leg is separately unrunnable
from a headless session — its File ▸ Open dialog typing needs the foreground, and the
fade-off control fails it identically.

---

## Regression (this session, physical panel 5120x1440 @ 239.999Hz unless stated)

- 4K H.264 cadence ×2: **99.1 / 99.2%**, 120 frames, `drop 0`, `rephase 0`,
  `handler>budget 0 of 120`, buckets `~1x 118 / 1.5-2.5x 1` — the recorded class. (Run at
  `win 1278x1083`-class geometry; quote from the run itself as always.)
- 4444 cadence ×2: **99.8 / 99.8%**, 261 frames, `drop 0`, `handler>budget 0 of 260`.
- 4444 `scrub -SnapRelease`, bar mode widened to 1280: **`target 261 shown 261 delta 0`**
  full-res planar, `release 20.5ms`, `hitch 0`, `land 0`, `ui over-16ms 0 of 695`.
- Transitions matrix, empty-state harness, UIA tree: see the session summary (run after the
  code landed; the matrix needs the foreground to itself).

No control binary was built this session — the figures above are compared against their
recorded classes, not against a same-day control, and every one landed inside its class.

One expected *paints* difference is worth naming in advance: the reveal-loop fix means the
chrome no longer blinks on a ~2.2s cycle during idle playback, so paint counts on runs where
the pointer sat inside the window may drop relative to older records. That is the bug being
gone, not a regression.
