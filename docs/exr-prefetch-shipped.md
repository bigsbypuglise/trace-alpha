# Reverse playback validated, and the stride-aware gate ships

Record of what was measured and what was decided. 2026-08-25, branch
`exr-stage0-dependencies`, physical panel **5120x1440 @ 239.999000 Hz**
(re-checked before measuring). Closes the EXR prefetch session.

**THE STRIDE-AWARE GATE (run of 4 unit steps) IS NOW THE SHIPPING DEFAULT.**
`TRACE_SEQ_PREFETCH_STRIDE=0` is the rollback to the fixed `+-1` window.

---

## THE LAST MEASUREMENT: REVERSE PLAYBACK

Never executed before this pass. The signed counter was a claim about
construction -- every prior figure for this policy was forward playback.
Harness `scripts/measure/seqreverse.ps1`: End, J (reverse 1x), hold, capture
before K, then K, then +3 / -3 steps.

### `R2_OP_Stacks_01` -- PIZ, 217 frames

| | fixed +-1 | **gate 4** |
|---|---|---|
| presented | 20.20 (**84.2%** real time) | **21.64 (90.2%)** |
| frames / elapsed | 182 / 9.01s | **195 / 9.01s** |
| **skip** | 34 | **21** |
| handler p50 / max | 41.8 / 53.8 ms | 41.5 / **47.7 ms** |
| `handler>budget` | 34 of 181 | **20 of 194** |
| `tick-stall` | 0 | **0** |
| cache HIT / MISS | 147 / 37 | 93 / 104 |
| ISSUED / DECLINED | -- | 110 / 81 |
| loads per presented frame | 1.20 | **1.12** |

### `MultlayerAces` -- DWAA, 27ch, 97 frames

| | fixed +-1 | **gate 4** |
|---|---|---|
| presented | 6.78 (**28.3%** real time) | **14.65 (61.1%)** |
| frames / elapsed | 28 / 4.13s | **60 / 4.09s** |
| **skip** | 70 | **36** |
| handler p50 / max | 147.8 / 187.6 ms | **67.4 / 144.4 ms** |
| `handler>budget` | 27 of 27 | 59 of 59 |
| **`tick-stall`** | **26** | **2** |
| cache HIT / MISS | 1 / 29 | 0 / 62 |
| ISSUED / DECLINED | -- | **0 / 56** |
| loads per presented frame | 2.89 | **1.13** |

**REVERSE IMPROVES ON BOTH FILES, AND THAT IS THE SIGNED COUNTER DOING REAL WORK
RATHER THAN MERELY COMPILING.** The fixed window prefetches both neighbours; in
reverse the `+1` is the frame just left, so half its loads were pure waste. That
is why PIZ -- which gains nothing from this policy going forward -- gains 6
points going backward.

### Exactness after stopping and stepping

| file | negative control (stop vs +3, one build) | frame stopped on | after +3 | back to stop |
|---|---|---|---|---|
| PIZ | **47.54% differing** | **0%** | **0%** | **0%** |
| DWAA | **8.84% differing** | **0%** | **0%** | **0%** |

The negative control is listed first because it licenses the rest: a build
frozen on one frame would read 0% on all three cross-config columns.

---

## THE DECISION

**Gate 4 is the shipping default.** The fixed `+-1` window is preserved behind
`TRACE_SEQ_PREFETCH_STRIDE=0`.

**The flip was verified from the counters in both directions**, not from the
command line:

| configuration | frames | DECLINED | LEGACY +-1 | accounted/frame |
|---|---|---|---|---|
| default, no knob | **62** | **58** | 5 | **59.79 ms** |
| `TRACE_SEQ_PREFETCH_STRIDE=0` | **28** | -- | **29** (every frame) | **144.63 ms** |

### What ships, in one table

| | forward, fixed | forward, shipped | reverse, fixed | reverse, shipped |
|---|---|---|---|---|
| **DWAA 27ch** | 28.7-29.2% | **62.2-63.1%** | 28.3% | **61.1%** |
| **PIZ 3ch** | 99.9% | **99.9%** | 84.2% | **90.2%** |
| **PNG 3ch** | 100.0% | **100.0%** | -- | -- |

- **DWAA improved from ~29% to ~62-63%** forward and 28.3% -> 61.1% reverse.
- **PIZ remained in the shipping performance class** -- identical forward,
  better reverse.
- **PNG remained unchanged** -- 100.0%, cache HIT 167/1, both policies.
- **Exactness is preserved STRUCTURALLY**, not by a passing test: the policy is
  gated on `playTimer_.isActive()`, so paused stepping and random access take
  the legacy window verbatim. `prefetchNeighbors()` is reached from seven places
  and only two are the playback tick.

---

## KNOWN CAVEAT, RECORDED AND DELIBERATELY NOT FIXED

**DWAA remains ~1-2 points behind turning prefetch off entirely** (63.3-64.1%),
and its handler tail stays higher (max ~100-122 ms against ~74-75). **All of it
is the five-frame warm-up window, which still uses the legacy `+-1` behaviour**
-- two loads per frame for the first five presented frames, before the gate has
enough samples to decide anything.

That is measured and attributed, not suspected. **It was deliberately NOT
optimised in this session.** Closing it is a separate one-line experiment
(decline during warm-up when stride-aware is on) and carries a real trade: it
would cost PIZ a handful of early cache misses, and the run-8 result proved that
starving the cache early on a file with ~4 ms of headroom can start a skip
cascade that never converges.

---

## OTHER OPEN GAPS, NAMED

- ~~**No owner hand-test.**~~ **CLOSED 2026-08-25: the owner watched it play and
  ruled ~15 fps on the 27-channel DWAA working file USABLE for review**, and
  explicitly not a blocker for the EXR milestone. The counters are no longer the
  only evidence. The limit itself is unchanged and is now a documented,
  accepted one rather than an open question -- see
  `docs/exr-release-notes.md` and the known-gaps entry in
  `docs/release-notes-alpha.md`.
- ~~**The policy's state is not on the HUD.**~~ **CLOSED 2026-08-25** --
  `seq-prefetch` is the fourth line of the image-sequence HUD, naming the policy
  (including the rollback, as `legacy (env)`), the stride, the gate's run
  counter, issued/declined/legacy and cache hit/miss. Record and control
  measurement: `docs/exr-prefetch-hud.md`. **Note that doc also closes the
  "unexplained" rep-to-rep spread below as OS file caching -- discard the first
  pass over a sequence, it reads roughly half the warm rate.**
- **Three sequences measured** (2 EXR + 1 PNG), forward on all three and reverse
  on both EXR files.

**Not pursued, by instruction:** gate 8 (refuted --
`docs/exr-unit-run-8-refuted.md`), scheduler-accumulator prediction, off-thread
EXR reads, alpha-prefill optimisation, GPU/OCIO work. **Cache architecture,
radius and put/get are unchanged throughout.**
