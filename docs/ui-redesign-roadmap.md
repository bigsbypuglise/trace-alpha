# UI redesign roadmap — owner's plan, with implementation flags

Owner roadmap received 2026-08-17, reproduced below with flags attached. The flags are not
objections; they name where a step re-opens a signed-off decision, invalidates a recorded
baseline, or touches the presentation path. The owner's guardrail governs: *does this make
Trace faster, clearer, or simpler? If not, do not add it.*

Design package: `assets/source/260817-trace-ui-v2/` — see "The delivery" at the end.

---

## FIRST, THE THING THAT CUTS ACROSS SEVERAL STEPS

**Steps 2 and 4 both change the size of the video rect, and the video rect drives cache depth
and every stall figure in the record.** This is not a regression risk so much as a
*measurement* one, and it will look like a regression if nobody expects it.

Phase 2 measured exactly this when `H` was added: hiding the HUD does **not** resize the window
— the viewer absorbs the HUD's height instead — so `win WxH` reads the same either way while
`display` goes **640x360 → 1280x720** and `stalls` goes **70 of 370 → 127 of 450**. Previews
convert to the size they will be drawn at and the cache is budgeted in bytes, so a bigger video
rect means larger entries, fewer of them, and more misses (§22.8).

Consequences to plan for rather than discover:

- **Every scrub and playback baseline in the repo was taken with the HUD visible and the status
  bar present.** Flipping the HUD default and removing chrome makes those figures
  non-comparable. Re-take the standing regression once, immediately after the chrome changes
  land, and treat that as the new baseline.
- **`hitch` stays comparable across the change** (fixed 33ms bar); `stalls` does not.
- **§4's opening geometry measures chrome as window-minus-viewer.** Removing the status bar and
  menu bar changes the chrome term, so every media-shaped opening size moves. The phase 12
  geometry sign-off was taken against the current chrome — expect to re-take it.

None of that argues against the steps. It argues for doing steps 2 and 4 **together, early, in
their own commit**, then re-baselining once rather than chasing moved numbers for the rest of
the pass.

**DONE, exactly this way, 2026-08-17** — three commits (`5ff6431` message surface, `635656a`
HUD default, `c63aba4` status bar) and one re-baseline immediately after, taken on 1920x1080 @
59.999Hz and recorded in `CLAUDE.md`'s session block: cadence flat across all seven files,
`-SnapRelease` `delta 0`/`hitch 0`, reversal `hitch 1`, both lifecycle legs, **25 of 25
transitions**, and the §4 opening geometry re-measured per shape in the shipping (no-HUD,
no-status-bar) configuration. That block is the standing reference until the next chrome
change; physical-panel figures remain valid as records but not as comparisons.

---

## The roadmap, step by step

### 1. Freeze playback behaviour first

**No flag — this is the right instruction and it matches the project's own rule.** Preserve
decode/playback, the async landing, exact paused stepping, scrub-release exactness and mixed-DPI
behaviour; keep UI work isolated and revertable.

Worth adding one mechanical rule the project learned at phase 14: **if two commits must be
separately revertable, their edits must not be adjacent.** Two independent one-line additions on
neighbouring lines conflict on whichever is reverted second, because git can only see that they
touch.

### 2. HUD off by default — **DONE 2026-08-17 (`635656a`)**

`ViewState::showHud` is `false`; `H`/Return/Enter toggle it and View ▸ Show Diagnostics HUD
was already in the menu. **`TRACE_HUD=1` forces it on from launch** — the measurement
override, which `restart.ps1` now passes by default because every recorded figure is read off
the HUD in a pixel capture; the six direct-launch HUD-reading scripts set it themselves, and
`dpimove.ps1` forces it so `-HideHud`'s `h` press still means "hide". The re-baseline was
taken with the flip in (see the session block in `CLAUDE.md`).

### 3. Polished empty state — **STATIC STATE DONE 2026-08-18 (`72aa9ac`); the idle animation is NOT built**

Built as a **fourth image in `OverlayModel`**, beside the atlas, the rate text and the toast,
emitted **outside both gates** in `buildFrame` — outside the opacity gate like the toast, so it
cannot fade, and outside `enabled_`, so it survives `TRACE_TRANSPORT_BAR=1`. It therefore
**removes a duplication rather than adding one**: the literal and its drawing code existed
independently in `CpuImageRenderer` and `D3D11VideoRenderer`, and not even as the same
mechanism (a `drawText` on one, a window-sized uploaded texture on the other).
`setPlaceholderText` and `ViewerWidget::setCenterText` are gone with it; the latter had no
callers at all.

Measured on both backends at 1920x1080 @ 59.999Hz, against the design package's own mockup:
mark ink **59x68** (mockup 59x68), optical offset **+10.5px** (mockup +9.5), hint **157x13**
centred **+0.5px**, gap **45px** (mockup 45). Cross-backend empty window **0 differing pixels,
max channel delta 0**. Harness: `scripts/measure/emptystate.ps1`, four modes — no other script
in that directory can reach this state, because `restart.ps1` takes a mandatory `-Clip`.

**Flag, REVISED — the idle animation is not the cheap thing the handoff assumed, and the two
approaches are ALTERNATIVES rather than layers.** The original note said it "needs a small
gradient animator (a `QVariantAnimation` on the stops, or a tiny shader)". That is true only if
the mark is drawn as **QPainter paths and gradients in code**, which is a different shipping
decision from the one taken here: the mark ships as committed PNG renditions embedded through
the `.qrc`, and re-authoring it as vectors would make those files embedded-and-unused — the
"artwork follows behaviour" rule pointing the other way. Animating the PNG instead means either
recolouring it at draw time (which is neither the design's spatial gradient rotation nor its
glow hue cycle) or committing a multi-phase sprite sheet, which at a smooth 18s roll is orders
of magnitude larger than the art it animates. **This is an owner decision, not a tidy-up.**

What still holds if it is ever built: **one place must decide both "the empty state is showing"
and "the animation is running"**, and that place already exists — `rebuildEmpty`'s
`mediaPresent_` branch, which is where the image is dropped and its revision bumped. A timer
started or stopped anywhere else is the failure this flag was written about. Reduced motion
holds static.

### 4. Edge-to-edge media — **STATUS BAR DONE 2026-08-17 (`5ff6431` + `c63aba4`); menu bar deliberately left for step 7**

The 35 `statusBar()->showMessage` sites were inventoried first — spec-required confirmations,
validation refusals and error reporting, none of which could vanish silently — and all now
route through **`MainWindow::showTransientMessage`**: bar mode keeps the status bar, overlay
mode shows a **composited toast** both renderers draw (a third image in `OverlayModel` beside
the atlas and rate text, emitted outside the panel's opacity gate so a message survives the
transport's fade). With every site routed, the status bar is simply never constructed in
overlay mode. The bar object and `timelineSlider_` stay alive exactly as phase 6 left them,
so the groove-scanning harnesses still run under `TRACE_TRANSPORT_BAR=1` — **but note two new
harness facts**: at 1920x1080 with the HUD shown, §4 gives 16:9 media a 656px window whose
groove is under `scrub.ps1`'s 300px minimum, so `widen.ps1` is needed after `restart.ps1` on
16:9 media too; and `transitions.ps1` runs with `-Env TRACE_TRANSPORT_BAR=1,TRACE_HUD=0`.

**The menu bar stays until step 7 builds the transient chrome that will hold it** — removing
it first would leave the app with no menu access in between.

### 5. Bottom transport overlay

**Flag — "previous / next" contradicts a stated product pillar.** Trace's own description is
"no libraries/playlists": there is nothing to go previous or next *to*. The retired glyph set
had prev/next-clip and the approved package dropped them on purpose. If these mean *frame step*,
that is keyboard-only by an owner decision at phase 4/5; if they mean *time skip*, that is new
behaviour that needs defining before it gets a button. The design handoff raises the same
question and asks for confirmation.

**Flag — volume does not exist.** Audio is 1× forward only and there is no volume control today,
only Mute on `M`. A volume slider is new behaviour. It must not touch the audio master clock:
audio owns rate and position during 1× playback, and that is what removed the hold/skip churn.
Volume is a gain applied to the sink, not a change to the clock.

**No flag on the rest** — play/pause, rewind/fast-forward, timeline, position/duration and
fullscreen all exist and are wired to shared `QAction`s. Reuse them; do not build a second path.

### 6. Frames / Timecode / Seconds

**Already shipped at phase 7, and the roadmap's three modes are a regression of four.** Trace
has `F` Frame Count, `S` Seconds, `E` Elapsed and `T` **source** SMPTE — and the entire point of
that phase was that *elapsed time is not timecode*. `T` is refused with a reason on a file that
carries no start timecode, rather than inventing `00:00:00:00`.

**Flag — do not collapse Elapsed and Timecode into one "Timecode" mode.** `hasSourceTimecode_`
is the single gate on everything SMPTE and must not grow a second answer. Keep four modes and
present them clearly; that is a naming and menu question, not a behaviour change.

### 7. Transient top chrome

**Flag — this is the accessibility risk of the whole pass.** The menu bar is currently the
main accessible surface: real `QMenu`s, exposed to screen readers for free. The composited
overlay has **no widget tree** — that is why phase 14 had to build a UIA proxy tree by hand
after phase 6 made the overlay the only transport. Moving menus into a transient overlay repeats
that mistake at a larger scale.

The workable shape is: keep menus as real `QMenu`s owned by the window, and make the *bar* that
displays them transient rather than reimplementing menus as drawn quads. `holdVisible` already
covers open popups, so an open menu keeps the chrome revealed by construction.

### 8. Auto-hide

**Built, measured and signed off.** `kAutoHideMs = 2000`, `kFadeMs = 165`, and the owner's
sign-off recorded *"no tuning is wanted"* — those are settled numbers, not defaults.

**Flag — 1.5s vs 2s is a decision, not an implementation detail.** Changing it re-opens that
sign-off. The hold rules the roadmap asks for (pointer over controls, timeline dragging, popup
open) are all already implemented in `OverlayHooks::holdVisible`.

### 9. Fullscreen presentation mode

**Largely built at phase 6** — Escape exits, geometry restores, the monitor rule holds, and the
cursor hides after inactivity. Note the cursor is hidden by *two different mechanisms* on the
two backends (`Qt::BlankCursor` on CPU, `SetCursor(nullptr)` answering `WM_SETCURSOR` on D3D11),
so `GetCursorInfo` sees only one of them — verify by the handle, not the flag.

### 10. Windows 11 visual conversion

**Flag — Mica/Acrylic backdrop blur is the one item here with real presentation risk.** The
video is presented into a **child HWND with a flip-model swapchain**. DWM backdrop effects apply
to the window backdrop and interact with the composition model; blur behind a swapchain-presented
child is at best ineffective and at worst forces a different composition path. **Prototype it
against a 4K ProRes 4444 playback run before committing to it in the visual language**, and have
the solid fallback (`#14161A` ~96%) ready — it is also needed when transparency effects are off
in Windows Settings.

Fonts and spacing carry no playback risk. Segoe UI Variable is a safe default on Windows 11.

### 11. Validate across both backends

**No flag — and this is the step most likely to be skipped, so treat it as mandatory.** The
current icon set differed between backends on **8.1% of the play glyph's pixels at max delta 29**
purely from two resamplers reconstructing the same art at a fractional offset, until the layout
was pixel-snapped. `overlay.ps1` and `banddiff.ps1` exist for this; take backend diffs in **bar
mode**, because the floating overlay's fade state otherwise lands inside the band and read 9.1%
on the first attempt.

**Note the DPI line has changed since this roadmap was written**: 125% and 175% were withdrawn
by owner decision on 2026-08-15 and the second display is disconnected. 100% and 150% are
validated on hardware. Do not re-propose the rest.

### 12. Frameless window — later

**Correctly deferred, and it is the highest-risk item on the list.** Flagging why, so the
deferral is made on the real reason: Trace already handles `WM_SIZING` (the aspect lock, which
constrains the drag rect rather than correcting afterwards) and `WM_DPICHANGED` (the reshape
that fixed the pillarboxing bug). A frameless window adds `WM_NCHITTEST` and `WM_NCCALCSIZE` to
the same `nativeEvent` path and can break both — and both are signed-off geometry.

---

## Codec roadmap

H.264, ProRes and HEVC all decode today through FFmpeg, so the first three are already done.
MPEG-2 is native and free.

**Flag — AV1 is a build decision, not a code one.** The shipping FFmpeg is a deliberately
minimal LGPL build with **zero `--enable-lib*` tokens** — no dav1d, no libaom. AV1 would fall to
FFmpeg's native decoder, which is substantially slower than dav1d. If AV1 matters, that is a
change to `scripts/build-ffmpeg/build-minimal-ffmpeg.ps1` with a licence review and a size cost
(the rejected prebuilt was 104MB against the current 20.8MB), decided before any AV1 testing.

The instruction not to depend on Microsoft Store codec extensions is right and is already how
Trace works.

---

## The delivery, as verified

`assets/source/260817-trace-ui-v2/Trace Media Player Icon.zip` — 102 entries.

**The ten interface glyphs are a clean drop-in.** Every one is `name.svg` + `name-24.png` +
`name-48.png` at exactly 24×24 and 48×48, correct alias names including the two fullscreen files
whose disk names differ from their aliases, no interaction-state variants. `verify_trace_assets.py`
passes on `assets/interface/**` and the app-icon PNG and SVG sets.

**Two things are missing and both are declared in the package's own handoff**: `trace.ico` and
`trace.icns` are not delivered as compiled binaries and must be built from the supplied PNG sets.
`trace.ico` is embedded through `app/trace.rc`, so **CI's asset check fails until it exists** —
compile it in the same commit.

**The source package carries `volume` and `loop` glyphs** for features that do not exist. They
belong in `source/` only; copying them into `assets/interface/` would fail `--strict` on purpose,
which is the "artwork follows behaviour" rule enforcing itself.

**Housekeeping — the folder name contained a backtick** (PowerShell's escape character, so any
harness or script referencing the path would misbehave in non-obvious ways). **Renamed to
`260817-trace-ui-v2` before first commit, 2026-08-17.**

---

## The swap is DONE (2026-08-17) — the redesign is not started

Three-commit session: the package committed untouched under the renamed folder; the ten glyphs
and the app-icon PNG/SVG sets swapped in place with no `.qrc` or code change; `trace.ico` and
`trace.icns` compiled from the delivered PNG sets, mirroring the existing containers exactly
(five PNG-compressed ICO entries 16/24/32/48/256, seven ICNS PNG entries
icp4/icp5/ic12/ic07/ic08/ic09/ic10). `volume` and `loop` stayed in `source/` only.

Verified: `verify_trace_assets.py assets --app-icon --strict` exit 0 · local build green with
all five ICO payloads and the new glyphs confirmed **inside the built `Trace.exe`** by byte
search · cross-backend `overlay.ps1` on both renderers with `08-mid-drag` reading **0 px, max
channel delta 1** — the recorded standard, so the new art needed no pixel-snapping work — and
`banddiff.ps1` in bar mode reading 0.12% / max delta 29, which is the video band's own backend
difference class, not the artwork. (Display this session: 1920x1080 @ 59.999Hz — figures are
not comparable to physical-panel records.)

**No roadmap step beyond the swap was begun.** Steps 2 and 4 change the video rect and need
their own commit and a re-baseline; that is the next session's decision.
