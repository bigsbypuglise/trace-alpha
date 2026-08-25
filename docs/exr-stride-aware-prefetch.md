# Stride-aware EXR prefetch: PIZ preserved in full, DWAA most of the way

Record of what was measured. 2026-08-24, branch `exr-stage0-dependencies`,
physical panel **5120x1440 @ 239.999000 Hz**. All three configurations run on
**one binary**, warm, 3 reps each, `TRACE_SEQ_PROFILE=1` on in every column so
the columns are the same measurement.

**`TRACE_SEQ_PREFETCH_STRIDE=1`, DEFAULT OFF. The fixed +-1 window still ships.
No policy is permanent.**

---

## THE ANSWER TO THE QUESTION ASKED

**Does stride-aware prefetch preserve the PIZ benefit while achieving the DWAA
no-prefetch performance class?**

- **PIZ: YES, in full.** Identical to the fixed window on every measure --
  99.9% x3, cache **HIT 216 / MISS 1**, 1.005 loads per frame, `<0.9x` bucket 0.
- **DWAA: MOST OF THE WAY, NOT ALL OF IT.** Steady state reaches **62.9%**
  against no-prefetch's **63.7-64.1%**, but the run-to-run spread is
  **54.3-62.9%** where no-prefetch is flat, and **tail latency is clearly
  worse** -- handler max 116-131 ms against 80-84 ms, `tick-stall` 11/2/0
  against 0/0/0.

So the mechanism is right and the residue is real. It is **not** ready to be
made permanent as it stands, and the reason is identified below.

---

## DWAA -- `MultlayerAces`, 97 frames, 27ch, 3 reps

| | **A** fixed +-1 (ships) | **B** prefetch off | **C** stride-aware |
|---|---|---|---|
| **presented** | 29.6 / 28.4 / 29.0% | **63.9 / 64.1 / 63.7%** | 54.3 / 60.6 / **62.9%** |
| frames | 29 / 28 / 29 | 63 / 63 / 63 | 53 / 59 / 62 |
| **skip** | 67 / 69 / 70 | 33 / 34 / 34 | 43 / 37 / 34 |
| handler **p50** | 148.9 / 149.8 / 146.1 | 65.4 / 66.4 / 67.0 | 70.9 / 67.2 / 65.6 |
| handler **max** | 188.2 / 202.4 / 193.7 | **84.1 / 80.4 / 81.3** | 130.6 / 124.3 / 116.1 |
| **`tick-stall`** | 22 / 24 / 24 | **0 / 0 / 0** | 11 / 2 / 0 |
| `handler>budget` | all | all | all |
| **loads per presented frame** | **2.72** | **1.02** | **1.06** |
| **cache HIT / MISS** | 3 / 27 (10%) | 0 / 63 | 2 / 61 |
| predictions declined | -- | -- | **58 of 62** |
| accounted per frame | 141.67 ms | **58.80 ms** | 60.34 ms |

**The policy does what it was designed to do**: it recognises that a stride of
~1.4 has no integer answer and declines **58 of 62** times, landing at 1.06
loads per frame against no-prefetch's 1.02 and the fixed window's 2.72.

**What it does NOT do is match B's tail.** The four predictions it *did* issue
cost 170.1 ms between them -- **42.5 ms each, a full load** -- and every one of
those lands inside a tick that is already over budget. That is what the handler
max and the `tick-stall` column are showing.

**The rep-to-rep spread (54.3 -> 60.6 -> 62.9%) is NOT explained.** Each rep is
a fresh process, so the EMA warm-up is identical in all three, and the A and B
columns are flat across their own reps on the same file in the same session --
so it is not OS file caching either. Recorded as unexplained rather than
attributed; three reps is too few to characterise it and it is the first thing
to nail down if this policy is taken further.

---

## PIZ -- `R2_OP_Stacks_01`, 217 frames, 3ch, THE CONTROL

| | **A** fixed +-1 | **B** prefetch off | **C** stride-aware |
|---|---|---|---|
| **presented** | **99.9 / 99.9 / 99.9%** | 99.7 / 99.7 / 99.7% | **99.9 / 99.9 / 99.9%** |
| skip | 0 | 0 | 0 |
| handler p50 | 41.7 / 41.6 / 41.6 | 41.7 / 41.6 / 41.5 | 41.7 / 41.6 / 41.7 |
| handler max | 38.0 / 38.4 / 39.3 | 37.1 / 37.1 / 39.2 | 37.1 / 37.2 / 36.9 |
| `handler>budget` | **0 of 215** | **0 of 215** | **0 of 215** |
| `<0.9x` bucket | 0 / 0 / 1 | 6 / 9 / 5 | **0 / 0 / 0** |
| drift | -12 / -11 / -12 ms | -26 ms x3 | **-13 / -11 / -13 ms** |
| `tick-stall` | 0 | 0 | 0 |
| **loads per presented frame** | **1.005** | 1.005 | **1.005** |
| **cache HIT / MISS** | **216 / 1** | 0 / 217 | **216 / 1** |
| predictions declined | -- | -- | **0** |
| accounted per frame | 26.80 ms | 27.02 ms | 27.03 ms |

**C is A, on this file.** The stride reads exactly 1.0, so the prediction is
issued every time and is right every time: **zero declines, 216 hits of 217**.
The `<0.9x` regression that blanket removal caused (6/9/5) is **gone**, and the
drift is back at A's -11 to -13 ms rather than B's -26 ms.

It also drops the trailing `-1` request that A makes, which costs nothing
either way -- on this file that frame is already inside the radius-1 window, so
A's request is a cache hit rather than a load. That is why A and C both read
1.005 loads per frame.

---

## EXACTNESS AND PAUSED REVIEW -- UNCHANGED

`scripts/measure/seqstep.ps1`, 7 steps out and 7 back, fixed vs stride-aware:

| file | negative control (frame 7 vs 0, one build) | frame 7 across configs | back to 0 |
|---|---|---|---|
| DWAA | **10.60% differing** | **0%** | **0%** |
| PIZ | **54.82% differing** | **0%** | **0%** |

This is structural rather than lucky: **the stride policy is gated on
`playTimer_.isActive()`**, so paused stepping and random access take the legacy
`+-1` window verbatim. `prefetchNeighbors()` is reached from seven places and
only two of them are the playback tick.

---

## WHAT THE RESIDUE IS, AND THE ONE-CONSTANT CHANGE THAT WOULD CLOSE IT

Every DWAA cost above B is an **issued prediction that missed**: 42.5 ms of
synchronous load inside a tick that then still has to read the real frame.
The policy issues one whenever the EMA drifts within 0.15 of an integer, and on
a file whose true stride is ~1.4 that happens occasionally and is wrong when it
does.

**Requiring the stride to be confidently 1 -- rather than confidently any
integer -- would collapse C onto B for DWAA and leave C identical to A for
PIZ**, because PIZ's stride is exactly 1.0 and DWAA's is never near it. That is
a change to one predicate, it needs no cache work, and it is **not built and not
measured**. It is the obvious next experiment if this direction is continued.

The wider version -- predicting non-unit strides properly -- would need the
prediction to come from the **scheduler's own accumulator** rather than from a
history average, since the scheduler already knows what the next target will
be. That is a real design and is not attempted here.

---

## WHAT IS STILL TRUE REGARDLESS OF POLICY

**None of these three configurations makes the DWAA file hold real time.**
`handler>budget` is every frame in all three columns. At B's 58.80 ms per frame
`read_image` alone is **41.27 ms -- 99% of a frame budget** -- so reaching 24 fps
still needs the read off the UI thread or made cheaper. Unchanged from the
decomposition's ranking, and not started.

---

## PROVENANCE AND SCOPE

Commit `b96f14c`. Knobs: `TRACE_SEQ_PREFETCH_STRIDE=1` (this policy),
`TRACE_SEQ_PREFETCH=0` (no prefetch), `TRACE_SEQ_PROFILE=1` (the stage table
and the cache/decline counters). All default off.

**Cache architecture is unchanged** -- same `FrameCache{1}`, same put/get, same
radius. **No off-thread EXR reads. Alpha prefill untouched. No GPU or OCIO
work.** Every figure is with no colour transform active.
