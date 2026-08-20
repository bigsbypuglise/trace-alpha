# MP4 scrub is poor on a higher-core machine — investigation brief

Owner report, 2026-08-19. Scrubbing MP4 files is badly degraded on a second machine, seen
before and dismissed as a one-off. **ProRes is not affected. Only MP4.** Top priority.

> **STATUS, 2026-08-19 (final): DIAGNOSED AND FIXED. Both early hypotheses are refuted and
> the cause is the scrub chain's PAINT, throttled by the present.**
>
> - **Threading: refuted.** `TRACE_DECODE_THREADS=8` and `TRACE_LONGGOP_SLICE_THREADS=1`
>   were both tried on the Threadripper and neither helped. The five-minute test section
>   below is retained as the record of what was ruled out. The fault also reproduces on
>   `3_1080p_H264_MP4\M&M_TopGun_1080.mp4`, the validated pool file — the machine, not the
>   media.
> - **The worker round trip: refuted by the Threadripper's own selftest paste.** Overhead
>   read 0.22–1.43ms, 6.6–8.4% of the round trip — nothing like a cost that could pin a
>   drag.
> - **The finding: the scrub loop was capped at one frame per display refresh.** Three legs
>   of wildly different demand (pointer 58–307 f/s) all presented 57.8–59.6 f/s, paint gap
>   16.4–17.0ms against that monitor's 59.98Hz, while decode read 0.79–0.93ms — a frame
>   costs under a millisecond to decode and took a full refresh to appear. Leg 3 is the
>   cleanest evidence: a slow 3x drag at supply 99.3% still read p2p 416ms. The chain was
>   request → decode → deliver → **paint** → next request, and on that machine the paint's
>   `Present` blocks until the display consumes a frame.
> - **The refresh rate alone is NOT the mechanism — measured here, not assumed.** The dev
>   box's own panel set to a true 5120x1440 @ 60Hz runs the identical selftest byte-for-byte
>   at its 240Hz figures: interval-0 flip presents are last-one-wins on this driver/DWM and
>   never block. The throttle is a **driver/composition property of the machine** (the
>   forced-vsync / composed-presentation class), which the Threadripper has and this box
>   does not. `TRACE_PRESENT_SYNC=1` is the in-binary model of that class, and under it the
>   dev box reproduces the fault emphatically (see the fix section at the end).
> - **The fix shipped: the scrub paint gate** — decode decoupled from presentation during a
>   drag, painting rationed to the display refresh period or half the thread's wall time,
>   whichever is coarser, always with the newest delivered frame. Every frame is still
>   decoded, delivered, counted and cached in order; exact release and the landing are
>   untouched. `TRACE_SCRUB_PAINT_GATE=0` is the rollback.

| | dev box (all recorded figures) | the machine showing the fault |
|---|---|---|
| CPU | AMD Ryzen 9 5900XT — 16 cores / **32 logical** | AMD Ryzen Threadripper 3970X — 32 cores / **64 logical** |
| GPU | RTX 4090 | RTX 4090 |
| RAM | — | 128 GB |
| OS | Windows 11 | Windows 11 Enterprise 25H2 |

**The more powerful machine is the worse one.** That is the shape of the answer, not a paradox.

---

## The leading hypothesis, with the mechanism already documented in our own code

`VideoDecoderFFmpeg.cpp:1006` sets the decoder's thread count, and the discriminator is
`intraOnly`:

```cpp
const int cpuThreads = std::min(av_cpu_count(), 64);
impl_->codec->thread_count =
    envInt("TRACE_DECODE_THREADS", intraOnly ? cpuThreads : 0);
```

Long-GOP — **every MP4** — gets `0`, meaning FFmpeg's automatic count, together with
`FF_THREAD_FRAME | FF_THREAD_SLICE`. The comment directly above it states the consequence:

> under frame threading `thread_count` is how many frames are **IN FLIGHT**, and a deeper
> pipeline costs a longer refill after every seek and flush — which is the exact mechanism
> that makes frame threading unusable for random access

And `VideoDecoderFFmpeg.cpp:2295` puts a number on it:

> Frame threading must refill ~`thread_count` packets after every flush: on the 8K plate a
> frame-threaded seek measures **116ms against a slice-threaded ~30ms**.

**FFmpeg's automatic count is derived from the machine's core count.** So on a 64-logical-core
Threadripper the long-GOP pipeline is roughly twice as deep as on the 32-core dev box — and
**scrubbing is nothing but seek, flush, walk, repeated.** Every random-access request pays a
refill proportional to that depth.

That explains all three observations at once:

- **MP4 only.** ProRes is intra-only, so it is slice-threaded — there is no frame pipeline and
  therefore no refill. `thread_count` there means threads sharing one frame, which is simply
  more of the machine and is why the raised count *helps* random access on ProRes (measured:
  4444 `-SnapRelease` shuttle **29.63 → 15.89 ms/frame**, `hitch` **2 → 0**).
- **Machine specific.** The depth is a function of `av_cpu_count()`.
- **Worse on the faster machine.** More cores means a deeper pipeline means a longer refill.
  The Threadripper 3970X is also Zen 2 with more chiplets than the dev box's CPU, so
  cross-core synchronisation — which is what a pipeline refill is — is more expensive per
  thread as well as being needed more often.

## THE PREMISE THAT EXPIRED IS HARDWARE, NOT TIME — a new variant of this project's pattern

`TRACE_LONGGOP_SLICE_THREADS=1` exists precisely to force slice-only threading on long-GOP,
and it is recorded as **"measured, refuted, knob retained as the control."** That refutation is
real — but it was measured **on the 32-thread dev box only**, and the cost it measured is a
function of core count. **Hardware-specific evidence has been carried as though it were
universal.** Re-derive it on this machine rather than citing it; that is the whole reason the
knob was kept.

---

## What Anj can test in five minutes, no rebuild

Both knobs already ship. On the Threadripper, open an MP4 and scrub hard, then relaunch with
each of these and scrub the same file the same way:

```
:: 1. cap the frame pipeline depth
set TRACE_DECODE_THREADS=8
"...\build\app\Release\Trace.exe"

:: 2. remove the refill entirely - slice-only threading for long-GOP
set TRACE_LONGGOP_SLICE_THREADS=1
"...\build\app\Release\Trace.exe"
```

(Or edit the `Run Trace (d3d11).bat` launcher, which already sets an environment variable, and
add either line above the `start`.)

**If either makes MP4 scrubbing feel normal, the hypothesis is confirmed and the fix is
small.** Press `H` for the HUD and quote `thr`, which prints the applied mode and count
(`thr frame x16` / `thr slice x64`) — that says what actually took effect rather than what was
asked for.

## What the session should measure

1. **Characterise before changing anything.** `scrub.ps1` with `-Env TRACE_TRANSPORT_BAR=1`
   plus a reversal drag on the same MP4, on both machines if possible. Quote `hitch`,
   `win WxH`, `display`, `rev-hit %`, `seeks`, `ra-walk`, `walk max` and the new `thr` field.
   **The dev box is the control** — the same file, the same gesture, the same window size.
2. **Sweep `TRACE_DECODE_THREADS`** on the Threadripper — 4, 8, 16, and the default — and
   report scrub cost against each. The forward-playback cost of a lower count is the other half
   of the trade and must be measured, not assumed.
3. **Run `TRACE_LONGGOP_SLICE_THREADS=1`** and measure both scrub *and* forward playback. The
   original refutation was about playback throughput; establish what it costs *here*.
4. **Check ProRes is genuinely unaffected on this machine**, rather than assuming it from the
   report. If ProRes is also degraded, the hypothesis is wrong and the cause is elsewhere.

## The likely fix, if confirmed

Not "cap the threads globally". The honest shape is that **long-GOP `thread_count` should be
bounded rather than left to FFmpeg's automatic**, because the automatic scales with core count
while the *cost* of that scaling is paid on every random access — and Trace is a scrubbing
tool. Forward playback on H.264 has enormous headroom (4K H.264 reads 100% of real time with
`dec` under 1ms), so trading a little playback throughput for a much cheaper seek is very
likely the right side of that trade on this codec family.

Whatever is chosen must be **derived from the machine, never hard-coded** — a literal that
works on a 64-thread box breaks a four-core one, which is exactly why the intra-only side uses
`av_cpu_count()` rather than a constant.

## Other candidates, ranked — all weakened by the MP4-only symptom

- **I/O.** ProRes bitrates are far higher than MP4, so a storage problem would hit ProRes
  first. Still worth reading the HUD's `io` and `src` lines once to rule it out cheaply.
- **Window size / cache depth.** A different monitor changes the video rect, and cache depth
  follows it (§22.8) — but this would affect ProRes too, and the reverse cache is byte-budgeted
  so its behaviour is machine-independent. Quote `display` anyway; it is free.
- **The scrub batch.** `TRACE_SCRUB_BATCH=0` is the in-binary control. Not machine-dependent in
  principle, but it is one run to eliminate.
- **GPU.** Both machines are RTX 4090. Ruled out.

**Do not start with these.** The threading hypothesis explains all three observations with a
mechanism already documented and measured in this codebase; the others explain at most one.

---

# The scrub self-diagnostic: `--scrub-selftest` (built 2026-08-19)

The third selftest, following `--renderer-selftest` and `--window-shape-selftest`. Unlike
those two it needs the full application: it opens the clip, **drives the shipping scrub path
itself** with scripted drags through the real timeline slider (the same
setValue/setSliderDown route the floating transport has used since phase 6 — coalescing
timer, press-jump, batch chain, worker lease and landing all run as shipped), and prints one
pasteable block. Runnable by anyone: one invocation, no interaction, no arguments beyond the
clip. **Not a CI step** — it needs a real clip and a real desktop.

## The one command for the Threadripper

From a Command Prompt in the folder holding the Trace build (any CI ZIP at or after this
commit):

```
Trace.exe "--scrub-selftest=C:\path\to\M&M_TopGun_1080.mp4"
```

The window appears, scrubs itself for ~20 seconds, and exits. The block is written to
**`trace-scrub-report.txt` beside `Trace.exe`** (falls back to `%TEMP%`), as well as stdout —
the file exists precisely so nobody has to fight GUI-subsystem shell semantics (the
pwsh-vs-5.1 wait trap) to capture it. Send the file back; that one paste answers the
question. Exit 0 = report produced; 6 = clip missing/unopenable; 7 = a leg failed
structurally (nothing presented, landing timed out, or release landed off target). **Slow
numbers are the report, never a failure.**

Four legs, each against a fresh reopen of the clip (cold cache, per-leg counters — the same
reason the harnesses restart Trace per run), window forced to 1280x760 logical so both
machines measure at one size (window size drives cache depth, §22.8):

1. **forward sweep** — the whole clip in 1.5s, snap release (`scrub.ps1`'s default gesture);
2. **reversal drag** — `scrub.ps1 -Reversals`, segment for segment;
3. **slow forward drag** — ~3x speed for 2s. No recorded counterpart; it exists because a
   fixed per-request cost shows cleanest at low demand, where `p2p` is nearly pure latency.
4. **rapid back-and-forth** — ten 16%-of-timeline throws at 150ms each, ~6.7 direction
   changes a second. Owner instruction (2026-08-19): the `-Reversals` shape is milder than
   real use, and a fix's validation gesture must be at least as demanding as the real one —
   a milder harness gesture is how a fix passes the test and still feels broken, which this
   project has recorded twice.

The gesture is **time-based, never step-counted**, so a slow machine gets the same hand
motion and shows its deficit as lag rather than slowing the hand down to hide it.

## How to read the block against the hypotheses

- **`round trip/request` and its split** (`wake` / `deliver` / `overhead`) is the headline.
  `overhead` is the round trip minus the batch's own decode — the term that scales with the
  machine rather than the media. Dev box, shipping config: **0.02ms avg**. If the
  Threadripper reads milliseconds here, the round-trip hypothesis is confirmed and the fix
  is transport, not decode. The same split is now permanently on the HUD's `worker` line
  (`rt … wake … deliv … ovh …`).
- **`decode/frame`** beside it says whether the decoder itself is slow there. If decode is
  high and overhead is low, it is a decode problem after all (and the threading knobs that
  were tried should have moved it — re-check they actually applied via the `thr` field).
- **`thr frame xN` and `av_cpu_count`** — what threading actually took effect, beside the
  machine's own count, logical/physical cores and processor groups.
- **`batch cap 4 achieved-avg … budget-cut N`** — whether the 8ms walk budget is cutting
  batches short on that machine (the budget-cut counter is new; the two ways a batch loop
  ends looked identical before it).
- **`seeks` / `ra-walk` / `walk max` / `rev-hit` / `cache`** — the GOP-walk and cache terms,
  so a cache-class fault (window/display driven, §22.8) is visible; `win`/`display` are
  printed per leg for the same reason.
- **`behind` / `p2p` / `hitch`** — the subjective anchor. Note `p2p max` under the reversal
  leg is the metric's known artefact class (a gesture that crosses the same frames twice
  charges a frame with drag history); quote `p2p end`, `behind` and `hitch` from that leg.
- **`painted N gated N | paint cost X ms gate Y ms`** on the hitch line (added with the
  fix) — how many deliveries reached the screen, how many the gate declined at once, what
  one paint observably costs on that machine, and the gate period those two produced.
  `paint cost` near the refresh period **is** the throttled-present class; near 0.3ms is a
  healthy present. `gated` non-zero is the gate working, not frames lost.
- **`hud SHOWN|hidden`** on the knobs line — see below; the two configs measure different
  transports and a paste is only comparable to a paste in the same config.

## Dev-box reference (2026-08-19, Ryzen 9 5900XT 16c/32t, build at this commit)

Display was **1920x1080 @ 60Hz (remote-session class)** — `hitch` is the comparable figure.
Shipping config (HUD hidden, no TRACE_* set), `M&M_TopGun_1080.mp4`:

- leg 1 (forward sweep): ptr 156.9 f/s | supply 100.3% | behind 0/1f | p2p 1.6/10.1ms |
  **rt 0.58/0.9ms, wake 0.01, deliver 0.01, overhead 0.02/0.2 (4.2% of rt)** |
  decode/frame 0.56/0.8ms | seeks 0 | hitch 0 | delta 0
- leg 2 (reversals): supply 110.7% | behind 0/46f | **rt 1.34/77.5ms, overhead 0.03/0.3
  (2.5%)** | seeks 10 | ra-walk 23.6 | walk max 29f | rev-hit 97.8% | hitch 9 | delta 0
- leg 3 (slow drag): **p2p 0.6/1.9ms** | rt 0.73ms avg, overhead 0.03 | hitch 0 | delta 0

With `TRACE_HUD=1` (the harness-comparable config): leg 1 reads **rt 3.08/4.1ms, deliver
2.50/3.6, overhead 81.5% of rt** — the HUD's own ~25-line rebuild per present is ~2.5ms of
delivery latency on this box. That is instrument cost, not shipping behaviour, and it is why
the selftest respects `TRACE_HUD` and prints which config ran.

**Validated against the real gesture, same box, same build, same day** — `scrub.ps1` with a
real mouse (bar mode, widened to 1280, HUD on) against the selftest at `TRACE_HUD=1`:
forward sweep rt 3.97 vs 3.08ms avg, deliver 2.67 vs 2.50, ptr 154.7 vs 157.2 f/s, supply
99 vs 99.8%, hitch 0 both, delta 0 both; reversal drag rev-hit 97.3 vs 97.8%, seeks 11 vs 9,
ra-walk 18.8 vs 20.6, walk max 29 both, hitch 8 vs 8, delta 0 both. And on the 4K H.264 pool
file the reversal leg reads **rev-hit 98.8% | seeks 3 | hitch 1 | delta 0** — that file's
recorded class. The instrument measures what the gesture measures.

## The part-2 question, resolved by the paste (2026-08-19)

The three dev-box-tuned constants — the **batch cap of 4**, the **8ms walk budget**, the
**60ms seek-walk fill budget** — were the honest suspects, and the Threadripper's paste
**exonerated all three**: batches, budgets and fills read normal there; the term that was
wrong was the paint. The one piece of adaptivity actually built is therefore the paint
gate's cost term, and it honours the safety property this section demanded: **on the home
box it reproduces the tuned behaviour** — paint cost reads 0.28ms, the cost term never
binds, the gate sits at the pure refresh period, and every recorded scrub figure is
reproduced (measured, table below). Do not adapt the three constants; nothing in either
machine's paste says they are wrong anywhere.

---

# The diagnosis and the fix (2026-08-19): the scrub paint gate

## What the Threadripper paste said

Overhead 0.22–1.43ms (6.6–8.4% of round trip) — the round-trip hypothesis dead. Presented
throughput pinned at **57.8–59.6 f/s in all three legs** against pointer demand of 58–307
f/s; paint gap **16.4–17.0ms** against the monitor's 59.98Hz (16.67ms); decode/frame
0.79–0.93ms. Leg 2: supply 19.4%, 205 frames behind at peak, p2p max 2767ms. Leg 3 — the
cleanest evidence, because nothing was being asked of it — supply 99.3% at a 3x drag and
still paint gap 17.0ms, presented 57.8 f/s, p2p 416ms. A frame costs under a millisecond to
decode and takes a full refresh to appear.

It also explains MP4-versus-ProRes with nothing MP4-specific: 4K ProRes decodes at
15–23ms/frame, at or below a 60Hz refresh already, so the cap never binds; 1080p H.264
decodes at 0.9ms and is entirely paint-bound.

## The mechanism, and the half of it the dev box corrected

The chain was `request → decode → deliver → paint → next request`, with the paint a
synchronous `repaint()` per delivered frame — deliberate, with the recorded reason that
"update() coalesces, so a walk loop would decode every frame and display only the last."
Each paint ends in `Present(0, 0)` on the flip-model swapchain, and on the Threadripper
that present **blocks until the display consumes a frame**.

**But the block is not a property of the refresh rate — it is a property of the machine's
presentation path, and that was measured rather than assumed.** The dev box's own panel at
a true 5120x1440 @ 60Hz (`scripts/measure/setrefresh.ps1`, mode restored after) runs the
identical selftest **byte-for-byte at its 240Hz figures** — presented 157/353/71/245 f/s
across the legs, paint gaps 2.8–13.9ms avg. On this driver/DWM, interval-0 flip presents
are last-one-wins and never block, at any refresh. The Threadripper is in the class where
they do (forced vsync in the driver, or a composed-presentation mode — the exact cause on
that box is unattributed and does not need attributing; the fix is robust to the class).

**`TRACE_PRESENT_SYNC=1` is the in-binary model of that class**: it presents at sync
interval 1, so every present waits for the display. A diagnostic, never a configuration.
Under it, the dev box at 60Hz reproduces the fault emphatically — in fact more harshly than
the Threadripper (paints block ~250ms here, ~16.7ms there; the class is the same, the
severity is machine-specific).

## The fix: decode decoupled from presentation during a drag

Every delivered frame still updates the playhead, the lag model, the counters and the cache,
in order — nothing is sampled, nothing is skipped, and this is not §15's stride. What
changed is that **the screen is painted at most once per gate period, always with the newest
delivered frame**, and `paintScrubFrameNow()` is the one place a chain frame reaches the
screen. A frame arriving inside the gate window is marked pending and a single-shot paints
it at the boundary, so the trailing frame always lands. The landing, the release, the
reverse shuttle (already paced by the tick), playback and the synchronous walk
(`TRACE_ASYNC_SCRUB=0`) are untouched.

**The gate period is `max(refresh period, 2 x observed paint cost)`, and the second term is
what makes it work.** A gate pegged to the nominal refresh alone was built first and
measured useless on the fault model — `gated 0`, nothing improved — because when a paint
BLOCKS, it costs at least the gate period, so the gate is always open and the thread is
100% paint. Bounding paints to half the thread's wall time keeps the chain fed however
expensive the present is. The cost term is observed (instant attack, slow decay, reset per
media), so on healthy machines it reads ~0.3ms against a 4.17–16.7ms refresh and **never
binds** — the shipped boxes stay on the pure refresh gate, which is itself visually free:
painting faster than the refresh cannot show anything when presents are last-one-wins.

**Prior decisions this touches, cited as history.** Paint pacing was prototyped and
rejected twice (Aug 2026, `5daa5ce` and the `TRACE_SCRUB_PACE` knob), both times on the
240Hz panel, on the measured argument that 98% of paints landed inside one refresh and
pacing bought ~200ms of paint cost across a whole run — nothing. At a throttled present
those paints are not wasted, they are the bottleneck. That is the third hardware-specific
premise this investigation overturned, after the threading refutation and the
constants-tuned-on-one-box audit. The `repaint()`-not-`update()` rule survives inside
`paintScrubFrameNow()` — when the gate decides to paint, the frame must be on screen when
the call returns; what changed is how often it decides.

## Validation (dev box, 2026-08-19, physical panel; fault model = 60Hz + TRACE_PRESENT_SYNC=1)

| leg (M&M_TopGun_1080) | fault, fix OFF | fault, refresh-only gate | fault, shipped gate |
|---|---|---|---|
| 1 forward sweep: dec f/s / supply | 3.8 / 3.7% | 3.9 / 2.9% | **157.4 / 100.2%** |
| 2 reversals: dec f/s / supply | 4.3 / 2.8% | 8.0 / 4.8% | **313.8 / 100.8%** |
| 3 slow drag: p2p end | 3292ms | 2777ms | **0.4ms** |
| 4 back-and-forth: dec f/s / supply | 4.3 / 8.0% | 5.9 / 11.7% | **201.3 / 115.1%** |
| release delta | 0 | 0 | 0 |

Under the shipped gate the paint cost EMA reads 82–152ms on the fault model and the gate
rations painting to half the thread; on the Threadripper's real ~16.7ms class the same
arithmetic gives ~30 painted pictures a second with the chain at full decode speed.

**And the healthy path is flat.** 240Hz default config, gate on versus the same-day pre-fix
baseline: all four legs identical within run variance (leg 1 supply 100.2 vs 100.1%, behind
0/1f both; leg 2 rev-hit 97.8% both, walk max 29 both, hitch 9 vs 8; delta 0 everywhere),
paint cost 0.28ms, gate 4.17ms. Real-mouse `scrub.ps1` forward and `-Reversals` match the
pre-fix controls figure for figure (forward: delta 0, hitch 0, behind 1/6f; reversals:
rev-hit 97.3%, seeks 12, walk max 29f, hitch 8, delta 0). Lifecycle **87.2% moving / 0%
control**; 4K H.264 cadence x2 **99.2/99.2%**, `drop 0`, `rephase 0`, identical buckets,
`handler>budget 0 of 120`; **25 of 25 transitions**; renderer selftest
`d3d11 fellback=0 planar=1`.

A caveat stated rather than hidden: the 60Hz-throttled *feel* cannot be judged from here,
and the recorded remote-session scrub anomalies (hitch 8–9 on this file at 60Hz-class
remote displays) are decode-walk hitches, not present blocks — yesterday's remote 60Hz runs
showed paint gaps of 2.8ms, so Parsec-class virtual displays are NOT in the throttled
class on this box, and the re-encoding attribution for their feel stands.

## What to do on the Threadripper

Run the same one command on a build at or after this commit:

```
Trace.exe "--scrub-selftest=C:\path\to\M&M_TopGun_1080.mp4"
```

Expected if the fix lands there: supply back near 100% on every leg, `paint cost` reading
~16.7ms with `gate` ~33ms and `gated` large, p2p end in single-digit milliseconds, delta 0.
Then scrub by hand — the feel is the owner's call, and `TRACE_SCRUB_PAINT_GATE=0` is the
one-variable rollback if anything reads worse.

---

# The beta.3 residues, reproduced and triaged (2026-08-19, second session)

The gate shipped and the Threadripper's beta.3 paste confirmed it (supply 100–124% on every
leg). Three residues remained in that paste, and the owner still reports the feel differing
from the 240Hz dev box. **The display physics half of that gap is not chased** — at 60Hz the
picture updates at most 60 times a second against 240 here, and no software touches it. The
three numeric residues were each reproduced on the dev box before anything was proposed.

**Method.** Dev box at HEAD (`19b7e88`, the beta.3 build of 2026-08-19 16:33), physical panel
switched to a true 5120x1440 @ 60Hz with `setrefresh.ps1` (restored to 240 after), selftest on
`M&M_TopGun_1080.mp4`. Four runs: the fault model (60Hz + `TRACE_PRESENT_SYNC=1`) **twice
back-to-back**, then two 240Hz controls — `TRACE_PRESENT_SYNC=1` and the bare default. All
shipping config (HUD hidden, gate on), all exit 0, `delta 0` on every leg of every run.

## Residue 1 — leg 1's elevated paint cost: REAL, DETERMINISTIC, AND ATTRIBUTED. It is the
## gate pacing paints at exactly the refresh period under sustained above-refresh demand.

| leg 1 (forward sweep) | paint cost | gate | paint gap avg/max | painted | hitch | supply |
|---|---|---|---|---|---|---|
| 60Hz + sync, run 1 | **16.53ms** | 33.06 | 17.7 / 33.7 | 86 | 2 | 97.5% |
| 60Hz + sync, run 2 | **16.54ms** | 33.09 | 17.6 / 33.6 | 85 | 2 | 97.7% |
| 240Hz + sync | 0.34ms | 4.17 | 6.7 / 12.1 | 224 | 0 | 99.6% |
| 240Hz default | 0.38ms | 4.17 | 6.6 / 12.4 | 226 | 0 | 100.0% |
| Threadripper beta.3 | 4.76ms | — | 23.0 / — | — | 8 | ~100% |

**Not cold-start**: two back-to-back launches read 16.53 and 16.54 — identical to the digit,
which no cache- or driver-warm-up effect produces. **Not "first leg after launch" either**:
the same knob at 240Hz reads 0.34ms on leg 1 of a fresh launch. The discriminator is that
**leg 1 is the only leg whose delivery is continuous at above-refresh rate for its whole
duration** (pointer 156 f/s against a 60Hz drain; legs 2 and 4 demand more but in bursts
broken by seeks and throw boundaries — their measured paint gaps average 17.9–19.1ms, above
the refresh period — and leg 3's 59 f/s sits just under it).

The mechanism: under sustained above-refresh demand the gate opens every `refresh period`
exactly, so paints are submitted at precisely the display's drain rate. In the
blocked-present class the flip queue therefore never drains, and **every present blocks for
close to a full period — the cost EMA is measuring the display, not the machine, which is why
it reads the 16.67ms refresh period to within 0.14ms and reproduces exactly.** The cost term
then doubles the gate, presents get cheap, the EMA decays, the gate returns to the refresh
period, and the queue refills — an oscillation that equilibrates just over the boundary
(paint gap avg 17.7 against 16.67 here; 23.0 on the Threadripper's milder block class, i.e.
**~43 painted pictures a second on a display that could show 60**). That deficit is leg 1's
`hitch 2` here and very likely the Threadripper's leg-1 `hitch 8`, and it lands on exactly
the gesture a fast review scrub is — the first residue is the one that matters.

**Proposed fix: the gate period should sit strictly above the refresh period —
`refresh × 1.05` — so sustained painting submits below the drain rate and the queue never
fills.** Derived from the machine (the refresh is read at runtime), no new constant class. On
healthy last-one-wins boxes it is invisible: painting at 95% of the refresh rate misses at
most one refresh in twenty, and painting faster than refresh already shows nothing — the
shipped design's own argument. **BUILT AND VALIDATED the same day — see "The gate margin"
section below**, including the honest half: the fault model keeps a small margin-independent
residual that is attributed to the model's own swapchain shape, not to the margin.

## Residue 2 — leg 2's p2p max ~1.6s: NOT A RESIDUE. It reads the same on the fully healthy
## dev box and is the metric's recorded artefact class.

| leg 2 (reversals) | p2p max | p2p end | supply | seeks | walk max | dec max | hitch |
|---|---|---|---|---|---|---|---|
| 60Hz + sync, run 1 | 1694.5ms | 8.8ms | 114.2% | 10 | 29f | 79.0ms | 7 |
| 60Hz + sync, run 2 | 1535.1ms | 0.0ms | 114.0% | 10 | 29f | 76.2ms | 6 |
| 240Hz + sync | 1533.7ms | 0.0ms | 113.8% | 10 | 29f | 89.0ms | 8 |
| **240Hz default (healthy)** | **1695.5ms** | 8.2ms | 113.6% | 10 | 29f | 89.6ms | 9 |
| Threadripper beta.3 | 1643ms | — | 112% | 10 | 29f | 84.9ms | 8 |

Five configurations across two machines, including the dev box's bare shipping default at
240Hz, read the same ~1.5–1.7s to within run variance — with `ui gap max` 2.1–2.5ms, so the
thread never stopped for anything like 1.6s and no single decode exceeds 90ms. It is the
artefact the reading guide already names: a reversal crosses the same frames twice, and a
frame first crossed early in the drag and presented on the later pass is charged the whole
interim. The scripted gesture makes the interim — and therefore the figure — reproducible
across machines. **Quote `p2p end`, `behind` and `hitch` from that leg** (0–8.8ms, 0/40f,
and the file's own class respectively, everywhere). The single 84.9ms decode in the paste is
the 29-frame GOP walk and appears in every config including the healthy one. Optional
instrument work, unscheduled: charge `p2p` from the *latest* crossing of a frame, so the
reversal leg stops printing a number that reads as a 1.6-second freeze that never happened.

## Residue 3 — hitch 8: leg 2's is THIS FILE'S OWN CLASS, not a Threadripper deviation;
## leg 1's is residue 1.

The "recorded hitch 1 class" belongs to the **4K H.264 pool file**, not this one — the
validation section above records it explicitly ("on the 4K H.264 pool file the reversal leg
reads … hitch 1 — that file's recorded class"). `M&M_TopGun_1080.mp4`'s reversal-leg class on
the dev box is **hitch 8–9** (this doc's own validation: "hitch 9 vs 8", real mouse "hitch 8
both"), and the healthy 240Hz control this session read **hitch 9**. These are decode-walk
hitches — 10 seeks at ra-walk ~22f, walk max 29f, paint gap max 84–86ms in that leg on every
config including the healthy one — and the Threadripper's 8 sits inside the class. Leg 1's
hitch (0 healthy, 2 on the fault model, 8 there) is the one genuine deviation, and it is
residue 1's mechanism, closed by the same fix.

**Net: one real residue (leg 1's refresh-locked gate), one metric artefact, one
misattributed baseline.** The remaining feel gap after residue 1 is fixed is the 60-vs-240Hz
display physics plus this file's own walk hitches, both of which the dev box shows equally.

---

# The gate margin (2026-08-19, third session): `refresh × 1.05`, built and validated

One line in `MainWindow::scrubPaintGatePeriodMs()`:
`max(refreshMs, 2 × costEma)` → `max(refreshMs * 1.05, 2 × costEma)`. Nothing else in this
pass — no decode or threading policy was touched.

## Fault model (60Hz + `TRACE_PRESENT_SYNC=1`), leg 1, `M&M_TopGun_1080.mp4`

| leg 1 (forward sweep) | pre-fix ×2 | fixed ×2 |
|---|---|---|
| paint cost EMA | **16.53 / 16.54ms** (= the refresh period) | **5.65 / 5.54ms** |
| supply | 97.5 / 97.7% | **100.2 / 100.3%** |
| behind at end | 5 / 7 frames | **0 / 0** |
| p2p end | 50.0 / 50.1ms | **0.7 / 0.9ms** |
| release | 32.7 / 34.6ms | **0.0 / 0.0ms** |
| dec f/s | 152.6 / 152.4 | 157.4 / 157.4 |
| hitch | 2 / 2 | 3 / 3 |
| delta | 0 | 0 |

The sustained phase lock is gone — the cost EMA no longer reads the display's period, the
chain runs at full supply, and the release that used to absorb a blocked present lands
instantly. Legs 2–4 stay inside their classes (leg 2 hitch 8/9 against the file's own 8–9
class; leg 3 hitch 0; leg 4 hitch 3 against an observed 1–4 spread).

**The honest half: `hitch 3` and a ~5.5ms cost EMA remain on leg 1 under the model, and the
margin is NOT the lever for them — measured, not assumed.** A diagnostic build at
`refresh × 1.10` reads cost 8.63 / hitch 4 — no better — so the residual does not scale with
the margin. The attribution: the in-binary model presents at sync interval 1 into a
**2-buffer** flip swapchain, which leaves no queue slack at all, so occasional
delivery-timing jitter still lands a present that blocks a full refresh. The Threadripper's
real class is a driver/composition throttle over the same swapchain with DXGI's default
frame latency of 3 in force, so the expectation — to be confirmed by the next Threadripper
paste, not asserted — is that the residual is the model's, not the machine's. What the paste
should show for leg 1 there: `paint cost` well below 4.76ms, `paint gap` avg near 17.5ms
(against the pre-margin 23.0), and the leg-1 hitches gone or near it.

## Healthy 240Hz, all figure-for-figure against the same-day pre-fix control

Selftest, bare default: leg 1 hitch 0 both, paint gap 6.6/12.3 vs 6.6/12.4ms, painted 224 vs
226, supply 100.0 both; leg 2 hitch 9 vs 9, rev-hit 97.8 vs 97.9%, seeks 10 both, walk max
29 both; leg 3 identical; leg 4 hitch 4 vs 4; `delta 0` on every leg of both builds. The
gate reads 4.38ms (4.17 × 1.05) and the cost term still never binds (0.32–0.36ms).

Real mouse, physical panel, bar mode widened to 1280 (`win 1264x1083`,
`display 1041x586/1066x600` per run): `M&M` forward `-SnapRelease` **`target 240 shown 240
delta 0`**, hitch 0, behind 0/5f, release 3.7ms; `M&M` `-Reversals` **rev-hit 97.3%
(439/451), seeks 12, walk max 29f, hitch 8, delta 0** — the recorded pre-fix control to the
digit; 4K H.264 `-Reversals` **rev-hit 97.3%, seeks 6, hitch 1, delta 0**, release 46.9ms —
that file's recorded class; 4444 `-SnapRelease` **`target 261 shown 261 delta 0` full-res
`YUV444P12 planar`, release 22.2ms, hitch 0, `land 0`** — the recorded landing standard.
Cadence, 4K H.264 ×2, `TRACE_NO_AUDIO=1`, scratch INI: **100.0 / 100.0% of real time**, 120
frames, `drop 0`, `rephase 0`, all 119 gaps ~1×, `handler>budget 0 of 119`.
