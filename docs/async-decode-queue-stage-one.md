# Checkpoint 2, stage one — built, measured, and the report before stage two

Built 2026-08-14, `d8beba8`. Design: `docs/async-decode-queue-design.md`.
Physical panel 5120x1440 @ 239.999Hz, `d3d11`, `win 1066x1083`, `display 1066x600`.
8K plate: `12_8K_ProRes4444\Foces_8K_Lut_Dino Stomp_plate_4444XQ.mov`, 7680x4320
ProRes 4444 XQ, 23.976 fps, 145 frames.

**`TRACE_PLAYBACK_QUEUE=N`, default 0 = off.** Nothing ships enabled. The
synchronous path is not a second implementation kept in sync — it is the path
that runs when the queue cannot answer, so it stays exercised.

---

## The headline

| build | queue off | depth 1 | depth 2 | depth 3 |
|---|---|---|---|---|
| vcpkg | **53.6%** (12.86 fps) | 48.3% (11.58) | **58.2%** (13.96) | 56.0%, clamps to 2 |
| minimal GCC FFmpeg | **56.5%** (13.55 fps) | — | **62.0%** (14.87) | — |

**+8.6% relative on vcpkg, +9.7% on the minimal build. The design scoped +22%.**

Everything else the design asked for holds: `drop 0`, `reseed 0`, `wait 0.01ms`,
`starve` falling with depth, memory bounded and reported, and the default
provably inert (`pq OFF 0/0 … posted 0` beside an otherwise unchanged HUD).

---

## The overlap is real, and it is checked the way the design demanded

The design forbids inferring overlap from the frame rate: *"the check that
overlap is real is `handler` collapsing to upload+paint while `dec`+`sws` stay
where they are, with `outside` rising to absorb the difference. If `handler`
falls and `dec` falls with it, the run decoded fewer frames rather than
overlapping them."*

Minimal build, queue off → depth 2:

| | off | depth 2 |
|---|---|---|
| `handler` | **70.42** | **16.28** |
| `outside` | 3.13 | **28.32** |
| `dec` | 38.55 | 41.08 |
| `sws` | 18.36 | 22.74 |
| `upload` | 12.61 | 24.12 |

`handler` collapsed, `dec` did not fall with it, `outside` rose to absorb the
difference. **Overlap confirmed on the design's own terms, not on the frame
rate.**

---

## Why it is +10% and not +22% — and this is the stage-two decision

**Both overlapped stages get slower when run concurrently.** `sws` +24%,
`upload` +91%. Nothing is idle any more, and a 199 MB frame moving through
conversion on one thread while another is uploading a different one contends for
memory bandwidth.

The binding term is the **worker's own serial cost, `dec + sws`**, because
conversion rides with decode — the premise correction that scoped this work.
Minimal build: `41.08 + 22.74 = 63.8ms = 15.7 fps` predicted against **14.87
measured**. The model is right; what was optimistic was assuming the two halves
cost the same overlapped as they did alone.

**The design flagged contention as a stage-two risk against a ~2 ms per frame
margin. It is already material at ONE stage.** That is the single most useful
thing stage one produced, and it is exactly what the owner funded it to find
out.

What stage two would have to do is split conversion off the worker onto a third
thread, taking the worker's stage to `dec` alone. On the minimal build that is
`41.08ms = 24.3 fps` — nominally at the 23.976 target with **no margin at all**,
before the third thread's own contention is counted, and stage one has just
measured that contention at +24% on `sws` and +91% on `upload`. On the numbers
in hand, **stage two lands short of 24 fps rather than at it.**

## Depth is justified by the sweep, not chosen

The design required this. **Depth 1 is worse than off** — 48.3% against 53.6%,
`starve 141` — because a depth-1 queue cannot overlap: the worker only starts
frame N+1 after the tick has consumed N, so a full cross-thread round trip sits
in series with every frame. **Depth 2 is the minimum that overlaps.** Depth 3
clamps to 2 on this file from the 512 MB budget against a ~199 MB entry, which is
the byte bound doing its job rather than a coincidence.

`starve` falls 141 → 88 across the useful range and never reaches 0, which is
correct and is the honest statement of the deficit: the pipeline still cannot
supply 24 fps, so most slots have nothing new.

## Two faults found by measuring, both recorded in the commit

**The playhead advanced on a starve, and that is a runaway.** The target ran
ahead at 24 fps while the pipeline supplied 20, so every frame that arrived was
already behind and was discarded on arrival: `posted 94, drop 93, starve 146,
reseed 50`, **one frame presented in 6.14 s — 0.7% of real time against the
synchronous path's 53.6%.** A starve is not a failure and cannot be treated like
one; it now leaves the playhead exactly where it was, which is what an audio hold
already does.

**`wait` was measuring the upload and read 52.01 ms on a run where nothing had
waited.** `setFrame()` is where the D3D11 upload happens — 24.58 ms on an 8K
plate — and timing to the end of the function folded it in under a name meaning
"the pipeline blocked me". Timed to the queue decision only, it reads **0.01 ms**.
Ninth instrument in this project to report the wrong thing confidently, and the
second in two sessions where the wrong answer was the alarming one rather than
the flattering one.

## Regression — flat at the default

4K H.264 cadence x3 **99.9/100.0/100.0%**, 120 frames, `0 of 119`, all gaps ~1x,
`drop 0`, `rephase 0` · ProRes 4444 x2 **99.8%**, 261 frames, `0 of 260` ·
`scrub -SnapRelease` `target 120 shown 120 delta 0` full-res planar, `hitch 0`,
`land 0` · both lifecycle legs (81.8% and the **0% control**) · **25 of 25
transitions**.

**With the queue ON at depth 2 a file that meets budget is unharmed**: 4K H.264
reads 100.0% x2 with `0 of 119`, and `handler 1.76 → 0.86ms` because its decode
moved off the UI thread. So the mechanism is not 8K-specific; it is simply worth
nothing where there was already headroom.

---

## What the owner is being asked

Stage two was funded on the expectation that the 8K plate is reachable close to
real time. **Stage one's measurement says the margin it depends on is not
there.** The options:

1. **Stop after stage one.** Ship the queue at depth 2 (default-on is its own
   decision), take the ~10%, and accept that the 8K plate stays around 62%.
2. **Build stage two anyway**, knowing it is predicted to land near but under 24
   fps rather than at it, and that the prediction now has measured contention in
   it rather than an assumption.
3. **Attack decode instead.** `dec` is the binding term in every arrangement and
   is the one thing neither stage touches. `TRACE_DECODE_THREADS` is already
   known to be worth +21% on this plate and is currently left at FFmpeg's
   automatic count, which caps at 16 on a 32-thread box.

**Option 3 is not in the funded plan and is the one the numbers point at.** It
is also the cheapest of the three. Recorded as a recommendation, not taken.
