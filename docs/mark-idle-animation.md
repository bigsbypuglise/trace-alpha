# The empty mark's idle animation — prototype (2026-08-21)

`TRACE_MARK_ANIM=1`, **default off**, built for the owner to judge by eye.
The design package's 18s loop for the empty-state prism mark — a spatial
gradient rotation on the inner edge plus a glow hue cycle — re-authored as
QPainter paths and gradients from `assets/interface/branding/empty-mark.svg`
(NOT `trace-play-mark.svg` beside it; the five near-identical values are the
recorded trap). Nothing ships changed: with the knob unset the mark is the
committed PNG rendition, byte for byte, and every figure below says so.

## What was built

- **The SVG's own SMIL block is the specification.** The `emEdge`
  gradientTransform rotates 150° → 510° about (520,512) over 18s; the two
  animated `emGlow` stops cycle the five-colour ladder
  (`#5AC8E8 #5B8DEF #8B6FE8 #D46FB8 #5AC8E8`, mids
  `#4FBEE8 #6F8FE0 #9F7FD8 #D48FC0`) over the same 18s, piecewise-linear over
  four segments exactly as SMIL interpolates five values. Phase 0 is the
  delivered still (the master's own t=0). objectBoundingBox gradients are
  reproduced exactly by defining the gradient in the unit square and giving
  the *brush* the bbox transform — mapping the endpoints alone is not the
  same thing for a diagonal vector, and `emBody`'s is diagonal.
- **`TRACE_MARK_ANIM_PHASE=<0..1>` pins the phase** (the
  `TRACE_TOPCHROME_ALPHA` idea): the mark renders procedurally at that phase
  with **no timer** — one deterministic raster. Values outside [0,1) wrap
  rather than clamp; a phase is an angle.
- **The whole run gate lives in one place**
  (`OverlayModel::syncMarkAnimation`): knob on, phase not pinned, **no
  picture on screen** (`mediaPresent_`, the renderers' own answer — so any
  video decoding stops it by construction, while audio-only playback keeps
  the mark animating), and **host visible and active** (ViewerWidget pushes
  Show/Hide/WindowActivate/WindowDeactivate edges; the timer tick re-checks
  as a belt). Freezing keeps the phase; resuming restarts the delta clock so
  a pause cannot bank a jump.
- The timer asks for 33ms and Windows' coarse timer delivers **~20–21
  ticks/s**; deliberate — an idle animation has no claim on a precise timer,
  and the phase advances from measured elapsed time, so the 18s period is
  exact regardless of tick cadence (measured 0.278 → 0.555 in 5.0s).
- `TRACE_MARK_ANIM_LOG=1` prints ticks/s, rebuild avg/max and phase every 5s.

## Measured (2026-08-21, dev box, 1296x799 default window)

- **Cross-backend at a pinned phase — the instrument survives**: d3d11 vs cpu
  at `TRACE_MARK_ANIM=1 TRACE_MARK_ANIM_PHASE=0.25`, whole window below the
  OS title bar: **0 of 983,664 px differ, max channel delta 0.** The empty
  state remains the one surface byte-identical across both renderers, which
  is the working instrument for every chrome comparison since step 3 — an
  unpinnable animation would have destroyed it, and the pin is what keeps it.
- **Cost**: animating, focused, idle: **~20–21 repaints/s, rebuild avg
  0.45–0.59ms (max 0.86ms warm, ~2.5ms cold first), 2.97% of one core**
  (296.9ms CPU over 10s). Same window, knob off: **0.0ms CPU over 10s** —
  the empty state stays fully idle, so the default configuration pays
  nothing.
- **The gate works in every direction measured**: deactivating the window
  stopped the ticks outright (zero log lines across 11s, resumed at ~21/s on
  refocus, phase frozen across the gap — 0.321 held, not jumped); a modal
  file dialog deactivates the viewer's window and stops it too; **audio-only
  playback keeps it running** (~21.5 ticks/s through a playing MP3, the
  item-1 interaction: the gate is "no video decoding", not "the empty state
  is showing"); **opening a video stops it** and it stays stopped (line
  count flat across 14s of an open 1080p file).

## Regression (2026-08-21, default-off configuration)

The standing regression ran at HEAD with both this and the audio commit in:
`scrubbar.ps1` full pool **PASS (22 files, 88 legs, `delta 0` throughout)** ·
4K H.264 cadence ×2 **99.2/99.1%** identical buckets · 4444 ×2 **99.8/99.8%**
· 4444 `-SnapRelease` **`delta 0`**, `hitch 0`, `land 0` · **25 of 25
transitions** · lifecycle **93.9%/0%** · `emptystate.ps1` all modes both
backends + `-Bar` · `uiatree.ps1` identical rects across backends. With the
knob unset the animated path never executes (one static `markAnimEnabled()`
read per rebuild), which is what those figures measure.

## What this is not, and the decision it tees up

The mark ships as committed PNGs and neither animated property is reachable
from a bitmap — so the animated path is a **second source for the mark**,
which "artwork follows behaviour" tolerates only as a prototype behind a
default-off knob. If the owner ships the animation, the PNG renditions and
the `paintIcon` route leave in the same decision (and
`verify_trace_assets.py` will demand it, since the derived set shrinks); if
the owner declines, `paintAnimatedEmptyMark` and the knobs are one
self-contained revert. Both routes coexist until then, and the ink-centring
scan, the hint layout and the audio-file hint suppression run identically on
either.

Rendering note for the judgement: the procedural mark is a faithful
re-authoring, not a byte-identical re-rasterisation — resvg and QPainter are
different rasterisers, so at the same phase the two sources are visually
matched rather than pixel-equal. The comparison that must hold (and does) is
cross-backend identity of whichever source is drawing.
