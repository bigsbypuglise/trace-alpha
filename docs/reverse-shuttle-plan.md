# Reverse shuttle — measurement pass and architecture proposal

**Status: MEASUREMENT COMPLETE, DESIGN PROPOSED, NOTHING IMPLEMENTED.**
By instruction (`docs/next-session-prompt.md`, owner 2026-08-10) this phase begins with
measurement and an architecture proposal. No reverse-shuttle code was written. The only
new file that ships behaviour is a measurement harness, `scripts/measure/revplay.ps1`.

Companion documents: `docs/gpu-initiative-plan.md` §29.2–29.3 (the J-K-L scheduler fault
that had been hiding every previous reverse figure), §15 (the sampling precedent and its
four failed gate inferences), §14 (the async worker whose lease model this reuses), §6
(the post-mortem on the two reverted async attempts).

---

## 1. Conditions — read these before comparing any number here to anything else

| | |
|---|---|
| build | `df56569`, local VS2022 / Qt 6.10.2 / vcpkg release build |
| renderer | `d3d11` (the default since 2026-08-10), planar upload on, step-9 shader on |
| reverse cache | 384MB default except where a run says otherwise |
| display | **physical panel, 5120x1440 @ 239.999Hz** (`refresh.ps1`, `QueryDisplayConfig`) — **not Parsec** |
| window | `win 1280x829` (16:9 media) / `win 1280x815` (4444 and 60fps), outer 1296x868 |
| audio | silent throughout: J is reverse and reverse is silent by design, so no `TRACE_NO_AUDIO` control is needed and all files are on the same scheduler |
| gesture | `revplay.ps1`: click the groove at 0.95, press J *n* times, hold 9s, capture, K |

Every figure below is a **single run**. Where a claim rests on a difference rather than on
a level, the control is named next to it. `hitch`/`stalls` are not quoted for reverse
because reverse presents through the playback tick, not the paint path — the cadence
percentiles are the equivalent, and they are threshold-free.

**The speed ladder that exists today is 1x / 2x / 4x** (`PlaybackController::jogReverse`
doubles and clamps at 4). The owner's interface asks for 2x / 5x / 10x / 30x. 5x, 10x and
30x are therefore **not measurable in the app as it stands**; §5 models them from measured
per-frame costs and states what would falsify the model.

---

## 2. The instrument

`scripts/measure/revplay.ps1` — new, and the first reverse harness the project has had.
`revplay.ps1` from the previous session was ad-hoc and uncommitted.

It finds the groove by the unfilled-track colour as `scrub.ps1` does, **clicks** (not
drags) to 0.95 of the clip so the cache starts empty and the playhead is not shuttled
there, presses J *n* times, holds, captures **before** K (the cumulative counters survive
the stop but `speed` does not, and a run whose speed cannot be confirmed from its own
capture cannot be quoted), then optionally runs the landing-exactness gesture.

`-StepCheck` presses Right then Left and compares the picture with the frame reverse
stopped on. It is a real check rather than a tautology: on 4K H.264 it reads **`+1 moved
7.5%, -1 returned to 0%`** — the +1 leg is the control that proves the comparison can
detect a moved picture at all, which is the mistake `lifecycle.ps1` documents.

`-Traverse` waits for the picture to go still instead of holding a fixed time. It is
retained but **was not used for any figure here**: its sampling resolution is ~200ms,
which is too coarse for a 2-second traverse, and the HUD's own `frames` / `elapsed` pair
is exact and answers the same question.

---

## 3. The reverse baseline, across the test set

All d3d11, 384MB cache, conditions as §1. `%` is of the demanded speed, not of real time —
so 100% means "reverse actually ran at the speed that was asked for".

| file | GOP | J presses | presented f/s | % of demanded | handler avg / max (ms) | over budget | seeks | rev-hit | p50 / p95 / p99 (ms) |
|---|---|---|---|---|---|---|---|---|---|
| 4K H.264, 24.000 | 30 | 1 (-1x) | 20.88 | **87.0%** | 8.30 / 103.8 | 11 of 114 | 11 | 88.8% | 41.8 / 118.9 / 145.7 |
| 4K H.264 | 30 | 2 (-2x) | 36.33 | **75.7%** | 8.43 / 108.7 | 11 of 111 | 11 | 88.8% | 21.0 / 101.1 / 128.7 |
| 4K H.264 | 30 | 3 (-4x) | 56.62 | **59.0%** | 8.70 / 106.8 | 11 of 108 | 11 | 88.8% | 10.2 / 91.5 / 115.5 |
| 1080p H.264, 23.976 | 48 | 1 (-1x) | 22.88 | **95.4%** | 3.09 / 121.2 | 5 of 207 | 5 | 96.7% | 41.9 / 43.2 / 152.8 |
| 1080p H.264 | 48 | 2 (-2x) | 43.29 | **90.3%** | 2.87 / 183.7 | 8 of 388 | 8 | 97.5% | 21.0 / 22.2 / 132.9 |
| 1080p H.264 | 48 | 3 (-4x) | 77.78 | **81.1%** | 2.92 / 186.9 | 8 of 385 | 8 | 97.5% | 10.3 / 12.0 / 121.7 |
| 4K H.264 60fps | ~30 | 1 (-1x) | 41.44 | **69.1%** | 9.05 / 119.4 | 15 of 153 | 15 | 89.1% | 16.9 / 102.0 / 127.0 |
| 4K H.264 60fps | ~30 | 3 (-4x) | 79.59 | **33.2%** | 8.88 / 136.8 | 14 of 148 | 15 | 89.1% | 7.2 / 88.2 / 130.8 |
| 4K ProRes 422 HQ | intra | 1 (-1x) | 23.97 | **99.9%** | 10.09 / 13.7 | **0 of 159** | 160 | 0.0% | 41.6 / 43.5 / 44.3 |
| 4K ProRes 422 HQ | intra | 3 (-4x) | 57.17 | **59.6%** | 10.25 / 13.5 | 73 of 154 | 160 | 0.0% | 18.6 / 22.2 / 23.0 |
| 4K ProRes 4444 | intra | 1 (-1x) | 23.94 | **99.7%** | 24.46 / 28.2 | **0 of 216** | 217 | 0.0% | 41.7 / 43.7 / 44.3 |
| 4K ProRes 4444 | intra | 3 (-4x) | 31.97 | **33.3%** | 24.67 / 28.4 | 243 of 243 | 248 | 0.0% | 31.3 / 33.3 / 34.2 |

**The 4K H.264 1x row reproduces §29.3** (86.7% there, 87.0% here; `handler>budget 11 of
110` there, `11 of 114` here; `seeks 13`/`11`; `rev-hit 88.5%`/`88.8%`). That is the check
that the harness measures the same thing the hand-driven run did.

### 3.1 The two codec families behave in opposite ways, and the intuition is inverted

**ProRes reverse at 1x is essentially perfect and long-GOP reverse is not.** 4444 —
the file that is hardest for everything else in this project — reads **99.7% of real time,
zero handlers over budget, and a cadence spread of 41.7 / 43.7 / 44.3ms**. Every frame is
a keyframe, so a backward seek lands on the target, the walk is empty, and each frame
costs one decode of known price. Uniform work, perfect cadence.

4K H.264 at 1x reads 87.0% with a **p95 of 118.9ms against a p50 of 41.8ms**. The work is
not large, it is *lumpy*.

**`rev-hit 0.0%` on both ProRes files is not a cache failure** and must not be treated as
one — §15.1 established this and it is confirmed again: there are no intermediate frames
to cache because a seek lands on the target.

### 3.2 Where each family runs out

ProRes falls off a cliff by 4x, and it falls off *smoothly*: 4444 at 4x reads `p50 31.3 /
p95 33.3 / max 34.2` — a rock-steady 32 f/s that is simply the wrong speed. The deadline
scheduler converts overload into a stable slower cadence rather than into jitter, which is
a real asset for this phase and is worth stating explicitly: **for uniform work, "stable,
intentional visual cadence" is already what the shipped scheduler produces.**

Long-GOP does the opposite. At 4x the 4K H.264 p50 is on grid at 10.2ms and the p95 is
91.5ms — the same eleven GOP-walk lumps as at 1x, now nine slots wide each.

---

## 4. The central finding: the decoder is idle, and the deficit is burstiness

Duty cycle is the handler's average cost against the slot it had to fit in.

| file | speed | handler avg | slot | **duty cycle** | achieved |
|---|---|---|---|---|---|
| 1080p H.264 | 1x | 3.09ms | 41.71ms | **7%** | 95.4% |
| 4K H.264 | 1x | 8.30ms | 41.67ms | **20%** | 87.0% |
| 4K ProRes 422 HQ | 1x | 10.09ms | 41.67ms | 24% | 99.9% |
| 4K H.264 60fps | 1x | 9.05ms | 16.67ms | 54% | 69.1% |
| 4K ProRes 4444 | 1x | 24.46ms | 41.67ms | 59% | 99.7% |
| 1080p H.264 | 4x | 2.92ms | 10.43ms | 28% | 81.1% |
| 4K H.264 | 4x | 8.70ms | 10.42ms | 84% | 59.0% |
| 4K ProRes 422 HQ | 4x | 10.25ms | 10.42ms | 98% | 59.6% |
| 4K ProRes 4444 | 4x | 24.67ms | 10.42ms | 237% | 33.3% |

**Read the top two rows against the bottom two.** At reverse 1x on the long-GOP files the
decoder is idle 80–93% of the time and still misses real time. At 4x on ProRes it is
oversubscribed 2.4x and delivers exactly what that predicts. These are two different
failures and one mechanism cannot address both:

- **Long-GOP at low speed: a scheduling failure, not a throughput failure.** Eleven lumps
  of ~104ms each, spread through 5.5 seconds that are 80% empty. Nothing needs to be
  decoded *faster*; the lumps need to stop landing inside the presentation slot.
- **ProRes at high speed: a pure throughput deficit**, exactly as §15.1 measured for the
  drag. The only lever is decoding fewer frames.

### 4.1 §15.3's decline of directional prefetch does not carry over, and here is the number

§15.3 declined speculative lookahead because *"H.264 backward measures 59–74% supply,
which means the worker is saturated for the whole gesture; there is no slack"*, and closed
with: **"do not revisit speculative lookahead without first showing measured idle worker
time coinciding with stalls."**

That condition is now met, on the reverse path, and it is met by a wide margin:

- idle time: **80% at 4K H.264 1x, 93% at 1080p 1x**;
- coinciding with the lumps: yes by construction — the 11 over-budget handlers and the 11
  seeks are the same 11 events, and the p95/p50 ratio is 2.8x.

The reason the answers differ is the one the brief named: a drag's target is the pointer
and is unpredictable, so lookahead is speculation. **A reverse run's target is
arithmetic** — at speed *S* the frame wanted in slot *k* is `anchor − round(k × S)` — so
lookahead is not speculation at all. It is scheduling work that is already known to be
required.

This is a re-derivation, not a citation, and it goes the other way from §15.3. Both are
correct for their own workload.

---

## 5. Where the cost actually is — a two-point solve

The cache fill can be switched off (`TRACE_SEEK_CACHE_WINDOW=0`), which makes **every**
reverse step a seek plus a walk from the keyframe. The walk length is then uniform over
`0..G-1`, so the mean handler and the max handler are two points on one line and the line
can be solved for its slope and its intercept.

| file | fill=0 handler avg | max | mean walk | walk at max | → per walked frame | → **fixed cost** |
|---|---|---|---|---|---|---|
| 4K H.264 (G=30) | 67.91ms | 105.4ms | 14.5f | 29f | **2.59ms/f** | **30.4ms** |
| 1080p H.264 (G=48) | 71.22ms | 112.2ms | 23.5f | 47f | **1.74ms/f** | **30.2ms** |

Supporting figures for the fill=0 runs: 4K H.264 `presented 10.80 (45.0%) | seeks 98 |
rev-hit 0.0% | dec avg 55.13 | sws avg 3.10 | seek avg 7.69`; 1080p `presented 10.45
(43.6%) | seeks 95 | rev-hit 0.0% | dec avg 63.11 | sws avg 0.60 | seek avg 5.82`.

**Two things follow, and the second one is the surprise.**

1. **A keyframe-aligned reverse sample — seek, decode one I-frame, present, no walk —
   costs about 30ms.** That is the price of the cheapest possible unit of coarse reverse
   motion on long-GOP, and it is what §6 of the proposal below is built on.

2. **That 30ms is the same at 1080p and at 4K.** It is therefore not pixel work. The seek
   itself measures 5.8–7.7ms, so roughly **20–24ms per seek is being spent somewhere that
   does not scale with resolution** — and the obvious candidate is the frame-threaded
   decoder refilling its pipeline after every flush. CLAUDE.md already records exactly this
   mechanism as the reason **intra-only codecs were moved to `FF_THREAD_SLICE`** in July
   2026 ("*every seek+flush stalled ~thread-count packets (~100ms) before emitting one
   frame*"); long-GOP codecs kept `FRAME|SLICE` "for playback throughput". Reverse
   playback is seek-dominated, so it may be paying that cost on every GOP.

   **This is a hypothesis derived from an intercept, not a measurement.** It predicts that
   a long-GOP decoder in slice-only threading would show the ~30ms intercept collapse
   while forward playback throughput falls. Measuring it needs a code change (there is no
   env knob for threading) and it is the **first experiment of the next session**, before
   any architecture is built, because it changes the cost of the coarse-scan regime by a
   factor of two or more and therefore moves the handover point.

---

## 6. The cache-fill experiment — misses per GOP, and a stale capacity term

`long-gap min/med/max` reports the spacing, in presented frames, between cadence gaps over
1.5x the period. On 4K H.264 at 1x it reads **4/13/13** — a lump every 13 frames on a file
whose GOP is 30. The seek log confirms it exactly; requested frames at each seek were
`107, 94, 89, 76, 63, 59, 46, 33, 29, 16, 3` — differences of 13, 5, 13, 13, 4, 13, 13, 4,
13, 13, with the short ones at GOP boundaries.

**Reverse pays 2.3 seeks per GOP where one would do.** The cause is that the seek-walk
cache fill window for `RequestMode::Playback` is an **18ms conversion budget** written for
a *Step landing* — where one frame is wanted and every speculative conversion is delay in
front of it. During reverse playback the frames walked past are precisely the next 29
requests. That is the same argument §15.2 made for the Scrub path, and it was never
applied to reverse.

| 4K H.264, reverse 1x | presented | % real time | seeks | rev-hit | long-gap med | handler avg | handler max | over budget |
|---|---|---|---|---|---|---|---|---|
| control (384MB, shipped) | 20.88 | 87.0% | 11 | 88.8% | 13 | 8.30 | 103.8 | 11 of 114 |
| control, second run | 21.54 | 89.7% | 10 | 89.7% | 13 | 7.75 | 105.8 | 10 of 113 |
| `TRACE_REVERSE_CACHE_MB=1024` | 22.06 | 91.9% | **4** | 94.8% | 30 | 5.06 | 132.0 | 4 of 113 |
| `…=1024` + `TRACE_SEEK_CACHE_WINDOW=30` | 22.42 | **93.4%** | **3** | 95.7% | **30** | **4.18** | 126.1 | **3 of 113** |

The long-gap spacing moving 13 → **30 = exactly the GOP** is the mechanism confirming
itself. Average handler cost halves. The price is the tail: max handler 103.8 → 126.1ms
and p99 145.9 → 165.7ms, because one seek now buys a whole GOP instead of half of one.
**That trade is only worth taking if the lump is off the UI thread** — which is §7.

**Why the first attempt at this experiment appeared to do nothing.** Setting
`TRACE_SEEK_CACHE_WINDOW=30` at the 384MB default changed nothing at all (87.0% → 89.7%,
spacing still 13). The knob is clamped to `reverseCacheCapacity`, and that is computed as
`384MB / (w × h × 4)` — the **BGRA** footprint — which reads **11** at 4K. Since GATE C the
entries are planar and the real depth is 32. So the clamp silently held the fill window at
11 frames. `reverseCacheCapacity` is a **pre-GATE-C currency term that survived the
change**: the fifth instance of this project's "a deferred item's premise expires" rule,
and the first where the expired premise is live code rather than a note.

**1GB of cache is not being proposed.** §26.5 approved 384MB and explicitly declined more.
The finding is that the *fill window* is the binding term, and it can be widened at 384MB
once the capacity term is computed from the bytes actually stored.

---

## 7. Proposal

Three components. Each is separately measurable, separately revertable, and only the first
two are needed for 1x–5x.

### 7.1 Component A — a reverse decode pipeline on the existing lease (fixes cadence)

**What it is.** Reuse `ScrubDecodeWorker`'s ownership model verbatim: one
`VideoDecoderFFmpeg`, leased to one worker for the duration of a reverse run, returned
through the single `reclaimDecoder()` choke point. §14.2–14.8 already argue this design
against all six defects of the reverted attempts and that argument is unchanged.

**What differs from the drag.** The request is not "one frame, latest wins" but "keep an
ordered ring of the next *N* frames of this sequence filled". The sequence is
`anchor − round(k × S)`, so the worker can run ahead without speculating. The UI tick pops
the ring head and presents it; it never decodes.

**Ring depth.** Enough to cover the worst lump: `ceil(worst handler / slot)`. Measured
worst handler is 126ms (with the wide fill of §6); at 1x that is 3 frames, at 4x 12, at
the modelled 10x about 30. **A 32-frame ring covers every measured case**, and its memory
is the frames it holds — which are reverse-cache entries anyway, so this is a reference
count, not a second copy.

**Why the idle time is really there to spend**: §4, 80–93% at 1x.

**Cost to normal playback: none by construction.** The lease is taken when a reverse run
starts and returned when it stops. Forward playback continues to decode synchronously on
the UI thread exactly as it does today — the same boundary §14.9 drew, and the same check
applies: `worker posted 0` through a forward playback run.

### 7.2 Component B — speed-derived sampling (fixes speed)

**The stride is the commanded speed.** Not an estimate, not an EMA, not a measured
capacity. At speed *S* the shuttle presents frames *S* apart, at a constant presentation
rate of one frame per slot.

**This is the entire answer to the oscillation question**, and it is worth stating in the
form the brief asked for. §15's four failed gate inferences — a latch, a decaying mean, a
mean per request, a mean per seek with a threshold — all failed the same way: each
inferred the stride from *measured decode cost*, and the stride changes the cost, so the
loop feeds back on itself. §15 records the runaway explicitly ("*a higher stride lowers the
measured rate which raises the stride*"). Here the stride is an **input**: the user pressed
a button. Nothing the decoder does can move it. There is no loop to oscillate.

**Cadence is held constant and speed changes only what is shown.** Speed never changes how
often a frame is presented. That is "stable, intentional visual cadence" as a structural
property rather than as a tuning outcome, and it is the third instance of the standing
drag-path priority, exactly as the owner licensed for accelerated reverse.

**This subsumes ProRes entirely.** ProRes has no GOP, one decode per frame, uniform cost;
sampling at stride *S* multiplies the achieved speed by *S* at an unchanged presentation
rate. From the measured 1x rows: 422 HQ sustains ~99 presents/s of work at 10.09ms each,
so stride 10 gives 10x at 24 presents/s with 76% headroom; 4444 sustains ~40 at 24.46ms,
so 30x at 24 presents/s needs 24 × 24.46 = 587ms of decode per second — **feasible, at
59% duty.** No new mechanism is needed for either.

### 7.3 Component C — the long-GOP snap (the different mechanism the brief demands)

§15's `AV_CODEC_PROP_INTRA_ONLY` gate refuses striding on long-GOP for a measured reason:
an arbitrary strided step leaves the walked GOP and pays a fresh seek and walk, so cost per
presented frame goes **up** (§15: 4K H.264 backward `hits 85.4 → 13.3%`, `decode 90.0 →
13.9 f/s`). A reverse shuttle at 30x is precisely the case that gate exists to reject, and
the brief is right that turning the same mechanism up is not the answer.

**The reverse shuttle is not arbitrary, and that is what makes a different answer
available.** The sample points can be *chosen*, and choosing them on the keyframe grid
turns the expensive case into the cheap one: a keyframe-aligned sample pays a seek and one
I-frame decode and **no walk at all** — the 30ms intercept of §5 against 2.59ms per walked
frame.

So there are two regimes, both presenting one frame per slot:

- **Walk regime.** Seek once per GOP, walk it, cache all *G* frames, present every *S*th
  from the cache. Cost per GOP `= fixed + G·k + (G/S)·present`.
- **Snap regime.** Seek to the keyframe nearest at-or-below the ideal target and decode
  only it. Cost per presented frame `= fixed + present`.

**The handover is `S ≥ G`,** because in snap mode presenting one keyframe per slot at the
source frame rate *is* a speed of exactly `G`. Below that the snap would have to hold each
keyframe for several slots, which lowers the presentation rate and reads as choppy;
above it, keyframes are skipped, which is free.

**Modelled achievable speed on 4K H.264** (G=30, fixed=30.4ms, k=2.59ms, present≈9ms):

| commanded | regime | model | verdict |
|---|---|---|---|
| 1x | walk | 79 f/s = 3.3x capacity | comfortable |
| 2x | walk | 123 f/s = 5.1x | comfortable |
| 5x | walk | 185 f/s = 7.7x | comfortable |
| 10x | walk | 222 f/s = **9.3x** | **marginal — misses 10x** |
| 30x | snap | 762 f/s = **31.7x** | reaches it, at ~25 presents/s |

**10x on long-GOP is the one speed the model does not clear**, and it clears it if §5's
threading hypothesis holds (a smaller intercept moves every walk-regime row up). That is
the second reason the threading experiment comes first.

**Why the handover cannot oscillate.** It is a comparison of two numbers, neither of which
moves with load: the commanded speed *S*, and the keyframe interval *G*. *G* is **observed,
not estimated** — after a backward seek the decoder already resolves the landed frame's
true index from its PTS (`seekResolvePending`), so `target − landedIndex` is a direct
measurement of the distance to the keyframe, available free on every miss. It is a property
of the file. **Every one of §15's failed inferences estimated a *cost*; this estimates a
*structure*.** Costs move with machine load, with window size, and with the stride itself.
A GOP length does not. A single-notch hysteresis is still cheap insurance since *S* takes
four discrete values, but it is insurance, not the mechanism.

### 7.4 What is deliberately NOT proposed

- **No second decoder, ever** (§6 defect 1).
- **No change to the drag path.** Exact scrub release, the sampling gate, the preview
  resolution and the 384MB budget are all untouched. §26.6 and §28.6 are signed off.
- **No change to forward playback.** The lease is not taken outside a reverse run.
- **No cache budget increase.** §6 above shows the *fill window* is the binding term, and
  §26.5 declined more memory.
- **No GATE E step 2.** Vsync snapping remains stopped by owner decision.

---

## 8. The invariants, and how each is held

The brief is explicit that the last two goals are where every previous attempt actually
failed. Each gets a mechanism, not an intention.

| invariant | mechanism | how it is checked |
|---|---|---|
| **frame order** | the ring is ordered by construction — the worker produces the arithmetic sequence in order and the UI pops in order. A stale result is *discarded*, never reordered. | `delta 0`; a ring that ever pops out of order is a crash-level assertion, not a counter |
| **no frame under another's name** | frames carry their own `frameIndex` (`03d840e`), and `target`/`shown`/`delta` are read off the delivered frame (§13). The `e76eabb` failure — returning true with a stale image — cannot recur because `convertCurrentFrame` returns bool and clears its output. | `delta 0`, `stale-blocked 0` |
| **exact landing on stop** | the playhead is set to the **last presented frame's own index**, then an exact Step request goes through `reclaimDecoder()` → `loadCurrentFrame`, the same choke point the drag release uses. The generation bump inside `reclaimDecoder()` makes every in-flight result stale by construction, so no shuttle frame can be painted after the landing. | `revplay.ps1 -StepCheck` (validated: `+1 moved 7.5%, -1 returned 0%`) |
| **no regression to audio state** | reverse must remain silent. J already calls `stopAudio()`; a reverse run must not write `userPlayIntent_`, and must go through **`beginPlaybackTimeline()`** (§29.3) rather than `startPlaybackRun()`, which would start audio. | `audio none` / `idle` in the HUD through a reverse run; `userPlayIntent_` unchanged across it |
| **no regression to forward playback** | the lease is not taken outside a reverse run; forward decode stays synchronous. | `worker posted 0` through a Space run; the forward baseline below |
| **no regression to scrubbing or stepping** | neither path changes. | `scrub.ps1 -SnapRelease` and `lifecycle.ps1` both gestures |

**Forward no-regression baseline, taken today on the same build and window** — 4K H.264,
Space, audio driving: `presented 23.79 / 24.000 (99.1% real time) | frames 120/120 |
handler 2.31 avg / 3.5 max | handler>budget 0 of 120 | rep 1 skip 0 | seeks 1 | cadence p50
41.6 / p95 42.8 / p99 43.1 | stalls 0 | hitch 0`.

### 8.1 Enumerate the entry points — the §29.2 rule, applied in advance

GATE E was validated on the Play action alone and every other path that started the timer
kept compiling silently. A reverse shuttle adds entry points into the same machinery, so
they are enumerated **now**, and each needs its own test:

1. J pressed (each press is a new speed — the timeline must be re-established every time).
2. A rewind control at 2x / 5x / 10x / 30x — the interface is deferred, but the engine
   entry point is the same one and must not assume the J ladder.
3. Reverse reaching frame 0 (the stop path, which also releases the lease).
4. Reverse → forward: L, or Space, pressed while reversing.
5. Reverse → scrub: slider pressed while reversing, and the release afterwards.
6. Reverse → step: an arrow key while reversing.
7. Reverse → pause: K.
8. Media switched, or the app quit, while reversing.

Items 4–8 are all lease-return paths and all of them must funnel through
`reclaimDecoder()`. Items 1 and 2 are `beginPlaybackTimeline()` paths.

---

## 9. Open questions for the owner

1. **Is 10x allowed to be approximately 10x on long-GOP?** The model says 9.3x on 4K
   H.264 in the walk regime, and the alternative — snapping at 10x — costs presentation
   rate (8 presents/s) and would read as choppy. Preference: land slightly under the
   number, or change the ladder?
2. **On stop, does "exact frame landing" mean the frame that was on screen?** In the snap
   regime the frames shown are keyframes, so the frame the shuttle stopped on is not
   generally the frame the arithmetic would name. The proposal lands on **the frame that
   was on screen**, which follows the standing "fidelity is owed to the frame the user
   stops on". Worth confirming rather than assuming.
3. **The subjective sign-off must be taken at the machine.** "Stable, intentional visual
   cadence" is a feel judgement by definition and Parsec re-times the screen. Nothing here
   substitutes for that.

---

## 10. Incidental findings

Two of these are live and neither belongs to this phase. They are recorded rather than
acted on.

1. **`reverseCacheCapacity` is computed in pre-GATE-C currency** (`w × h × 4`, the BGRA
   footprint) while entries have been planar since `e8566a4`. It reads 11 at 4K where the
   real depth is 32, and it silently clamps `TRACE_SEEK_CACHE_WINDOW` — which is why the
   first fill-window experiment appeared to do nothing (§6). Low severity today; it is the
   term the reverse fill window would have to be expressed against.
2. **The shipped seek-walk cache fill budget is 60ms, not the 240ms CLAUDE.md records.**
   The member initialiser is `240.0`, but `open()` overwrites it unconditionally with
   `envInt("TRACE_SCRUB_FILL_MS", 60)`, and `open()` runs on every media open. So §15.2's
   "60 → 240ms" change is **not in force in the shipping build**.
   **Measured before reporting, and the A/B says it does not matter on this gesture**: 4K
   H.264 backward drag, default vs `TRACE_SCRUB_FILL_MS=240` — `rev-hit 94.0% vs 94.2%`,
   `seeks 6 vs 6`, `ins 124 vs 124`, `hitch 4 vs 4`, `stalls 59 vs 58 of 115`. So this is a
   documentation-versus-code discrepancy, not a regression to chase; §15.2's recorded gain
   was not reproduced today and the note should be corrected rather than the default
   changed.
3. **`outside` — the per-cycle time that is not the handler — is 3.7–15ms and nobody has
   attributed it.** It is renderer-independent (a `TRACE_RENDERER=cpu` control at 1080p 4x
   reverse reads `outside 10.08` against d3d11's `9.92`), and the renderer's own `paint`
   and `draw` figures are 0.01–0.19ms, so it is neither the swapchain nor the blit.
   Candidates are the diagnostics HUD label repaint, the transport repaint, and event
   dispatch. It matters to this phase because it sets a ceiling on presentation rate that
   is independent of decode: **at ~9ms per present, presenting every source frame saturates
   near 110 f/s ≈ 4.5x at 24fps, whatever the decoder does.** It should be attributed
   before any target rate is promised — measuring it is cheap and it may be a measurement
   artefact that no shipping user pays.

---

## 11a. BUILT AND MEASURED (2026-08-10, same session)

Sections 1–10 are the measurement pass and the proposal. This section is what was
then built. Read it in preference to §7 where the two disagree — §7 is a design
and this is what the design turned into once it met the machine.

### 11a.1 The threading hypothesis is REFUTED

§5 predicted that long-GOP `FF_THREAD_SLICE` would collapse the ~30ms seek
intercept. It does — and the trade is catastrophic, so the answer is no.

Added `TRACE_LONGGOP_SLICE_THREADS=1` as scaffolding (off by default) and
re-ran the two-point solve on 4K H.264:

| | intercept | per walked frame |
|---|---|---|
| frame threading (shipped) | 30.4ms | **2.59ms** |
| slice only | **19.4ms** | 15.7ms |

**About 11ms of the intercept really is pipeline refill.** Removing it costs
**13ms on every walked frame**, which on a 30-frame GOP is +390ms. Reverse 1x
measures 91.9% → **73.5%** with a worst handler of **565.8ms**; forward 4K H.264
holds 98.9% but its handler goes 2.66 → **17.99ms**, which would not survive the
60fps budget. Frame threading is what makes the GOP walk cheap and it is bought
back many times over.

Recorded as a closed question. The knob stays as the control.

### 11a.2 The cache-pricing fix (`c55db40`)

`reverseCacheCapacity` and `entriesThatFit()` priced an entry at `w × h × 4`,
which has been the wrong currency since GATE C made entries planar. **Seeded at
open and then replaced by the size of the first full-resolution frame actually
stored** — observed rather than predicted, because a predicted size has to be
kept in agreement with the converter forever and that is what lapsed.

4K H.264 reverse 1x: **87.0/89.7% → 91.9%**, seeks 11/10 → **4**, hit 88.8 →
94.8%, long-gap spacing 13 → **30 = exactly the GOP**. The same result
previously required `TRACE_REVERSE_CACHE_MB=1024`; it reproduces at the shipped
384MB, which is what proves the entry count was the binding term and memory
never was.

### 11a.3 The shuttle (`e9fd236`)

Built as §7.1 + §7.2. §7.3's keyframe snap is **not** built — see 11a.5.

Reverse decode runs on the existing `ScrubDecodeWorker` under the existing lease.
Results are **queued**, not presented on arrival; the playback tick pops one per
slot. The stride is the commanded speed, and presentation stays at one frame per
source period at every speed.

**4K H.264 (GOP 30), % of the demanded speed:**

| speed | before | after | over budget | cadence p95 |
|---|---|---|---|---|
| 1x | 87.0% | **99.2%** | 11 of 114 → **0 of 113** | 118.9 → **43.1ms** |
| 2x | 75.7% | **100.1%** | → **0 of 54**, starve 0 | **43.0ms** |
| 5x | — | **95.0%** | 0 of 23 | 42.8ms |
| 10x | — | ~9.8x | 0 of 14 | 43.8ms |
| 30x | — | ~26x | 1 of 7 | — |

Worst reverse handler **132.6 → 6.3ms** at 1x, and long cadence gaps disappear
entirely (`long-gap min/med/max --`).

**ProRes is the clearest case, because it has no GOP and the stride is the whole
mechanism.** 4444 reverse 1x goes 99.7 → **100.0%** with the handler falling
**24.46 → 3.87ms**; **10x runs in full at 24 presents/s with `starve 0` and p99
42.9ms**, on the file that previously reached 33% of 4x.

### 11a.4 What was verified, and the two faults measurement found

Not regressed: forward 4K H.264 99.1%, `handler>budget 0 of 120`, worker
`posted 0`; forward 4444 99.7%, `0 of 198`, stalls 0, hitch 0; backward drag
`rev-hit 94.0%`, seeks 6, hitch 4, `delta 0`; `lifecycle -PlayThroughDrag` PASS
and `-PausedThroughDrag` PASS. Landing exactness with a control leg: **+1 moved
5.5%, −1 returned 0.1%**.

Every exit from a run is enumerated and tested rather than sampled
(`scripts/measure/revtransitions.ps1`): K, Space, L, stepping, slider press,
media switch, quit. All pass.

**Two faults the measurement caught, both worth keeping:**

**Reaching the head of the file ended the run but did not stop playback.**
`reverseRunActive_` went false while the mode stayed `PlayingReverse`, so the
next tick took the ordinary synchronous path *at the shuttle's speed* — period
41.71/30 = 1.39ms — and decoded on the UI thread as fast as it could. It was
visible only in the tail of a run, as `sched tick 1ms` on a run that had
otherwise presented perfectly.

**The pipeline was silently disabled by a guard that reads like the opposite of
what it means.** `isVideoScrubActive()` means "the media is a video file", not
"a drag is in progress", so `if (isVideoScrubActive()) return;` refused every
case the function exists to serve. Nothing looked wrong: the run behaved exactly
as before, and the only symptom was `posted 0` on the worker line.

### 11a.5 Open, in priority order

1. **The keyframe snap (§7.3) is not built, and 30x is where it shows.** 4K
   H.264 reaches ~26x because its GOP is 30 and a stride of 30 lands on
   keyframes by arithmetic accident. **1080p (GOP 48) reaches ~20x of 30x**, with
   `starve 6 of 17` — every sample falls mid-GOP and pays a walk. Snapping the
   sample to the nearest keyframe at or below the ideal target is the fix, and
   §5's numbers say it costs ~30ms instead of ~71ms there.
2. **Owner question:** at 30x on a file whose GOP does not divide the stride,
   is the exact speed at a lower presentation rate preferred, or a smoother
   picture at a lower speed? Snapping can hit 30x exactly at ~15 presents/s, or
   present at 24/s and land near 20x. This is a feel decision and it must be
   taken **at the machine**, not over Parsec.
3. **`outside` is still unattributed** (§10 item 3). It matters less now — the
   shuttle holds presentation at 24/s at every speed, so the ~10ms ceiling is no
   longer near — but it is still the term that would cap any future higher
   presentation rate.
4. **The rewind ladder changed `jogReverse` from 1/2/4 to 1/2/5/10/30.** That is
   an engine change driven by the interface spec, not interface work, but it does
   alter what J does today and the owner should know.

## 11b. Owner retest, the fast-forward fault, and the keyframe snap (2026-08-10)

### 11b.1 Owner decisions taken

- 4K H.264 reverse 1x **signed off** — feels smooth enough.
- ProRes reverse **feels good**.
- Reverse 30x: **accurate 30x with a stable ~15fps presentation**, not a smoother
  picture at a lower speed. §11a.5 item 2 is answered.
- **Stopping on the last visibly displayed frame is correct.** §9 item 2 answered;
  it is what was implemented.

### 11b.2 Accelerated fast-forward was broken, and it was the same fault

Reported by the owner across every format. Reproduced before theorising:

| forward | demanded | achieved | ceiling |
|---|---|---|---|
| ProRes 4444 | 2x | **1.00x** | 32 f/s |
| ProRes 4444 | 4x | 1.33x | 32 f/s |
| 4K H.264 | 4x | 3.97x | 95 f/s |

**One fault, shared, not format-specific — only the threshold varies.** The speed
lived in the tick rate and every present advanced exactly one frame
(`steps = 1` for video), so the demanded present rate was `speed × fps` and the
achieved speed was capped by per-frame decode cost. On 4444 two rungs of the
ladder were visually identical and neither was the number on the label. Separately
`jogForward` doubled and capped at 4x, so 5x/10x/30x were unreachable everywhere.

It is exactly the fault the reverse shuttle had already fixed. The fix was to
generalise that machinery rather than to patch the ladder: **the shuttle is now
direction-agnostic**, and above 1x forward the stride carries the speed while
presentation stays at one frame per source period. **At exactly 1x nothing
changes** — ordinary audio-mastered playback on the validated path, which the
shuttle never enters.

**Achieved forward speed after, all 16 cells measured:**

| | 2x | 5x | 10x | 30x |
|---|---|---|---|---|
| 1080p H.264 | **2.04x** | **5.02x** | **10.3x** | **28.1x** |
| 4K H.264 | **2.06x** | **4.86x** | **12.0x** | clip-limited |
| ProRes 422 HQ | **2.06x** | **5.27x** | **11.3x** | clip-limited |
| ProRes 4444 | **1.89x** | **5.17x** | **10.8x** | ~30x |

Cadence is p50 41.7 / p99 43.2ms almost everywhere, `handler>budget 0` on every
rung, `starve 0` on most. The 121–169 frame clips traverse in under 0.2s at 30x,
so only the 412-frame 1080p clip can measure that rung honestly.

The forward walk limit is raised 4 → 48 **for long-GOP only**: a forward stride
walks from where the decoder already is at ~0.9–2.6ms a frame against a ~30ms
seek. Intra-only keeps 4 and must, because a seek there lands on the target for
the price of one decode.

### 11b.3 The keyframe snap, built to the owner's decision

§7.3, now built and reverse-only. A mid-GOP target costs a seek plus a walk that
buys nothing when only one frame per GOP is shown; snapping removes the walk, and
the presentation period is scaled by `advance/stride` so the **content rate stays
exactly the commanded speed** and the presentation rate is what gives.

**1080p at 30x: `gop 48` learned exactly, cadence p50 66.3 / p99 68.2ms — 15.1
presentations a second at a steady 30.2x**, against ~20x with p95 166.8ms and 6
starves of 17 before. That is the owner's decision expressed as arithmetic:
48 frames per present ÷ 719 frames per second = 66.8ms.

**The grid is learned from keyframe POSITIONS, not from a statistic over them.**
A request for frame T that walked W frames landed on the keyframe at `T − W` —
exact. The first cut used `max(walk) + 1`, which converges **from below** and
stopped at 41 on a file whose GOP is 48, so every "snapped" target missed the
grid and still walked; the HUD read `SNAP gop 41` while nothing improved. **A
statistic over a quantity is not the quantity, and the positions were available
all along.** The grid is also *anchored* on an observed keyframe rather than
taken modulo the spacing, so a file whose first keyframe is not at frame 0 still
snaps onto real ones.

### 11b.4 Verified

Not regressed: forward 1x 4K H.264 99.1% and ProRes 4444 99.7%, both with
`handler>budget 0` and worker `posted 0`; backward drag `rev-hit 94.0%`, seeks 6,
hitch 4, `delta 0`; both lifecycle gestures PASS; **all six shuttle exit paths
PASS** (K, step, Space, L, scrub, quit).

### 11b.5 OWNER SIGN-OFF — THE SHUTTLE PHASE IS COMPLETE (2026-08-10)

Retest passed on the shipping build, at the machine:

- **fast-forward advances clearly through the complete ladder** on every format;
- **reverse 30× reads as intentional** at the approved ~15fps presentation cadence;
- **direction changes respond correctly**;
- **stopping lands on the last visibly displayed frame**;
- normal playback, audio return, scrubbing, exact release and stepping all remain good.

That closes the bounded reverse-shuttle phase and the fast-forward blocker with it.
Every goal in the phase brief is now met and verified: immediate response, stable
cadence, newest-target-wins, no UI-thread saturation, rapid direction changes,
appropriate sampling at 2×/5×/10×/30×, exact frame landing on stop, and no
regression to forward playback, scrubbing, stepping or audio state.

**The subjective half was taken at the machine, not over Parsec**, which is the
condition §1 attaches to any cadence judgement.

### 11b.6 Open

1. **30x is only measurable on the 412-frame clip.** Every other test file
   traverses in under 0.2s at that speed. A longer clip would make the 30x row
   trustworthy on all four formats.
2. **The UI fast-forward button must start at 2x**, not 1x. The engine takes any
   stride, so this is a call site rather than a change — but it is not built,
   because the interface pass is deferred.
3. **`outside` is still unattributed** (§10 item 3). It no longer binds: the
   shuttle holds presentation at 24/s (or 15/s snapped) at every speed.

## 11. What the next session does, in order

1. **Attribute `outside`** (§10 item 3). One instrumented run. It sets the presentation
   ceiling and therefore the whole speed budget.
2. **Test the threading hypothesis** (§5): long-GOP on `FF_THREAD_SLICE` during
   seek-dominated reverse. It predicts the ~30ms intercept collapses. It moves the handover
   point and rescues 10x, and it must be measured before the architecture is sized around
   either answer. Forward playback throughput is the control and the thing at risk.
3. **Widen the reverse fill window to the GOP** at 384MB, once `reverseCacheCapacity` is
   expressed in real stored bytes. Measured effect at 1024MB: `87.0 → 93.4%`, `seeks 11 →
   3`, `handler avg 8.30 → 4.18ms`, at the cost of `max 103.8 → 126.1ms`.
4. **Then** Component A, then B, then C, each behind its own control knob and each with the
   entry-point list of §8.1 exercised rather than the one the harness happens to drive.

**And the standing rule, because this document is itself a set of deferred conclusions: a
deferred item's premise expires. Re-derive §5's cost model and §7.3's handover arithmetic
against a fresh measurement before building on them.** Every number here is from a single
run on 2026-08-10, on the physical panel, at `win 1280x829`.
