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
