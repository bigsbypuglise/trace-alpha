# UI redesign roadmap — owner's plan, with implementation flags

Owner roadmap received 2026-08-17, reproduced below with flags attached. The flags are not
objections; they name where a step re-opens a signed-off decision, invalidates a recorded
baseline, or touches the presentation path. The owner's guardrail governs: *does this make
Trace faster, clearer, or simpler? If not, do not add it.*

Design package: `assets/source/260817-trace-ui-v2/` — see "The delivery" at the end.

**THE ROADMAP IS COMPLETE as of 2026-08-21.** Steps 1–11 are built and measured; step 12,
the frameless window, is **closed as declined** by owner decision — the native Windows title
bar stays, for Snap, Aero Shake, Win+arrow, multi-monitor and accessibility. Drag-anywhere-in-
the-picture to move the window was declined in the same decision. See step 12 below for both,
and for the two interim decisions that closing it settles. There is no open step and no open
owner question on this roadmap.

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

**DONE, exactly this way, 2026-08-17 — AND THEN AGAIN AT STEP 7.** The 2026-08-17 pass was
three commits (`5ff6431` message surface, `635656a` HUD default, `c63aba4` status bar) and one
re-baseline immediately after, taken on 1920x1080 @
59.999Hz and recorded in `CLAUDE.md`'s session block: cadence flat across all seven files,
`-SnapRelease` `delta 0`/`hitch 0`, reversal `hitch 1`, both lifecycle legs, **25 of 25
transitions**, and the §4 opening geometry re-measured per shape in the shipping (no-HUD,
no-status-bar) configuration. **That block is NO LONGER the standing reference**: step 7 (2026-08-18,
`10a7fba`) moved the menu bar out of the layout as well, `chrome` went `0x21 → 0x0`, every §4
opening size moved again and the video rect grew by the menu bar's height. The **step 7 block in
`CLAUDE.md` is the standing reference**, and it was taken with a control built from `d9d4d98`
beside it. Physical-panel figures remain valid as records but not as comparisons.

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

### 4. Edge-to-edge media — **DONE.** Status bar 2026-08-17 (`5ff6431` + `c63aba4`); menu bar at step 7, 2026-08-18 (`10a7fba`)

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

~~**The menu bar stays until step 7 builds the transient chrome that will hold it**~~ — **DONE
2026-08-18 at step 7 (`10a7fba`).** The bar moved into a strip that floats over the picture, so
`windowChromeLogical()` now reports **`chrome 0x0`** on every shape and the client area is
entirely picture. Every §4 opening size moved with it and three of the four shapes now reach the
design's area cap instead of being cut off by the work-area bound (1:1 774 → 960, 4:5 619x774 →
859x1074). The re-baseline is in `CLAUDE.md`'s step 7 block and **that block is the standing
reference**; the 2026-08-17 one is a record of the pre-step-7 chrome.

### 5. Bottom transport overlay — **DONE 2026-08-18.** Both blocking questions were answered by the owner first.

Built as the design package's edge-to-edge strip, replacing the floating 460×84 panel. Nine
controls in the conventional pro order — `|◀ ◀◀ ▶ ▶▶ ▶|` then mute, the timeline between its two
readouts, then fullscreen, a separator and share — with the position and duration driven by spec
phase 7's four existing modes and no fifth format anywhere.

**THE TIMELINE IS DRAWN, NOT REWRITTEN, and that was the one thing this step could get badly
wrong.** `timelineSlider_` is still the entire scrub state machine: the strip's track is a
picture of it, and pressing the track still runs `setSliderDown(true)` / `setValue()` /
`setSliderDown(false)` through `OverlayHooks`, exactly as the floating panel did since phase 6.
Nothing computes a target. Confirmed in the code before the control was designed and confirmed
again by measurement afterwards — `scrub -SnapRelease` on 4444 reads `target 261 shown 261
delta 0` full-res planar, identical to the control.

**THE PANEL GEOMETRY IS SUPERSEDED, KNOWINGLY, AND IT IS RECORDED AS SUPERSEDING RATHER THAN
DRIFTING.** Spec phase 6 settled 460×84 with 44×44 play and 34×34 utility controls and the owner
signed it off with *"no tuning is wanted"*. This step replaces those numbers with the handoff's
**56px strip, 40px play, 36px other controls, radius 6, track 4px (6 on hover), thumb 13px (16
while scrubbing), accent only on the played track and the thumb ring**. **`kFadeMs` and
`kAutoHideMs` are NOT superseded** and are untouched.

**Note the design package disagrees with itself on the sizes and the handoff wins.** Its
`HANDOFF.md` "Geometry in the mockups" line specifies 56/40/36; the mockup MARKUP renders the
same strip at 52/38/34. The handoff is the spec and the markup is a rendering of it at a demo
window size. The arrangement — buttons, position, track, duration, fullscreen, separator, share
— is the markup's exactly.

**FOUR GLYPHS HAD TO BE AUTHORED, AND THE ROADMAP'S PREMISE FOR ONE OF THEM WAS WRONG.** Decision
2 says *"the design package already carries `volume` / `volume-low` / `volume-muted` art"*. It
carries **one** `volume` glyph, in `source/` only, and no muted variant; and it ships **no
start/end art at all**. The retired 260807 set is no help either — **its `prev-clip` is a DOUBLE
TRIANGLE**, the same shape as `rewind`, so using it would have put one glyph on two controls.
So `go-to-start`, `go-to-end` and `volume-muted` are new, derived from the delivered masters
rather than invented beside them (same viewBox, same `#FFFFFF`, same 1.6 round stroke;
go-to-start and go-to-end share `rewind`'s 6.8..17.2 vertical span and go-to-end is an exact
mirror of go-to-start about x=12), and `volume` is the package's own master promoted out of
`source/`. **There is deliberately no `volume-low`**: with no slider, a third state would be art
with no behaviour behind it. `verify_trace_assets.py` demanded all eight PNGs with no edit to
itself — the derived set went **29 → 37**.

**THE RATE CHIP MOVED, AND THE GEOMETRY FORCED IT.** It sat at the old panel's top-left, in an
84px-tall panel with an empty corner. A 56px strip has no empty corner — the chip's ~27px would
land on Go to Start — so "inside the panel" stopped being a position that exists. It is now
**centred above the strip**, which is where the approved package's §6 puts it and which phase 8
had already recorded as the thing not yet done. Only the position is taken; §6's own padding,
radius and 900ms/200ms timing remain unimplemented.

**Home and End are bound**, as the decision requires, through the same `goToFrame()` exact Step
landing Go to Frame and Go to Timecode already use. **Mute was promoted from a `ShortcutTable`
key row to a shared `QAction`**, because a button and its key must trigger one action and the
accessibility proxy takes its name from that action. Neither key was bound before.

**Nine accessibility proxies, in left-to-right reading order**, with the four new controls
**interleaved rather than appended** — appending would have announced Go to Start, the leftmost
thing on the strip, after the timeline. Mute and Fullscreen are `CheckBox`, which is the honest
role for a control that reports a state.

**LOOP IS BUILT, AS A THIRD OWNER DECISION TAKEN AFTER THE FIRST PASS.** It is the tenth
control, after Mute, where the design's own markup puts it, and it runs the shared checkable
`loopAction_` phase 14 built. **It is the one control whose state is not a second glyph** — the
package ships one loop glyph where it ships a volume/volume-muted pair, so ON is the accent and
OFF is the neutral ink of its neighbours. Two glyphs would have meant inventing artwork.

**Two more owner decisions closed the step, and both are settled rather than defaults.** The
three glyphs authored here **stay** — accepted "for now", which makes them shipped artwork and
not placeholders, though they remain a drop-in swap if a designer draws replacements. And
**disabled controls stay visually unchanged**: with no media, Go to Start and Go to End are
disabled actions drawn at full brightness and the click is refused by the action. That was
already true of every control predating this step and the package supplies no disabled
treatment. **Neither is an omission to fix.**

---

**The two owner decisions and the original flags are retained below as the record of
what was asked for, not as open work.**

**DECISION 1 — "previous / next" means GO TO START and GO TO END.** Not clip navigation (there
are no playlists to navigate), not frame step (keyboard-only by the phase 4/5 owner decision,
and the approved package ships no frame-step glyph on purpose), not time skip (less useful than
scrubbing in a tool where people inspect specific frames). `|◀ ▶|` means start/end in Premiere,
Resolve and Avid, so the glyph reads correctly, and it is cheap: two **exact seeks through the
existing Step path**, no new decode behaviour. **Bind `Home` and `End` at the same time** —
neither is bound today, and a button without a key is the asymmetry `ShortcutTable` exists to
avoid. The resulting layout is the conventional pro one: `|◀ ◀◀ ▶ ▶▶ ▶|`.

**DECISION 2 IS REVERSED (owner, 2026-08-20, on tester feedback): the inline volume slider
IS BUILT** — the designer's option 1b, package `assets/source/Trace_AudioSlider.zip`, record
in `docs/volume-slider.md`, behind `TRACE_VOLUME_SLIDER` (0 restores the mute-only button
exactly). Recorded as the owner deciding again with testers' evidence, not as drift. The
decision's own guardrails were followed to the letter: volume is a gain on the sink, never
the clock, and `M` still works. The text below stays as the record of what step 5 decided.

**DECISION 2 — a MUTE BUTTON, and NO volume slider.** `M` already mutes and has no visible
control, so the button closes a gap rather than adding a feature, and the design package already
carries `volume` / `volume-low` / `volume-muted` art. A slider is more surface in the panel being
redesigned, needs hit-testing, an accessibility proxy and persistence, and the owner's own
guardrail answers it — artists ride system volume or an audio interface. **Note the three-state
art implies a level: with no slider, only `volume` and `volume-muted` are meaningful.** If a
slider is ever added, it is a **gain applied to the sink** and must not touch the audio master
clock — audio owns rate and position during 1× playback, and that is what removed the hold/skip
churn.

**THE SIGNED-OFF PANEL GEOMETRY IS BEING SUPERSEDED KNOWINGLY.** `kFadeMs`, `kAutoHideMs` and the
**460×84 panel with its 44×44 play and 34×34 utility controls** were signed off at phase 6 with
*"no tuning is wanted"* — so those were settled numbers, not defaults. This step replaces them
with the design package's edge-to-edge strip (**56px tall, full window width; play/pause 40px,
other controls 36px, radius 6; timeline track 4px and 6px on hover, thumb 13px and 16px while
scrubbing; accent only on the played track and the thumb ring**). That is a deliberate owner
decision, not a drift — record it as superseding the phase 6 sign-off rather than quietly
changing the constants. **`kFadeMs` and `kAutoHideMs` are NOT superseded** and stay as they are.

**THE TIMELINE IS THE ONE PLACE THIS CAN GO BADLY WRONG.** `timelineSlider_` is a real `QSlider`
and **is the entire scrub state machine** — phase 6 took the docked bar out of the layout but
kept the slider alive precisely because of this, and the composited overlay *drives the real
slider* rather than replacing it. A custom-drawn timeline must keep doing exactly that. If it
becomes an independent widget that computes its own target, every scrub guarantee goes with it:
exact release, latest-target-wins, the drag shuttle, the press-lands-exactly result, and the
`-SnapRelease` `delta 0` that every phase since 6 has re-measured. **Draw a new timeline; do not
write a new one.**

**Two rendering warnings, because the last two sessions each found a bug in this exact path.**
A mechanical rewrite of the D3D11 quad loop drew nothing while the CPU backend was fine
(`5ff6431`'s first cut), and a native sibling of the surface window corrupted every quad after
the first (`10a7fba`). This step is the largest change to that path yet. **Build it
incrementally, verify the panel draws on both backends after each stage, and keep a control
build to bisect against** — reading the code harder did not find either of those.

**Original flags, retained as the record of what was asked:**

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

### 6. Frames / Timecode / Seconds - **DONE. AUDITED AGAINST THE SHIPPED BUILD 2026-08-18, no code needed**

Audited rather than implemented, because everything it asks for predates this roadmap and the risk
was a later session "implementing" what already exists. All four modes reach BOTH strip readouts
through one `readoutTextAt`, and the strip and the HUD cannot print different values because they
call the same function. Measured on 4K ProRes 4444 at frame 1: Frame `1` / `261` - Seconds
`0.083` / `10.875` - Elapsed `00:00:00:03` / `00:00:10:21` - Timecode `00:00:01:16` /
`00:00:12:09`. **The last row is the check that matters** - the file's own recorded `00:00:01:12`
start plus the elapsed - so the source timecode is READ and not synthesised.

`hasSourceTimecode_` is still the single gate across ten read sites: `setReadoutMode` refuses
SMPTE with a reason, the menu item and Go to Timecode are enablement-gated on it, and `openPath`
resets out of Timecode when new media carries none.

**Observation, not a defect:** in Timecode mode the right-hand readout is the source timecode AT
the last frame - an end timecode rather than a length. Consistent by construction (both readouts
are "the value at frame N") and the pro convention, but worth having written down.

**A DEFECT WAS FOUND BESIDE IT AND IS FIXED (`b2a901b`): the readout mode did not repaint the
strip.** `setReadoutMode` rebuilt the HUD, which is a separate widget in the layout, while the
strip's readouts are quads built inside the viewer's paint. `keyPressEvent`'s `revealOverlay()` is
not enough - `OverlayModel::startAnimation` returns early once the fade has settled, so `reveal()`
on a strip that is up and idle schedules nothing. Measured paused with the strip revealed:
pressing `E` and choosing the same item from the menu both left it reading `0` / `261` at **zero
differing pixels** while the HUD switched to `Readout: Elapsed`; four changes in a row never moved
it, and an auto-hide plus re-reveal then showed the correct values - a repaint fault, not a wiring
one.

**Flag, retained - do not collapse Elapsed and Timecode into one "Timecode" mode.** Trace
has `F` Frame Count, `S` Seconds, `E` Elapsed and `T` **source** SMPTE — and the entire point of
that phase was that *elapsed time is not timecode*. `T` is refused with a reason on a file that
carries no start timecode, rather than inventing `00:00:00:00`.

**Flag — do not collapse Elapsed and Timecode into one "Timecode" mode.** `hasSourceTimecode_`
is the single gate on everything SMPTE and must not grow a second answer. Keep four modes and
present them clearly; that is a naming and menu question, not a behaviour change.

### 7. Transient top chrome — **DONE 2026-08-18 (`10a7fba`)**

Built exactly as the flag below asks: the menus are real `QMenu`s on a real `QMenuBar`, owned by
the window, and only the BAR is transient. `src/ui/TopChrome.*` holds the real menu bar beside
the brand mark, the wordmark and the filename, and is shown and hidden by the SAME reveal state
`OverlayModel` keeps for the transport — one idle timer, one hold list, one fade state, through
`OverlayHooks::setChromeRevealed`. `holdVisible` covers an open popup and the menu bar's own
keyboard focus by construction, so a menu being navigated cannot fade the chrome out from under
it. Gated on `barIsDocked_`, so `TRACE_TRANSPORT_BAR=1` keeps the old menu bar and the old
geometry.

`uiatree.ps1` finds a **`MenuBar` with five `MenuItem`s on real rects** beside the five transport
proxies, for free, which is the whole point of not drawing it. Alt and every mnemonic still reach
a hidden bar — Qt declines a `grabShortcut` whose widget is not visible, so an event filter on
the window HANDLE reveals the chrome before the shortcut map is consulted; measured with
`menushot.ps1` from a hidden strip, all five open their menu.

**Two things this found that outlive the step.** A **native sibling of the D3D11 surface window
corrupts that surface's own overlay pass** — the transport's first quad draws and every quad
after it renders as if its sampled colour were zero, with the atlas texture read back from the
GPU byte-identical to its source; parenting the strip one level up removes it entirely, so the
distinction is where in the HWND tree the native window sits, not whether it is native. And the
strip is **opaque**, because §18.4 measured that every native-surface variant loses translucency;
that is the design package's own stated fallback (`#14161A`) and the blur it falls back from is
step 10 below.

**Fullscreen gets the same strip**, verified working there. The design's screen-2 shows a
different one — 52px, filename in bold beside a dimmer `1920x1080 - 24 fps`, no menu bar — which
is a second layout plus new content, so it is left as an owner decision. One strip keeps the
menus reachable in fullscreen, which is strictly more functional than the mockup.

**Retained, because it is what the shape was chosen for:**

**Flag — this is the accessibility risk of the whole pass.** The menu bar is currently the
main accessible surface: real `QMenu`s, exposed to screen readers for free. The composited
overlay has **no widget tree** — that is why phase 14 had to build a UIA proxy tree by hand
after phase 6 made the overlay the only transport. Moving menus into a transient overlay repeats
that mistake at a larger scale.

The workable shape is: keep menus as real `QMenu`s owned by the window, and make the *bar* that
displays them transient rather than reimplementing menus as drawn quads. `holdVisible` already
covers open popups, so an open menu keeps the chrome revealed by construction.

### 8. Auto-hide - **DONE. AUDITED AGAINST THE SHIPPED BUILD 2026-08-18, no code needed**

`kAutoHideMs = 2000` sits at the top of the roadmap's own 1.5-2s band and `kFadeMs = 165` is
untouched by step 5, which superseded the panel geometry and explicitly not these two. All three
hold cases the roadmap names are covered in the timeout handler: pointer over a control
(`hover_ != Region::None`), timeline dragging (`draggingTimeline_`), and popup open (through
`holdVisible` -> `activePopupWidget() || activeModalWidget()`, which covers menus, tooltips and
modal dialogs alike). There is a fourth the roadmap did not ask for - a child widget holding
keyboard focus, scoped to `focus->window() == this` so a modeless Movie Inspector cannot hold the
transport up indefinitely.

**Built, measured and signed off.** `kAutoHideMs = 2000`, `kFadeMs = 165`, and the owner's
sign-off recorded *"no tuning is wanted"* — those are settled numbers, not defaults.

**Flag — 1.5s vs 2s is a decision, not an implementation detail.** Changing it re-opens that
sign-off. The hold rules the roadmap asks for (pointer over controls, timeline dragging, popup
open) are all already implemented in `OverlayHooks::holdVisible`.

### 9. Fullscreen presentation mode - **DONE, AND THE OPEN QUESTION IS CLOSED BY OWNER DECISION 2026-08-18**

**ONE STRIP, WINDOWED AND FULLSCREEN, WITH THE MENUS REACHABLE - and the design package is
deliberately ignored on this point** (owner, 2026-08-18). The design's screen-2 shows a different
fullscreen strip - 52px, filename bold beside a dimmer `1920x1080 - 24 fps`, no menu bar - and it
is **not being built**: no second layout, no metadata line. One strip keeps every menu reachable in
fullscreen, which is strictly more functional than the mockup, and that is the decision.
**Do not re-propose screen-2.**

Audited against the shipped build: Escape is a separate `QAction` enablement-gated on fullscreen,
so a windowed Escape is not consumed at all; geometry is captured BEFORE the state change with the
maximized bit recorded separately, so a maximized window returns maximized; the monitor rule is
Qt's own and was validated on real hardware at plan section 20.4 on 2026-08-14; and the cursor's
two mechanisms are applied together in one function, which is what makes "verify by the handle,
not the flag" a property rather than a convention.

**Largely built at phase 6** — Escape exits, geometry restores, the monitor rule holds, and the
cursor hides after inactivity. Note the cursor is hidden by *two different mechanisms* on the
two backends (`Qt::BlankCursor` on CPU, `SetCursor(nullptr)` answering `WM_SETCURSOR` on D3D11),
so `GetCursorInfo` sees only one of them — verify by the handle, not the flag.

### 10. Windows 11 visual conversion - **DONE 2026-08-18. Typography `d91f026`; the blur SHIPS ON at `a4c6bb2`, honouring Windows' transparency setting. Mica/Acrylic cannot do it, route 2 can**

> **ROUTE 2's PAINTED BLUR IS REMOVED, 2026-08-19 (owner feedback item 8, option B).** The
> strip rests at layered alpha 215 now — real translucency on d3d11 via item 11's
> `LWA_ALPHA` mechanism — and the 155..255 alpha sweep measured that a resting alpha UNDER
> the painted blur double-counts the video (the blur is itself a bright copy of it) and
> washes the menu labels out, while the solid fallback under the same alpha reads better
> than the blur ever did over bright footage. `StripBackdrop.*`, `TRACE_STRIP_BACKDROP`,
> `stripbackdrop.ps1` and `backdropcost.ps1` are gone; the `EnableTransparency` tri-state
> read survives inside `TopChrome`, gating the resting alpha instead of the blur, and the
> HUD's `backdrop` field is now `strip`. Everything below is retained as the record of what
> was built and measured, not as a description of the shipping strip. Record in
> `docs/ui-feedback-260818-progress.md`, item 8. **The Mica/Acrylic finding above is
> untouched and still binding.**

**DWM BACKDROP EFFECTS REACH THE TITLE BAR AND NOTHING ELSE.** Applied to Trace's live main
window: `DwmExtendFrameIntoClientArea(-1)`, `DWMWA_SYSTEMBACKDROP_TYPE` at all four values, the
undocumented `SetWindowCompositionAttribute(ACCENT_ENABLE_ACRYLICBLURBEHIND)` and legacy
`DwmEnableBlurBehindWindow` **all return S_OK** and change **rows 1..30 only** - 37,987 differing
pixels there and **zero anywhere in the client**, both strip bands included. Mica specifically
changes nothing at all. The HWND tree says why: `TraceD3D11Surface` is 1280x720 covering **100% of
the client**, so a backdrop has nothing to show through, and every ex-style is `0` - no
`WS_EX_LAYERED`, no `WS_EX_NOREDIRECTIONBITMAP`.

**THE DEEPER REASON IS INDEPENDENT OF THE SWAPCHAIN AND IS THE PART TO CARRY.** DWM backdrops blur
what is behind the **WINDOW** - the desktop. The design's mockup uses CSS `backdrop-filter:
blur()`, which blurs what is behind the **ELEMENT** - the video. They are different effects, and no
value of `DWMWA_SYSTEMBACKDROP_TYPE` turns one into the other. This would hold even if the client
area were reachable, which is why it is the durable half of the finding.

**ROUTE 2 IS BUILT AND MEASURED FLAT (`efa3160`), AND IT SHIPS ON AS OF `a4c6bb2`.** The
strip paints a tiny blurred copy of the video it covers as its own background, under the package's
own `rgba(22,22,24,0.66)` -> `0.04` scrim - so the design's look is reached **while the menu bar
stays a real `QMenuBar` in a real native window**, which is what route 1 (redraw the strip as
composited quads) would have had to trade away. **Cost is set by the OUTPUT, not the input**: a
48x6 grid at 4x4 source reads a cell is 4,608 samples per frame at 8K as at 720p. It must never
become a downscale of the band, which at 4K would be ~438k samples and would scale with resolution.

Measured **with the chrome held revealed for the whole run and jiggled in the control too** - the
strip auto-hides after 2s, so a plain run would have measured the effect for two seconds and the
fallback for nine and reported near-zero cost for the wrong reason. 4K ProRes 4444 x2 each,
1920x1080 @ 59.999Hz, `renderer d3d11 +overlay` read off both HUDs: **99.8% on all four, 261
frames, `drop 0`, `rephase 0`, `handler>budget 0 of 260`, `hitch 0`, `paints 313/262` identical**.
The captures confirm the strip really was revealed and really was drawing the blur in the measured
runs, so the flat result is not a check that could only report one thing. Both backends draw it;
cross-backend strip band 1847 px of 72576 at max channel delta 7, under the video band's own
backend class because the scrim attenuates it.

**THE PROTOTYPE IS GATED ON THE REVEAL STATE NOW (2026-08-18, `2a3c634`), so the version that
would ship is strictly cheaper than the one measured above.** It sampled on every presented
frame whether or not the strip was visible; it asks `OverlayModel::chromeRevealed()` -- the same
state that drives `OverlayHooks::setChromeRevealed` -- so an ordinary playback run, where the
chrome is hidden for nine seconds in eleven, samples nothing for those nine.

**THE GATE HAS ONE FAILURE MODE AND IT TOOK FOUR ATTEMPTS TO REPRODUCE.** A gate that stops
sampling must say so, because `TopChrome` keeps the last image it was given; publishing null when
hidden costs one `std::function` call, since `setBackdrop`'s null-to-null check returns
immediately after the first. But null is only published when the sampler **runs**, and it only
runs when a frame arrives -- so a reveal with no frame behind it draws the solid fallback on a
build meant to blur. `MainWindow::syncTopChrome()` refreshes it too: one sample per reveal, hide
and media change against one per frame.

**Three earlier candidates for that failure all PASSED on a build with the fix removed**, and
each corrected something. Stepping frames "while hidden" tested nothing, because `keyPressEvent`
calls `revealOverlay()` -- the arrow keys changing the frame were themselves holding the chrome
up. Pausing reveals the chrome *and* re-presents a frame. **Close Media already published null**,
because `closeMedia` calls `setFrame(VideoFrame{})` rather than `clearImage()`, so the
outgoing-file blur that was expected to need fixing never existed. What is left is the only
reveal that delivers no frame: **playback running off the end, then a reveal by pointer movement
alone** -- control `hsd 0.000` (the fallback) against fixed `hsd 3.999` (the blur).

**THE COST MEASUREMENT IS WIDENED TO THE TWO FILES THAT BOUND THE SET (`586f2b9`)**, plus 4444
re-taken on the same display so all three are one record. Physical panel 5120x1440 @ 239.999Hz,
`TRACE_NO_AUDIO`, scratch INI, x2 per configuration, same binary:

| file | backdrop off | backdrop on |
|---|---|---|
| 4K 60fps (16.67ms budget, the tightest) | 100.0 / 100.0% | 100.0 / 100.0% |
| 4K ProRes 4444 | 99.8 / 99.8% | 99.8 / 99.8% |
| 8K ProRes 4444 XQ (`TRACE_RT_DROP=0`) | 50.8 / 53.5% | 52.2 / 53.1% |

`drop 0`, `rephase 0` and `handler>budget 0` on both 4K files. **On the 8K the backdrop rows are
nominally HIGHER, and the spread WITHIN `bd=0` alone is 2.7 points against 0.5 between the
configuration means** -- the first run pays a cold decoder at `dec 49.42` against ~45 -- so the
honest reading is "inside run-to-run variance", not "faster". `TRACE_RT_DROP=0` on the 8K because
the drop policy adapts to load: with it on, added cost would show as a changed drop count rather
than changed throughput, which compares two policies rather than two amounts of work.

**Two things about that method.** The chrome is held up by **parking the pointer over the
picture**, not by jiggling: the auto-hide holds while `hover_` is a region, so a stationary
pointer inside the client keeps the chrome up indefinitely -- measured still up at 6s at the
client centre and in the corner, against ~2.6s to hide with the pointer outside. That holds the
strip up for the whole run and generates **no input during it**, which removes the jiggle from
both sides rather than balancing it. And **every run proves its own premise from the same capture
the figures come from**: `strip hsd` is horizontal variation across a strip row, exactly 0 for
the design's purely vertical fallback gradient, and it reads 0 on all six `bd=0` rows and
3.995 / 2.946 / 17.15 / 16.151 on the `bd=1` rows. Harnesses: `scripts/measure/stripbackdrop.ps1`
and `scripts/measure/backdropcost.ps1`.

**Note the table measures the WORST case deliberately** -- holding the chrome revealed throughout
defeats the gate on purpose, so it is an upper bound rather than a typical cost.

**IT SHIPS ON (owner, 2026-08-18, `a4c6bb2`), AND IT HONOURS WINDOWS' TRANSPARENCY SETTING WHILE
DOING SO.** `TRACE_STRIP_BACKDROP` is three-valued now -- `0` forces off (the rollback), `1`
forces on (the override), unset reads
`HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\EnableTransparency`.
**That is the design package's own instruction rather than an addition to it**: it supplies the
solid `#14161A` as "the fallback when transparency effects are disabled in Windows Settings", so
it already expected the setting to be read, and honouring it is what makes the Fluent direction
Windows-**native** rather than Windows-looking. The registry read is **tri-state**, because a
wrong key path and a machine that has never touched the toggle produce the same boolean -- the
dev HUD's `backdrop` field prints `on` against `on (unset)` so the two are different readings. **Closed on hardware 2026-08-18**: with the setting off the HUD
reads `off (windows)` and the strip is byte-identical to the forced-off fallback, forcing it on
against a Windows "off" works, and flipping the setting while Trace was running updated the strip
with no restart.
Full record in `CLAUDE.md`. **DirectComposition (`WS_EX_NOREDIRECTIONBITMAP`) and rebuilding the
strip as video quads are both explicitly NOT being pursued** (owner, 2026-08-18).

**So the rest of step 10 is typography and spacing, which carry no playback risk** - exactly what
the original flag anticipated as the fallback.

**Original flag, retained: Mica/Acrylic backdrop blur is the one item here with real presentation
risk.** The
video is presented into a **child HWND with a flip-model swapchain**. DWM backdrop effects apply
to the window backdrop and interact with the composition model; blur behind a swapchain-presented
child is at best ineffective and at worst forces a different composition path. **Prototype it
against a 4K ProRes 4444 playback run before committing to it in the visual language**, and have
the solid fallback (`#14161A` ~96%) ready — it is also needed when transparency effects are off
in Windows Settings.

**THE TYPOGRAPHY HALF IS DONE (`d91f026`): `src/app/Theme.*`, one home for the application
font, the palette and the popup-menu surface.** `main.cpp` had a hand-rolled grey palette;
`TopChrome` keeps its own screen-1 strip colours because those describe one surface, and
everything else inherits from `Theme`.

**QT AND GDI DISAGREE ABOUT WHETHER `Segoe UI Variable` EXISTS, AND THE FIRST BUILD SHIPPED THE
WRONG ANSWER.** GDI lists only the three static optical cuts -- `Display`, `Text`, `Small` --
and nothing under the plain name, so the first version concluded the design named a family
Windows does not have and mapped it onto `Segoe UI Variable Text`. `QFontDatabase::hasFamily()`
declined that and the application silently ran on **Segoe UI**. **Qt 6 enumerates through
DirectWrite and exposes the VARIABLE font under its typographic family name**: it sees exactly
one match and it is `Segoe UI Variable`, the design's own CSS name. Both are in the chain now,
the design's name first, so a Qt build that enumerates the GDI way still lands on the right
optical size rather than on plain Segoe UI.

**Which cut that is was settled by the package's own embedded TTF, not by guidance.** The
mockup bundles a **1.8MB font** -- most of why that one file is larger than all of `src/` -- and
its tables read axes `wght` 300..700 and `opsz` 5..36 with a **default optical size of 10.5**,
which is squarely `Text` and agrees with the design's 12px UI type.

**THE DEV HUD REPORTS THE FAMILY THAT ACTUALLY RESOLVED (`font ...`), and it earned itself on
its first run** by reading `font Segoe UI` on the build above. A fallback to Segoe UI looks very
nearly right and no screenshot separates the two -- the same silent-degradation class `renderer`
and `planar` are reported for. **No point size is set**: pinning the design's 12px would look
right at 100% and wrong at every other scale factor, and would override the user's Windows
text-size setting. That is `TopChrome`'s own reasoning for the menu bar, applied application-wide.

**THE ACCENT GOES WHERE WINDOWS PUTS ONE AND NOWHERE ELSE.** `#5AC8E8` becomes `Highlight` and
`Link` -- text selection -- with `HighlightedText` **dark**, because the accent has a high
relative luminance and white on it is unreadable in the inspector's source-path field. It is
deliberately **not** the menu highlight: the design uses the accent on the played track and the
thumb ring only, recorded twice above as an owner decision, and a popup lighting up cyan under a
menu bar lighting up white would be two languages on one gesture. Popups reuse step 7's own
`rgba(255,255,255,0.10)`.

**POPUP MENUS ARE THE SURFACE STEP 7 EXPLICITLY LEFT HERE** -- its comment says a bare `QMenu`
rule there would restyle every popup the bar owns. The values are the design's rather than new
ones: `#1A1B20` is the strip's lighter stop, the 1px `rgba(255,255,255,0.09)` border and 8px
radius are screen-1's window border and radius, and the 3px/8px item padding is the menu bar's.
**This is the one surface the package does not show** -- all three of its screens have no menu
open -- so it is **derived**, the same position the three authored transport glyphs are in.
`scripts/measure/themeshot.ps1` puts it on screen for that reason; the corner was checked at 10x
and is cleanly rounded and anti-aliased, with no square artifact.

**Regression flat** (physical panel 5120x1440 @ 239.999Hz, so not comparable to the step 7
block; the 4444 control is the same session's own `bd=0` rows on the immediately preceding
binary): 4K H.264 cadence x2 **100.0/100.0%**, 120f, `drop 0`, `rephase 0`, `0 of 119` - 4444
cadence x2 **99.8/99.8%**, 261f, `0 of 260`, **identical to the control** - `scrub -SnapRelease`
**`target 261 shown 261 delta 0`** full-res planar, `hitch 0`, `land 0`, release 21.8ms, `ui
over-16ms 0 of 642` - **25 of 25 transitions** - `emptystate` all four modes PASS on d3d11 plus
the `-Bar` control at the recorded **641-row** stage, and **cpu identical to the pixel** -
`uiatree` MenuBar + five MenuItems on real rects and the ten controls.

**ONE FIGURE MOVED AND IT IS THE FONT BEING IN FORCE: the empty state's hint reads `169x14`
where the record says `157x13`, and the gap 44 where it says 45.** The hint is *text*. The mark
stays `59x68` with its `+10.5` optical offset and `+0.5` centring, because the mark is a bitmap.
**A build where the font had silently fallen back would still read `157x13`** -- which is what
makes this a check rather than a drift.

**The Fluent icon direction needs no work and must not be given any.** The delivered 260817
glyph set already is it -- the mockup's own `w-play` / `w-rewind` / `w-volume` / `w-loop` /
`w-full` / `w-share` paths are the art that shipped -- and swapping to `Segoe Fluent Icons`
(which is installed) would put a font glyph where a delivered asset exists, which is "artwork
follows behaviour" pointing the wrong way.

Original flag, retained: fonts and spacing carry no playback risk. Segoe UI Variable is a safe
default on Windows 11.

### 11. Validate across both backends - **DONE 2026-08-18. CONSOLIDATED, NOT REPEATED, AND IT FOUND ONE DEFECT**

> **RE-CONSOLIDATED AT HEAD, 2026-08-19 (`86f1186`, physical panel 5120x1440 @ 239.999Hz),
> because three later passes changed what this record asserts**: the owner feedback pass
> (eight strip controls, not ten — item 15; brand mark gone from the strip — item 1; the
> style home moved into `Theme::apply` — item 10), item 11's layered-window fade, and item
> 8's resting translucency. Two rows below are SUPERSEDED rather than stale: the "top strip
> with the backdrop drawing, over video" row measures a mechanism item 8 deleted, and over
> video the resting strip now differs across backends **by owner decision** (d3d11 rests
> translucent at alpha 215, cpu rests opaque — measured 5434 of 5434 samples at max delta 37
> in the item 8 record). **The strip-band difference over video is the decision, never a
> defect to reconcile.** Every "ten controls" below reads eight as of item 15.
>
> **What was already re-verified at HEAD by the item-8 session**: the whole empty-state
> window with both strips revealed still reads **0 of 972,800 px, max channel delta 0**
> across backends (the empty-state strip deliberately rests opaque to preserve exactly this
> instrument); `overlay.ps1` all legs PASS on both backends re-pointed at the eight-control
> strip; 25 of 25 transitions; `topchromefade.ps1` rest/anim/menus/loop with the cpu leg's
> PASS being "OPAQUE".
>
> **What was closed today, on the eight-control build**: popup **View** menu body **0 of
> 99,876 px, max channel delta 0** across backends (`themeshot.ps1 -Renderer` both ways —
> re-checked because item 10 moved the style home and added the `KeyboardCuesStyle` proxy;
> mnemonic underlines were in force in both captures). `uiatree.ps1` on both backends,
> pointer parked inside: **eight named controls, MenuBar, five MenuItems and the filename
> Text on identical rects to the pixel** — and the first attempt reproduced the recorded
> same-coordinate trap: a `SetCursorPos` to a pixel the pointer already occupies generates
> no input since the items-3+7 filter, so the chrome never revealed and BOTH walks read no
> MenuBar; jiggle through two points. §4 opening geometry, shipping configuration
> (`TRACE_HUD=0`): 16:9 `client 1280x720` and 9:16 `609x1083`, **identical on both
> backends**. Escape hatch health: cpu 4444 cadence **99.4% ×3** (`drop 0`,
> `handler>budget 1 of 260` — the recorded class to the digit; one additional cold-start
> rep read 94.6% with `drop 11` and did not reproduce in three later reps), cpu 4K H.264
> **100.0% ×2** (`0 of 119`, `thr frame x16`), cpu `-SnapRelease` **`target 261 shown 261
> delta 0`**, `walk 0f`, `hitch 0`, `land 0`, release 45.5ms with the landing async
> (`async 5 sync 0`). Copy Current Frame: clipboard **3840x2160** and the toast drawn with
> the chrome hidden.

Display: **physical panel, 5120x1440 @ 239.999Hz**, so no figure here compares to the step 7
block's 1920x1080 record. `renderer` was read off both HUDs before any cross-backend figure was
believed -- `d3d11 +overlay` and `cpu +overlay`, both `backdrop on`.

**MOST OF THIS STEP WAS ALREADY DONE INCREMENTALLY, AND THE JOB WAS TO FIND WHAT WAS NOT.** Every
step since 2 took a cross-backend comparison, so the audit came first and the run only closed
what it left. What the audit found is that **`abdiff.ps1` samples rows 6%..46% of the capture --
the VIDEO band -- so it cannot see either strip, the empty state or the toast.** Every
"cross-backend" figure taken with it is a statement about the picture, not about the chrome. That
is why the chrome had to be compared separately, and it is the single most useful thing this step
established.

**THE HEADLINE: THE ENTIRE EMPTY-STATE WINDOW WITH BOTH STRIPS REVEALED READS 0 OF 972,800 PIXELS
DIFFERING AT TOLERANCE 0, MAX CHANNEL DELTA 0.** That is the top strip, the brand mark and
wordmark, the real `QMenuBar`, the filename, the prism mark and its hint, and the whole
edge-to-edge transport with all ten controls, its timeline and both readouts -- byte-identical
across renderers in one measurement. It subsumes the separate step 3, 5, 7 and 10 chrome claims
at the current geometry. **Taken over the empty state, per step 5's own rule**, because a diff
taken over video cannot see a translucent strip.

**What each surface reads:**

| surface | figure | note |
|---|---|---|
| whole empty-state window, both strips up | **0 of 972,800 px, delta 0** | tolerance 0 |
| top strip band alone | 0 of 48,640 px, delta 0 | |
| bottom transport strip alone | 0 of 70,400 px, delta 0 | |
| popup menu body (View) | **0 of 94,944 px, delta 0** | `themeshot.ps1` gained `-Renderer` |
| top strip **with the backdrop drawing**, over video | 0 px above tolerance 2, **max delta 2** | better than step 10's recorded 1847 px / delta 7 |
| transport strip over video | 1.14% above tolerance 2 | show-through, not the way to measure a strip |
| `overlay.ps1` `08-mid-drag` | **0 px, max delta 0** | the recorded standard |
| video band | ~10% at max delta 12 | the long-signed-off GATE B class |

**SIX PIXELS ARE THE ONLY CHROME DIFFERENCE ANYWHERE, and they are worth writing down because
they look like the fault this step exists to catch.** Over *video*, the transport band carries
six pixels at delta 247 -- x 116..123, y 716..727, which is the play glyph's two diagonal edges:
white on one backend, background on the other. That is superficially the "8.1% of the play
glyph's pixels" class the original flag names. **It is not**, and the empty-state result is what
says so: the same glyph is byte-identical there. So it is compositing residue that only shows
over a bright background, not glyph geometry, and it is six pixels rather than hundreds.

**THE OTHER LEGS, all on both backends.** §4 opening geometry, shipping configuration, all four
shapes **identical to the pixel**: 16:9 `client 1280x720` (1.7778) - 9:16 `609x1083` (0.5623) -
1:1 `960x960` - 4:5 `859x1074` (0.7998), matching the step 7 record. `emptystate.ps1` all four
modes PASS on both plus the `-Bar` control, mark 59x68 / offset +10.5 / hint 169x14 / gap 44
identical to the digit. `overlay.ps1` every leg PASS on both, played track `0.822 -> 0`, mute
`43 of 625`, loop accent `9/89/9` on d3d11 and `9/71/9` on cpu. `uiatree.ps1` ten named transport
controls plus MenuBar and five MenuItems, **on identical rects to the pixel**. Copy Current Frame
returns 4096x2304 on both.

**Scrub and playback on the escape hatch, which is the half most likely to have rotted:** cpu
4444 cadence x2 **99.4%** (`drop 0`, `handler>budget 1 of 260`) against d3d11's 99.8%, and cpu 4K
H.264 **99.2/99.2%** against 99.1/99.2% with identical buckets -- the recorded GATE C class, `sws
16.27` against `5.09`. `scrub -SnapRelease` on 4444 lands exactly on both: `target 261 shown 261
delta 0`, `hitch 0`, release **21.3ms** on d3d11 and **91.1ms** on cpu, which is that path's own
recorded cost. Reversal drag 4K H.264 `delta 0` on both, `seeks 5` both, `rev-hit 97.3%` against
97.1%, `hitch 1` against 2. **25 of 25 transitions.**

**THE DEFECT IT FOUND: the composited toast was drawn entirely behind the top chrome strip**, and
the both-backend pass is what surfaced it -- capturing the toast on each renderer produced no
toast on either, while the clipboard demonstrably held the frame. Step 2 chose its 12px top-left
margin while the menu bar was still in the layout; step 7 floated the chrome over the picture and
did not move it. Fixed at `e002085` by reading `OverlayModel::setTopInset`, which gains its second
reader. See the `CLAUDE.md` entry for why the offset is unconditional.

**TWO HARNESS FACTS.** `uiatree.ps1` must be run with **the pointer parked inside the client** or
the menu-bar half of the walk depends on the auto-hide -- a first run read `MenuBar 0` on cpu and
`MenuBar 2` on d3d11 and looked exactly like a backend difference; held revealed, both read the
same rects to the pixel. And **any mouse/SendKeys harness is void if another window takes the
foreground mid-run**: an `overlay.ps1` run had its foreground stolen and reported `panel-mean 0`
with every interaction leg failing at once, which -- like `transitions.ps1`'s 25 identical
failures -- is a statement about the harness's inputs and not about the build.

**The DPI line stands as narrowed**: 125% and 175%, hot-plug, a live scale change and three-plus
displays were withdrawn by owner decision on 2026-08-15 and the second display is disconnected.
100% is what this pass ran at. Do not re-propose the rest.

**Original flag, retained:** the current icon set differed between backends on **8.1% of the play
glyph's pixels at max delta 29** purely from two resamplers reconstructing the same art at a
fractional offset, until the layout was pixel-snapped. `overlay.ps1` and `banddiff.ps1` exist for
this; take backend diffs in **bar mode** or over the empty state, because the floating overlay's
fade state otherwise lands inside the band and read 9.1% on the first attempt.

### 12. Frameless window — CLOSED AS DECLINED (owner, 2026-08-21)

**The native Windows title bar stays.** Owner decision, taken on its merits rather than
deferred again: the native frame is what gives Trace Snap, Aero Shake, Win+arrow, correct
multi-monitor behaviour and the accessibility a real caption carries. A frameless window would
have to reimplement all of that, and reimplementations of window management are where this class
of app goes wrong. **Do not re-propose it.**

The risk flag it was deferred on still reads true and is retained as the record of why the
decision was easy: Trace already handles `WM_SIZING` (the aspect lock, which constrains the drag
rect rather than correcting afterwards) and `WM_DPICHANGED` (the reshape that fixed the
pillarboxing bug). A frameless window adds `WM_NCHITTEST` and `WM_NCCALCSIZE` to the same
`nativeEvent` path and can break both — and both are signed-off geometry.

**Two things that were conditional on step 12 are now settled by its closing.** The top strip's
brand mark and wordmark, removed at owner item 1 (2026-08-18) as an "approved interim" on the
stated grounds that they return "with roadmap step 12, when the strip becomes the only header",
**do not return** — the strip is never the only header. The `brand-mark-*` renditions stay out
of the `.qrc` and the working copies. And the design package's screen-2 fullscreen strip, already
declined at steps 6/8/9 in favour of one strip windowed and fullscreen, keeps that ruling with no
remaining route back to it.

### 12b. Drag anywhere in the picture to move the window — DECLINED (owner, 2026-08-21)

Raised and declined in the same decision. **The picture's press gesture is already spoken for**:
it is the phase 15 pan when the picture is zoomed, and it is feedback item 13's click-activate
otherwise. A third meaning for the same press would have to be arbitrated by zoom state, which
makes the same gesture do different things depending on something the user is not looking at.

Beyond that, **Windows applications move by their chrome**, and Trace now keeps a native caption
to move by (step 12, above). And it would multiply the one measured cost of a window drag: the
modal move loop already costs the picture a ~110ms hiccup and a run of single-frame real-time
drops (`docs/audio-window-drag.md`), and putting that gesture under the pointer everywhere would
put it in the middle of scrubbing and panning. **Do not re-propose it.**

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
