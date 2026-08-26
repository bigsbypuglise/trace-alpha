# The neighbour-prefetch experiment: measured, not projected

Record of what was measured. 2026-08-24, branch `exr-stage0-dependencies`,
physical panel **5120x1440 @ 239.999000 Hz**, shipping renderer, `TRACE_HUD=1`,
scratch settings, root pass, no colour transform, warm (a discarded warm-up run
precedes each set).

**Knob `TRACE_SEQ_PREFETCH=0`, default OFF -- i.e. prefetch stays ON and
shipping behaviour is unchanged.** Nothing about the cache or playback
architecture was redesigned. No policy is permanent yet.

---

## THE RESULT IN ONE LINE

**On the 27-channel DWAA sequence, disabling the neighbour prefetch takes
playback from ~29% to ~64% of real time.** The decomposition projected 59.1 ms
per frame; the measurement reads **58.82 ms**, within 0.5%.

**And the same change is a small NET NEGATIVE on the plain PIZ sequence**, which
is why the control was run: there prefetch is not waste at all.

---

## DWAA -- `MultlayerAces`, 97 frames, 1920x1080, 27ch, 3 reps each

| | prefetch ON | prefetch OFF |
|---|---|---|
| **presented** | 27.2 / 28.3 / **29.2%** | **64.0 / 63.9 / 63.0%** |
| frames / elapsed | 27-29 / ~4.13s | **62-63** / ~4.10s |
| **skip** | 71 / 69 / 70 | **34 / 34 / 34** |
| handler **p50** | 163.1 / 154.6 / 149.6 ms | **65.0 / 67.1 / 67.1 ms** |
| handler **max** | 208.2 / 189.4 / 195.2 ms | **72.6 / 71.5 / 73.7 ms** |
| `handler>budget` | 26 of 26 · 27 of 27 · 28 of 28 | 62 of 62 · 62 of 62 · 61 of 61 |
| **`tick-stall` (>100ms)** | 23 / 24 / 24 | **0 / 0 / 0** |
| media | 98.8 / 98.2 / 99.5% | 98.5 / 98.4 / 97.6% |
| **loads per presented frame** | 79 / 29 = **2.72** | 63 / 62 = **1.02** |
| **cache HIT / MISS** | **3 / 27** (10%) | 0 / 63 (0%) |
| accounted per frame | **140.96 ms** | **58.82 ms** |

**`handler>budget` is still every frame in both columns, and that is the honest
reading**: 58.82 ms is still above the 41.67 ms budget, so the file presents
about two frames in three rather than one in three. It does not hold real time
and this change was never going to make it.

**What DID become clean is the stalling.** `tick-stall` -- ticks over 100 ms --
goes **23-24 to 0**, and handler max falls from ~195 ms to ~73 ms. That is the
difference between a file that hitches and a file that is merely slow.

### The stage table, same runs

| stage | ON per frame | OFF per frame |
|---|---|---|
| `read_image` | 112.02 ms | **41.27 ms** |
| `alpha prefill` | 11.96 | 4.45 |
| `map+convert` | 10.42 | 10.29 |
| `open+spec` | 5.04 | 1.76 |
| `group/choose` | 0.60 | 0.33 |
| `upload` | 0.68 | 0.62 |
| `loader tail` | 0.21 | 0.08 |
| `buffer alloc` | 0.04 | 0.01 |
| **accounted** | **140.96** | **58.82** |
| *prefetch (cross-check)* | *83.41* | ***0.00*** |

`prefetch` reading exactly 0.00 ms with 62 calls is the knob proving itself
live, from inside the build rather than from the command line.

---

## PIZ -- `R2_OP_Stacks_01`, 217 frames, 3ch, THE CONTROL

| | prefetch ON | prefetch OFF |
|---|---|---|
| presented | **99.8 / 99.9 / 99.9%** | 99.7 / 99.7 / 99.7% |
| skip | 0 | 0 |
| handler p50 | 41.6 / 41.5 / 41.6 ms | 41.7 / 41.6 / 41.5 ms |
| handler max | 36.1 / 37.3 / 37.2 ms | 37.1 / 37.1 / 39.2 ms |
| `handler>budget` | **0 of 215** | **0 of 215** |
| `<0.9x` cadence bucket | **1 / 0 / 0** | **6 / 9 / 5** |
| drift | -14 / -13 / -13 ms | **-26 / -26 / -26 ms** |
| loads per presented frame | 217 / 216 = **1.005** | 217 / 216 = **1.005** |
| **cache HIT / MISS** | **216 / 1** (99.5%) | 0 / 217 (0%) |
| accounted per frame | 26.63 ms | 27.24 ms |

**PREFETCH IS NOT WASTE HERE AND THE LOAD COUNT IS WHAT SAYS SO: 1.005 loads per
frame in BOTH columns.** With the playhead advancing exactly one frame per
present, the `+1` neighbour it loads is precisely the frame wanted next, and the
`-1` is already inside the radius-1 window. It is one necessary load moved a
tick earlier, off the critical path before presenting -- not an extra one.

The cost of removing it is therefore small but real and consistent: presented
**99.8-99.9% -> 99.7%**, drift **-13 -> -26 ms**, and the `<0.9x` bucket
**0-1 -> 5-9** frames per run. Both configurations still hold real time with
`skip 0` and `handler>budget 0 of 215`.

---

## EXACTNESS AND PAUSED REVIEW -- UNCHANGED, ON BOTH FILES

`scripts/measure/seqstep.ps1`, 7 steps forward then 7 back, both configurations:

| file | negative control (frame 7 vs 0, same build) | frame 7 across configs | back to 0 across configs |
|---|---|---|---|
| DWAA | **10.60% differing** | **0%** | **0%** |
| PIZ | **54.82% differing** | **0%** | **0%** |

**The negative control is listed first because it is what licenses the other two
columns.** A build showing one frozen frame forever would read 0% on both
cross-config comparisons and pass a test that only looked at them.

Frame indices read off the captures rather than inferred: forward lands on
`frame 7 | Frame: 7/96` with the slider at 7, and the return lands on
`frame 0 | Frame: 0/96` with the slider at 0. Paused review renders correctly
with the full media line intact in both configurations.

---

## WHAT THIS MEANS FOR THE POLICY -- and it is NOT "remove the prefetch"

The two files disagree, and the reason they disagree is the whole finding:

- **PIZ**: playhead advances **1** frame per present. `+-1` predicts correctly.
  **99.5% hit rate, 1.005 loads/frame. Prefetch earns its keep.**
- **DWAA**: playhead advances **~3.4** frames per present, because the file is
  over budget and the scheduler skips. `+-1` predicts wrongly almost every time.
  **10% hit rate, 2.72 loads/frame. Prefetch is 59% of the frame, wasted.**

So the defect is not that prefetching exists. **It is that the prefetch window
is a fixed `+-1` while the playback stride is not 1.** The obvious shape of a
fix -- prefetch the frames the playhead is actually going to land on, i.e. let
the window follow the stride -- would keep PIZ's 99.5% hit rate AND remove
DWAA's waste, and would need no cache redesign.

**That is a recommendation, not a measurement, and it is not built.** Blanket
removal is measured, and it trades a small regression on well-behaved sequences
for a large win on struggling ones.

**Neither option makes the DWAA file hold real time.** At 58.82 ms per frame it
is still 1.4x over budget with `read_image` alone at 41.27 ms, which is 99% of a
frame budget by itself. Reaching 24 fps still needs the read off the UI thread
or made cheaper -- unchanged from the decomposition's ranking.

---

## PROVENANCE

`f8ab018` knob + cache counters, `fe1fb82` the stepping harness.
`TRACE_SEQ_PREFETCH=0` is default off; `TRACE_SEQ_PROFILE=1` was on for every
run in both columns, so the two columns are the same measurement.

**Alpha prefill was not touched**, per instruction, and is visible in the table
above as 11.96 -> 4.45 ms purely because it now runs once per frame instead of
2.72 times. **No GPU or OCIO work was started.** ACES remains a separate track
and every figure here is with no transform active.
