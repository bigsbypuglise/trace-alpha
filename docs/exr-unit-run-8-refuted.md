# Raising the unit-run gate to 8: refuted, and it corrected a wrong attribution

Record of what was measured. 2026-08-25, branch `exr-stage0-dependencies`,
physical panel **5120x1440 @ 239.999000 Hz** (re-checked before measuring).
Warm, 3 reps each, `TRACE_SEQ_PROFILE=1` on in every column.

**The gate is back at 4. `TRACE_SEQ_PREFETCH_STRIDE=1` is still DEFAULT OFF and
the fixed +-1 window still ships.**

---

## VERDICT

**Raising the run gate from 4 to 8 FAILS, and fails on the file it was supposed
to leave alone.**

| target | result at run 8 |
|---|---|
| PIZ remains in the fixed-prefetch class | **FAILS DECISIVELY -- 99.9% -> 98.9 / 96.6 / 93.4%** |
| DWAA matches prefetch-off | **NO CHANGE AT ALL from run 4** |
| tick-stall remains 0 | **1 / 1 / 0** (and run 4 measures 0-1, not a robust 0) |
| exactness unchanged | MET -- 0% cross-config, both files |

---

## THE CORRECTION, AND IT INVALIDATES A CLAIM IN THE PREVIOUS RECORD

`docs/exr-unit-run-prefetch.md` stated that the run-4 gate leaked four
predictions per playthrough on the DWAA file, and attributed them to the file
genuinely producing runs of four unit steps -- with the arithmetic
`0.45^4 x 61 ~ 2.5`.

**That was wrong. The gate has never fired on the DWAA file, at either
setting.** The four were the WARM-UP frames falling through to the legacy `+-1`
window. They were counted as "not declined" because the stride branch is not
entered during warm-up at all, so it records no decline, and
frames-minus-declines was read as the gate firing.

**A `prefetch ISSUED` counter settles it by measurement rather than inference:**

| file | ISSUED | DECLINED | LEGACY +-1 | cache HIT / MISS |
|---|---|---|---|---|
| DWAA, gate 4 | **0** (absent) | 57 | 5 | 2 / 60 |
| DWAA, gate 8 | **0** (absent) | 57 | 5 | 2 / 60 |
| PIZ, gate 4 | **212** | **0** | 5 | **216 / 1** |
| PIZ, gate 8 | 82 | **116** | 5 | 72 / 131 |
| PNG, gate 4 | 163 | 0 | 5 | **167 / 1** |

The identical `DECLINED 57` at both DWAA settings was already sufficient to
infer this from the previous session's data -- every branch entry declined in
both -- but it took the counter to make it a measurement. **The lesson is the
one this project keeps relearning: a count derived by subtracting two other
counts is not a measurement of the thing you think it is.**

**Consequence for the previous report:** the residual DWAA gap against
prefetch-off is **not** leaked predictions and never was. It is the five-frame
warm-up window doing two loads per frame. Run length cannot touch it, which is
exactly why 8 changed nothing.

---

## WHY 8 BROKE PIZ -- a coupling the predicate does not show

| PIZ | fixed +-1 | gate 4 | **gate 8** |
|---|---|---|---|
| presented | 99.9 / 99.9 / 99.9% | 99.9 / 99.9 / 99.9% | **98.9 / 96.6 / 93.4%** |
| frames | 216 | 216 | **214 / 209 / 202** |
| skip | 0 | 0 | **2 / 7 / 14** |
| `handler>budget` | 0 of 215 | 0 of 215 | **2 / 7 / 13** |
| `<0.9x` bucket | 0 / 1 / 0 | 0 / 1 / 0 | **3 / 10 / 18** |
| cache HIT / MISS | 216 / 1 | **216 / 1** | **72 / 131** |
| ISSUED / DECLINED | -- | 212 / 0 | 82 / **116** |

**The warm-up window is what PRIMES the cache.** With warm-up 4 and gate 4 there
is no hole: the legacy window keeps the next frame resident, the gate opens into
a steady state where the presented frame is already a hit, and the tick pays one
load. Widening the gate to 8 leaves four frames in which nothing is prefetched,
the cache drains, and when the gate finally opens the tick pays a **MISS plus a
prefetch -- two loads**. On a file whose handler already sits at 38-40ms against
a 41.67ms budget that is enough to miss the deadline, skip, **reset the run**,
and fall back to declining.

**It never converges, and the degradation is monotone across reps: 98.9 -> 96.6
-> 93.4%.** That progression is the signature -- this is a feedback loop, not
noise.

---

## DWAA AT BOTH SETTINGS -- unchanged, because the gate never fires

| DWAA | fixed +-1 | prefetch off | gate 4 | gate 8 |
|---|---|---|---|---|
| presented | 29.2 / 28.9 / 28.7% | **64.1 / 64.1 / 64.1%** | 63.1 / 62.2 / 62.5% | 62.4 / 63.7 / 63.7% |
| handler p50 | 146-150 ms | 65.7-67.3 ms | 66.1-67.5 ms | 66.7-66.9 ms |
| handler max | 184-190 ms | **74.1-75.2 ms** | 99.4-121.8 ms | 98.0-104.1 ms |
| `tick-stall` | 24 / 24 / 25 | **0 / 0 / 0** | 1 / 1 / 1 | 1 / 1 / 0 |
| loads per frame | 2.82 | 1.02 | 1.07 | 1.05 |

Gate 4 and gate 8 are the same policy in practice on this file. **The p50 is
already prefetch-off's at both**; what remains is the tail, and the tail is the
warm-up window's two-load frames.

**`tick-stall` is 0-1 rather than a robust 0.** The previous record's 0/0/0 was
a favourable sample; across the runs taken today it reads 1/1/1 at gate 4 and
1/1/0 at gate 8. Stated plainly because the acceptance criterion asked for 0.

---

## A THIRD SEQUENCE, ADDED BECAUSE TWO FILES IS NOT A POPULATION

`6_Image_Sequence\PNG_SEQ\R2_OP_Stacks_02D`, 168 frames, 1920x1080, PNG -- a
different loader on the same playback path.

| PNG | fixed +-1 | gate 4 |
|---|---|---|
| presented | 100.0% | **100.0 / 100.0%** |
| skip | 0 | 0 |
| `handler>budget` | 0 of 166 | **0 of 166** |
| `tick-stall` | 0 | **0** |
| cache HIT / MISS | 167 / 1 | **167 / 1** |
| ISSUED / DECLINED | -- | **163 / 0** |

Identical. The gate opens and stays open, exactly as on PIZ.

*(The loader stage rows read 0 for PNG: `SeqProfile` instruments `loadExr`
specifically, so `ACCOUNTED` is meaningless on a non-EXR sequence. The cache and
prefetch counters live in `MainWindow` and are valid. Worth knowing before
someone reads a PNG profile table as a decomposition.)*

---

## EXACTNESS

`seqstep.ps1`, fixed vs gate 8, 7 steps out and back: **0% cross-config** at
frame 7 and back at 0 on both EXR files, negative controls **10.60%** and
**54.82%**. Unaffected by the gate length, as expected -- the policy is gated on
`playTimer_.isActive()` and stepping takes the legacy window.

---

## RECOMMENDATION

**Yes -- I would make the unit-run policy (gate 4) the shipping default, with
three things said out loud rather than buried.**

The case for it:

- **DWAA 28.7-29.2% -> 62.2-63.1%**, a 2.1x improvement on the file that fails.
- **PIZ identical to today's shipping behaviour** -- 99.9% x3, cache 216/1.
- **PNG identical** -- 100.0%, cache 167/1. Three sequences, no regression.
- **Exactness is structural**, not a test result that could rot: the gate is on
  `playTimer_.isActive()`, so stepping and random access take the legacy window
  verbatim.
- Cache architecture, radius and put/get are untouched.

The three caveats:

1. **DWAA does not fully reach prefetch-off** -- ~1-2 points, handler max
   99-122ms against 74-75, `tick-stall` 0-1 against 0. **All of it is the
   five-frame warm-up window**, now correctly attributed. Closing it is a
   separate one-line experiment (decline during warm-up when stride-aware is
   on, instead of using the legacy window) and is **unmeasured** -- it would
   cost PIZ a handful of early cache misses and that trade has not been taken.
2. **Reverse playback is handled by construction and was never measured.** The
   counter is signed and predicts backwards at -1, but no reverse run was
   timed. If this ships, that is the first gap to close.
3. **No owner hand-test.** Every figure here is a counter; nobody has watched
   the DWAA sequence play under this policy.

**If the answer is "ship it", the honest sequencing is: close caveat 2 first
(it is a measurement, not a change), then flip the default, then hand-test.**

---

## SCOPE

Commit `4cfeb25`. Gate back at 4; `kSeqStrideWarmupSamples` and
`kSeqUnitRunRequired` are now separate constants. **No scheduler-accumulator
prediction. No off-thread EXR reads. Alpha prefill untouched. No GPU or OCIO
work. Cache unchanged.** Every figure is with no colour transform active.
