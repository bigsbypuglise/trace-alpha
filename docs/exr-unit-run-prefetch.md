# Unit-run gated prefetch: three of four acceptance criteria met

Record of what was measured. 2026-08-25, branch `exr-stage0-dependencies`,
physical panel **5120x1440 @ 239.999000 Hz** (re-checked before measuring; same
mode as the 2026-08-24 runs, `parsecd` daemon up with no virtual display
active). All configurations run on **one binary**, warm, 3 reps each,
`TRACE_SEQ_PROFILE=1` on in every column.

**`TRACE_SEQ_PREFETCH_STRIDE=1`, DEFAULT OFF. The fixed +-1 window still ships.
Not a policy.** Supersedes the policy half of
`docs/exr-stride-aware-prefetch.md`; that document's measurements stand as the
record of the first formulation.

---

## VERDICT AGAINST THE STATED ACCEPTANCE TARGETS

| target | result |
|---|---|
| PIZ remains in the fixed-prefetch class | **MET -- identical on every measure** |
| DWAA collapses to the no-prefetch class | **NEARLY -- 62.3-62.9% against 63.3-63.9%, consistently ~1.1 points short** |
| no extra tick-stalls from mispredicted prefetch | **MET -- 0/0/0 on both files, every rep** |
| exactness unchanged | **MET -- 0% cross-config, both files** |

**Three of four hold outright. The DWAA shortfall is small, consistent, and
fully attributed** -- see the residue section.

---

## THE PREDICATE, AND WHY IT IS A RUN COUNTER

The requested change was "only issue prefetch when the observed stride is
confidently 1.0". **The obvious implementation of that sentence does not
deliver it, and was measured before being replaced.**

| formulation | predictions issued (DWAA) | tick-stall | handler max |
|---|---|---|---|
| EMA within +-0.15 of **any integer** | 4 of 62 | 11 / 2 / 0 | 116-131 ms |
| EMA within +-0.15 of **1.0** | 4 of 61 | 1 / 1 / 0 | 99-116 ms |
| **run of 4 consecutive unit steps** | **4 of 61** | **0 / 0 / 0** | **96.6-97.6 ms** |

Narrowing the tolerance to 1.0 specifically **did not reduce the leak at all**.
A file whose strides alternate 1,2,1,2 has a mean near 1.5 that still wanders
inside any tolerance of 1.0 after a couple of unit steps. **"The average is near
1" and "it is stepping one frame at a time" are different claims**, and only the
second is worth acting on. A run counter cannot leak that way -- one skip resets
it to zero -- and it costs an int compare where the average cost a float one.

The leak count did not change; **what changed is when the leaks happen.** The
run counter only predicts during a genuine local unit-stride run, where the
prediction is likely to be right, so the stalls disappear.

---

## DWAA -- `MultlayerAces`, 97 frames, 27ch, 3 reps

| | **A** fixed +-1 (ships) | **B** prefetch off | **E** unit-run gate |
|---|---|---|---|
| **presented** | 29.2 / 28.9 / 28.7% | **63.9 / 63.7 / 63.3%** | **62.3 / 62.9 / 62.4%** |
| frames | 29 / 29 / 28 | 63 / 62 / 62 | 61 / 62 / 61 |
| **skip** | 70 / 69 / 68 | 34 / 34 / 34 | 35 / 35 / 35 |
| handler **p50** | 146.4 / 150.3 / 146.7 | 68.0 / 66.3 / 67.1 | 67.2 / 67.9 / 67.5 |
| handler **max** | 188.9 / 184.4 / 189.7 | **71.5-73.6** | 97.6 / 96.8 / 96.6 |
| **`tick-stall`** | 24 / 24 / 25 | **0 / 0 / 0** | **0 / 0 / 0** |
| **loads per presented frame** | **2.82** | **1.02** | **1.07** |
| **cache HIT / MISS** | 2 / 27 | 0 / 63 | 2 / 60 |
| predictions declined | -- | -- | **57 of 61** |
| accounted per frame | 145.24 ms | **58.90 ms** | 61.32 ms |

**`tick-stall` is the criterion that was in doubt and it is clean**: 0 on every
rep, against the fixed window's 24-25 and the first formulation's 11/2/0.

**The p50 is already B's** (67.2-67.9 against 66.3-68.0). What separates E from
B is the **tail** -- handler max ~97 ms against ~72 -- and that is the leaked
predictions, not a systemic cost.

---

## PIZ -- `R2_OP_Stacks_01`, 217 frames, 3ch

| | **A** fixed +-1 | **E** unit-run gate | (**B** off, 2026-08-24) |
|---|---|---|---|
| **presented** | **99.9 / 99.9 / 99.9%** | **99.9 / 99.9 / 99.9%** | 99.7 x3 |
| skip | 0 | 0 | 0 |
| handler p50 | 41.7 / 41.7 / 41.6 | 41.6 / 41.6 / 41.5 | 41.5-41.7 |
| handler max | 38.2 / 37.8 / 40.2 | **36.8 / 36.8 / 37.5** | 37.1-39.2 |
| `handler>budget` | **0 of 215** | **0 of 215** | 0 of 215 |
| `<0.9x` bucket | **0 / 1 / 0** | **0 / 1 / 0** | 6 / 9 / 5 |
| drift | -12 / -12 / -12 ms | -12 / -11 / -11 ms | -26 ms x3 |
| `tick-stall` | 0 | 0 | 0 |
| **loads per frame** | **1.005** | **1.005** | 1.005 |
| **cache HIT / MISS** | **216 / 1** | **216 / 1** | 0 / 217 |
| predictions declined | -- | **0** | -- |
| accounted per frame | 26.72 ms | 26.63 ms | 27.02 ms |

**E is A on this file, to the digit that matters.** The stride is exactly 1 for
the whole run, so the gate opens after four frames and never closes: **zero
declines, 216 hits of 217.** The `<0.9x` 6/9/5 and -26 ms drift that blanket
removal cost are absent.

---

## EXACTNESS AND PAUSED REVIEW

`scripts/measure/seqstep.ps1`, 7 steps out and back, fixed vs unit-run gate:

| file | negative control | frame 7 across configs | back to 0 |
|---|---|---|---|
| DWAA | **10.60% differing** | **0%** | **0%** |
| PIZ | **54.82% differing** | **0%** | **0%** |

Structural rather than lucky: the gate is on `playTimer_.isActive()`, so paused
stepping and random access take the legacy `+-1` window verbatim.
`prefetchNeighbors()` is reached from seven places and only two are the playback
tick.

---

## THE RESIDUE, ATTRIBUTED

**Four predictions of 61 are issued on DWAA, and that is not a bug in the gate
-- the file really does produce runs of four consecutive unit steps.** Its
stride mix is roughly 45% unit (62 presents, 34 skips, 97 frames), so a run of
four appears about `0.45^4 x 61 ~ 2.5` times per playthrough, and each run that
continues issues another. **Two of the four were subsequently used** (`cache HIT
2`), so half of them paid off; the other half cost a whole ~43 ms load.

That is the entire 61.32 vs 58.90 ms per frame gap, and `prefetch (all)` reads
**2.83 ms per frame** against a 2.42 ms difference -- the arithmetic closes.

**The lever, NOT turned:** requiring a longer run. At `p(unit) ~ 0.45`, a run of
**8** takes the expected leak to ~0.1 per playthrough, while on PIZ it only
delays the first prediction by four more frames out of 216. That is the same
predicate with one constant changed, and it is the obvious way to close the last
~1.1 points if the DWAA class is required exactly.

---

## WHAT IS STILL TRUE

**No configuration makes the DWAA file hold real time.** `handler>budget` is
every frame in all three columns; at B's 58.90 ms per frame `read_image` alone
is **41.34 ms**, which is 99% of the 41.67 ms budget by itself. Unchanged from
the decomposition's ranking.

---

## SCOPE

Commit `2274f3f`. **Cache architecture unchanged** -- same `FrameCache{1}`, same
radius, same put/get. **No scheduler-accumulator prediction. No off-thread EXR
reads. Alpha prefill untouched. No GPU or OCIO work.** Every figure is with no
colour transform active.
