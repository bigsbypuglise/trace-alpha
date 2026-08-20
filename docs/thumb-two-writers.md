# The beta.4 thumb excursions: two writers under one read (2026-08-20)

Owner report from v0.3.0-beta.4 on the Threadripper: scrub performance is good;
the slider thumb's animation is bad — visible artifacting/jumpiness during
drags. Frame analysis of the screen recordings (`260820_temprecordings\` at the
repo root): single-frame thumb excursions of 100–350px that immediately revert,
clustered at direction changes, with the excursion side matching the decoder's
lag direction.

## The mechanism, found in code and confirmed by measurement

Phase 6's contract is that the strip's track is a picture of `timelineSlider_`
and nothing else. The overlay's thumb and played track are drawn from
`OverlayHooks::positionFraction` — which read
`playback_.state().currentFrame`, not the slider. During an async drag that
field has **two writers**:

- `queueVideoScrubFrame` (`MainWindow.cpp`, the `valueChanged` path) writes the
  **pointer's frame** on every slider move;
- the scrub delivery loop in `onScrubResult` writes the **delivered frame's
  index** on every chain frame — which legitimately trails the pointer, and on
  the part-2 sampled/keyframe-landing path differs from the requested frame by
  up to a GOP *by design* (the seek's keyframe landing is delivered instead of
  walking to the mid-GOP target).

So any paint landing between a delivery write and the next pointer write drew
the thumb at the **decoder's** position, and the following pointer-move write
snapped it back: a single-frame spike toward the decoder's lag side, then an
immediate reversion. The gated scrub paint (`paintScrubFrameNow`) fires
directly after a delivery — the worst possible sampling moment — while
hover/fade-tick `update()` paints sample whichever write came last. That is
the whole recorded signature: excursions cluster at direction changes because
that is where the lag and the keyframe landings peak, and the excursion side
is the lag direction because the delivered index is the side of the hand the
decoder is on.

The slider itself was never contaminated — `syncTransportBar`'s write-back is
guarded by `isSliderDown()` (the `f77d472` fix), and `seekToFraction` is only
called from the hand. Only the two **read** hooks were wrong:
`positionFraction` (thumb + played track) and `positionText` (the strip's
position readout, which flickered the same way, less visibly).

## The fix

While `timelineSlider_->isSliderDown()`, both hooks read the **slider's
value** — the position the user is setting. Everywhere else (playback,
stepping, shuttle, landings) they keep reading `playback_.state()`, unchanged.
The delivered-frame write into `currentFrame` stays: it is the playhead and
the HUD/step/release logic depend on it. The picture may trail the hand — that
is what the picture shows — but the thumb belongs to the hand, and now has
exactly one source for the whole gesture. On release, slider value ==
landing target == `currentFrame`, so the handover is seamless.

## Reproduction and verification: `scripts/measure/thumbtrack.ps1`

New harness. It presses on the overlay strip's track, sweeps hard reversals
with one strip-band capture per pointer step (stride-aware LockBits scan), and
tracks the rightmost accent pixel — the played track / thumb ring is the only
accent on the strip while Loop is off. Classifier: an excursion is a step of
≥60px immediately followed by a reversion of at least half its size; the
pointer moves monotonically inside a leg, so a genuine thumb cannot do that —
every excursion is a second position source leaking into a paint.

Fault model (the recorded Threadripper class): 60Hz + `TRACE_PRESENT_SYNC=1`,
WeLo (`17_Random_Mp4s\WeLo-AI-rc13-1x1_SOCIAL.mp4`, dec/ptr 0.53 — the worst
demand-over-supply file).

| build | config | excursions | max spike | thumb-vs-pointer max lag |
|---|---|---|---|---|
| pre-fix (`793b0d6`) | 60Hz + sync, WeLo | **4** | **73px** | 266px |
| fixed | 60Hz + sync, WeLo | **0** | 0 | 67px |
| fixed | 60Hz + sync, Universe | **0** | 0 | 66px |
| fixed | 240Hz default, WeLo | **0** | 0 | **19px** |

The pre-fix run is the harness's own fail-proof: all four excursions spike
*upward* during *backward* legs — the delivered index above the hand, i.e. the
decoder's lag side — which is the recordings' signature reproduced, not merely
a nonzero count. Example row: pointer at 502 moving left, accent right edge
544 → **607** → 531. Max spike is 73px on WeLo's ~500px track in a 960px
window; the Threadripper recordings' 100–350px is the same fault at that
machine's larger track and lag.

The residual thumb-vs-pointer lag on the fixed build (~19px at 240Hz, ~67px
under the harsh sync model) is the scrub coalescing window plus capture
latency between `SetCursorPos` and the next painted frame — the thumb tracking
the hand at input cadence, not a position error.

## Regression

- Standing scrub bar: `scrubbar.ps1` over the full pool on the fixed binary —
  PASS, exit 0, `delta 0` on every leg (the exactness contract; the fix
  touches two read hooks and no decode/landing path).
- The fix is inert outside a held slider by construction: `isSliderDown()` is
  false during playback, stepping, J/K/L shuttle and async landings, so every
  recorded figure from those paths is taken on the unchanged branch.
- Bar mode never had the bug — the docked QSlider draws its own thumb from its
  own value — and is untouched.
