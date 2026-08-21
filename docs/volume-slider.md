# The inline volume slider — record (2026-08-20)

Owner decision on tester feedback, **reversing UI redesign roadmap step 5's decision 2**
("a MUTE BUTTON, and NO volume slider") — recorded as the owner deciding again with tester
evidence, not as drift. The design is the designer's **option 1b**, delivered as
`assets/source/Trace_AudioSlider.zip` (committed untouched at `3d96c61`); the feature is
`af4e427`. **`TRACE_VOLUME_SLIDER=0` is the owner-required rollback** and restores the
step 5 mute-only button exactly — the same in-binary-revert pattern as
`TRACE_SCRUB_GOP_SAMPLE` and `TRACE_SCRUB_PAINT_GATE`.

## What ships

- Hovering (or clicking) the speaker slides a **74 logical px** slider out in the transport
  row between Mute and Loop; it collapses after **1.5s** idle (its own single-shot timer
  beside the auto-hide — the slider must collapse while the strip stays up). The scroll
  wheel over the speaker (or the slider) adjusts by 5% a notch and expands it.
- The three glyphs are the package's — `volume` (rest), `volume-open` (expanded),
  `volume-muted` — **superseding the step 5 volume/volume-muted pair in place** (same
  names, byte-different art). The slider itself is code-drawn per the package: track 4px in
  the timeline's white-0.22, fill in the accent, thumb an 11px plain white dot (its own
  atlas cell — not the timeline's 13px ringed thumb).
- **Volume is a sink gain and never the clock**: `AudioOutput::setVolume(fraction)` maps the
  UI's 0..1 perceptually (`QtAudio::convertVolume`, logarithmic→linear) onto
  `QAudioSink::setVolume`, composing with mute (`muted ? 0 : gain`). Audio remains the
  master clock at 1×; nothing in the tick changed. `M` keeps working.
- **The slider is NOT a picture of `timelineSlider_`.** Its own press routing
  (`draggingVolume_`, `setVolumeFraction`/`setVolumeDragging` hooks); a volume drag can
  never issue a seek and never touches `setScrubbing`.
- Expanding **re-lays the strip** (Loop and the timeline shift right, the middle gives the
  74px), bumps `layoutRevision_`, and the accessibility proxies re-sync from the same rects
  — the phase 14 mechanism, not a second layout. The ninth proxy (role Slider, no action)
  is gated on the same `volumeSliderEnabled()` and parks at an **empty rect while
  collapsed**, the honest geometry for a control not on screen. `controlRects()` returns
  nine entries whenever the feature is on, so the proxy count is stable.
- Wheel routing is new in both backends: `WM_MOUSEWHEEL` on the d3d11 surface (whose lParam
  is **screen** coordinates, unlike every other mouse message — converted once), and
  `wheelEvent` on the cpu path. Only volume consumes a wheel; everything else keeps its old
  propagation.
- HUD: the audio line carries `vol N%` while the feature is on (gated on the same knob, so
  the rollback restores the old line byte for byte).

## The semantics, and which were RECORDED DEFAULTS pending the owner
**(ANSWERED 2026-08-21 — see "The owner's answers" at the bottom; the "session-only" bullet
below is superseded there: volume persists now.)**

- **Dragging to zero reads as muted** (owner item wording): glyph flips to `volume-muted`,
  audio silent at gain 0, mute flag untouched.
- **Clicking the speaker at level zero restores the pre-drag level.** There is no prior
  audible state in that case (unlike an explicit mute), so the host remembers: a drag that
  ends at zero restores where the drag STARTED — the drag edges
  (`setVolumeDragging`) exist precisely because the drag's own ramp values (60, 40, 20, 5
  on the way down) must not be what "restore" means. Wheel-to-zero restores the last
  wheel-set non-zero value (a stepwise descent is read as deliberate). A fresh session
  restores to 100%. **This is the answer to the owner question "what does unmute restore
  when the level was dragged to zero" — a recorded default, not a settled decision.**
- **Raising the level while muted unmutes** (the counterpart of drag-to-zero-reads-muted),
  and `M`'s unmute at level zero also restores, so the key and the click match.
- **Volume is session-only** — not persisted. Consistent with mute (not persisted) and with
  the owner's Loop-persistence reversal (item 6). **The persistence question is the second
  recorded default pending an owner call.**
- The expand/collapse is instant rather than animated. The package says "slides out"; a
  width animation would re-lay the strip per tick and re-sync the proxies with it, so v1
  ships the instant version and the animation is owner-feel polish if wanted.

## Measured (2026-08-20, physical panel 5120×1440 @ 239.999Hz, build `af4e427`)

`scripts/measure/volumeslider.ps1` (new; geometry from the client rect — GetWindowRect's
invisible-border trap — and interactions run HUD-hidden, because the dev HUD sits below the
viewer and the strip is NOT at the client bottom with it shown; the vol token is read from a
final capture after `H`):

- **Hover expands**: slider-region pixels 18 → 325 (d3d11), 14 → 325 (cpu) — the same
  expanded count on both backends.
- **Wheel**: five notches down over the speaker, HUD reads `vol 75%`.
- **Drag to zero**: speaker cell changes 167 px (d3d11) / 186 px (cpu) of 625 —
  the muted glyph, hovered-vs-hovered (the overlay.ps1 lesson).
- **Unmute click restores `vol 75%`** — the pre-drag level, not the ramp's last value.
- **`-Mode off` (the rollback)**: slider-region pixels 18/18 — the strip is unchanged by
  hovering the speaker, delta 0.

Harness updates: `overlay.ps1`'s loop leg parks on Play for 2.2s after the mute taps — the
expanded slider shifts Loop right by ~76px, and the collapsed-layout `$loopX` would land on
the slider (the owner-item-15 stale-offset class). `uiatree.ps1`'s reading-order note is
nine controls now, with the collapsed Volume rect documented as expectedly empty.

## The owner's answers (2026-08-21) — nothing here is a default any more

1. **Unmute after a drag to zero restores the previous volume** — the shipped pre-drag-level
   behaviour, confirmed.
2. **Volume PERSISTS between sessions** — built at `b3a7d41`: `audio/volume` in the one
   settings home, seeded at startup, written at settled values only (wheel steps, drag ends),
   both gated on `TRACE_VOLUME_SLIDER` so the rollback keeps mute-only behaviour exactly. The
   fraction is quantised to 0.1% steps in its one writer (the wheel's 0.05 subtractions
   accumulate binary crumbs; 0.7499999999999998 reached the settings file before this).
   `volumeslider.ps1` runs on a scratch `TRACE_SETTINGS_FILE` now — a persisted volume is an
   input to a measurement — and its `-Mode persist` is the check (INI holds 0.75, the
   relaunch HUD reads `vol 75%`).
3. **Expansion stays instant** — the owner will test the feel at the machine and report on
   hover timing, collapse timing and whether instant expansion reads right. That feel test is
   the one open item on this feature.
