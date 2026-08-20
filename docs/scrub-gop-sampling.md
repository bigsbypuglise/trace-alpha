# Long-GOP backward sampling with keyframe landings — part 2 of the reliability phase

2026-08-20, dev box, same session as the part-1 sweep (`docs/scrub-population-sweep.md` — read
it first; this document assumes its table). The class being fixed, from that table: **long-GOP
H.264 under a reversal or rapid gesture whose pointer demand exceeds decode supply** — three
files at dec/ptr 0.53 / 0.77 / 0.82 against ≥0.96 for every clean file, severity monotone in
the ratio, display-independent, with the worst ending a reversal drag 216 frames / 1.7s behind
the pointer while every fixed subsystem measured healthy.

## The mechanism

Three pieces, all riding the machinery that already exists. `TRACE_SCRUB_GOP_SAMPLE=0` is the
in-binary control and restores the pre-change behaviour exactly.

**1. The §15 stride controller now returns strides for long-GOP — backward only.**
`computeScrubStride` takes the direction; the demand/supply formula, the clamps and the intra
path are untouched (the phase's safety property: adaptive values reproduce on this box — they
must, because the expression is the same expression). Forward long-GOP stays at stride 1: a
forward strided Scrub request seeks, and walking back up from the landed keyframe costs more
than the stride saves. This is the reverse shuttle's own boundary — "only backward pays the
intercept" — applied to the drag.

**2. Sampled backward steps carry a keyframe-landing flag into the decoder.** `ScrubRequest`
gains `keyframeLand` + `keyframeFloor`; `decodeFrameAt` honours them **in Scrub mode only** —
Step (the exact landing) and Playback (the shuttle, the tick) cannot inherit it by
construction. When a flagged request seeks, the first decoded frame after the seek **is** the
landed keyframe, PTS-resolved to its true index; if it lies short of the target and not before
the floor, it is converted and delivered as the preview, walk zero. §15's measured catastrophe
(hits 85.4% → 13.3%) was the walk *after* the seek — a mid-GOP strided target pays for a
region the next sample immediately leaves — and the landing removes that term by construction.

This is the reverse shuttle's snap trade **without the learned grid**: the seek landing is the
ground truth for where a keyframe is, per request, no inference, no statistic — so WeLo's
irregular GOP structure (gaps 3..23) is handled exactly, where a spacing model would drift.
The four failed gate inferences recorded in `computeScrubStride`'s comment stay recorded and
stay refuted; nothing here measures a cost and feeds it back.

**The floor is the pointer.** A landing below it would put the picture past the hand and the
chain would ping-pong across it. When the keyframe the seek reaches is below the floor —
which is the "converged to within a GOP of the pointer" case — the request falls through to
the exact walk, whose cache fills cover the final approach. Coarse hops far out, exact frames
near the pointer, and the release untouched either way.

**3. The step is eased, and the first smoke run is why.** The first cut posted
`active − stride` with the flag: stride read ~3 (the §15 estimate is diluted by the gesture's
forward walk deliveries — the gate comment's "mean per request" failure in a new costume), so
each hop advanced ~one GOP from the *picture* while the deficit was ~400 frames. WeLo ended
157 behind — barely better than the control's 216. One hop costs one seek whether it spans
one GOP or ten, so the flagged step now covers
`max(stride, ceil(gap × kScrubEase))` — the same ease constant, and the same reasoning, as
the synchronous walk and the batch: exponential convergence, hops that shrink as the picture
arrives. After: WeLo ends at the pointer.

**Honesty telemetry.** The delivered keyframe carries its own true index — nothing is
mislabelled, which is the July-2026 scrub fault's exact rule. `previewApproximate` /
`previewTargetFrame` / `previewDisplayedFrame` are set on every landing; the HUD's `sample`
line reads `ON-bwd`/`idle-bwd`/`GATED` for long-GOP and gained `kf-land N` (landings that
actually happened, counted at the delivery boundary); the selftest prints `kf-land` per leg
and `gop-sample` on its knobs line. **`kf-land` must read 0 on intra media and on every
unflagged gesture** — that is the check the mechanism has not crept anywhere it does not
belong, and it held across the entire sweep.

## Measurement — full pool, fix against same-binary control

`scrubsweep.ps1` (22 files × 4 legs), three passes on the fixed binary: **240hz-fix**,
**240hz-ctrl** (`TRACE_SCRUB_GOP_SAMPLE=0` — the pre-change behaviour, same binary, same
session, per the phase's "build the control" rule), **60hz-fix** (via `setrefresh.ps1`,
restored and verified). Panel 5120x1440, plain config, cursor parked. **All 264 legs of all
three passes PASS with `delta 0`** — the exactness contract is untouched.

### The three failing files, leg 2 (reversal drag), fix / control at 240Hz

| file | behind end | p2p end ms | p2p max ms | hitch | kf-land |
|---|---|---|---|---|---|
| **WeLo 1x1 720f** | **0 / 216** | **85 / 1668** | 2537 / 2702 | 13 / 18 | 15 |
| **Universe 9x16 412f** | 0 / 0 | **57 / 241** | 1635 / 2736 | 9 / 10 | 2 |
| **Jeep 4K60** | 0 / 0 | **152 / 210** | 704 / 950 | 4 / 4 | 1 |

At 60Hz the fix reads the same class: WeLo `p2p end 87.9` (part-1 60Hz control: 700), Universe
`44.6` (269), Jeep `149.4` (209). Leg 4 (the owner's rapid gesture): all three end at the
pointer with `p2p end ≤ 8.5ms` on both fix passes.

**What remains, by design:** `behind max` mid-gesture is unchanged (~175 Universe, ~415 WeLo)
— that is the *forward* halves of the reversal gesture trailing at decode rate, and forward
deliberately does not sample. Every backward stretch and every gesture end now converges in a
few hops. If forward trailing is ever the felt complaint, forward keyframe hopping is the
recorded follow-up — it needs container-index queries to avoid unproductive seeks, and it was
deliberately not built until the table says it is needed. **Jeep is the boundary file** (ratio
0.82): the mechanism engages barely (`kf-land 1`) and the gain is modest (p2p end 210 → 152);
it was the mildest member of the class in part 1 and remains it.

### The other nineteen files

Zero movement beyond noise, checked per leg per file against the control: legs 1 and 3 show
**no file moved past noise thresholds and `kf-land 0` everywhere**; legs 2 and 4 are flat on
every healthy H.264 file (stride 1, kf-land 0) and on every ProRes file (§15's intra path,
byte-identical by construction — `kf-land 0` on all 24 intra rows of every pass). The
Seedance HEVC reference row is unchanged.

## Standing regression, same session, fixed binary

- **Cadence** (playback — shares `decodeFrameAt` but can never set the flag): 4K H.264
  **99.1/99.2%**, `drop 0`, `rephase 0`, 118 of 119 gaps ~1x, `handler>budget 0 of 120` —
  the recorded class. (One first-launch rep read 88.9% with `drop 173`; it did not reproduce
  on a fresh launch — the recorded cold-start class, aggravated here by running seconds after
  the 60→240Hz mode restore. Three of four reps clean.) 4444 **99.8/99.8%**, 261 frames,
  `drop 0`, `0 of 260`, all gaps ~1x — the recorded class to the digit.
- **4444 `-SnapRelease`** (real mouse, bar mode, widened 1280): `target 261 shown 261
  delta 0` full-res planar, `release 23.0ms`, `hitch 0`, `land 0`, `kf-land 0`.
- **4K H.264 `-Reversals`** (real mouse, mechanism live): `delta 0`, **`hitch 1`** (the
  recorded figure), `rev-hit 95.9%`, `seeks 4`, `release 43.6ms` — and `kf-land 0` with
  `skipped 84 over 60 steps`: this file's strided backward targets were all reverse-cache
  hits, so the landing correctly never fired. The mechanism degrades to the cache, not past
  it.
- **Transitions: 25 of 25 PASS.** Lifecycle **97.7% moving / 0% control**.

## Constraints, checked

- **Class, never a file**: the gate is codec property (`intraOnly`) + live demand/supply +
  the seek's own landing. No filename, resolution or profile branch anywhere.
- **`delta 0` non-negotiable**: held on all 264 sweep legs and both real-mouse releases. The
  flag is unreachable from Step by construction.
- **Sanctioned exception scope**: the landing exists only in the async drag chain's
  Scrub-mode steps — the active drag preview, nowhere else.
- **Safety property**: the stride formula is unchanged, so the tuned values reproduce
  trivially; intra behaviour identical (measured: 4444 SnapRelease and reversal figures in
  recorded class).
- **Bounded memory**: no new allocation; landed keyframes enter the existing byte-budgeted
  cache, tagged preview-res as any Scrub frame.

## Open

- **Owner feel test at the machine** (feel judgements are not valid over Parsec): the thing
  to judge is a fast backward drag on `WeLo-AI-rc13-1x1_SOCIAL.mp4` and
  `Universe_rc07_I_9x16_Online.mp4` — the preview now shows GOP-spaced frames while the hand
  outruns the decoder, converging onto exact frames as it slows, where before it walked every
  frame seconds behind. `TRACE_SCRUB_GOP_SAMPLE=0` is the A/B.
- **Forward halves trail by design** (behind max mid-gesture unchanged); forward keyframe
  hopping is the recorded follow-up if the owner's hand finds it.
- ~~Part 3 (the standing regression with a per-file pass bar) is unstarted~~ **DONE
  2026-08-20, the following session — `scripts/measure/scrubbar.ps1` + the derived
  `scrub-pass-bar.csv`; record in `docs/scrub-pass-bar.md`.**
