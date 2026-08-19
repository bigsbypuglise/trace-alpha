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
