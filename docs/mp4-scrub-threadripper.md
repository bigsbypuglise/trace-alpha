# MP4 scrub is poor on a higher-core machine — investigation brief

Owner report, 2026-08-19. Scrubbing MP4 files is badly degraded on a second machine, seen
before and dismissed as a one-off. **ProRes is not affected. Only MP4.** Top priority.

> **STATUS, 2026-08-19 (later the same day): THE THREADING HYPOTHESIS BELOW IS REFUTED.**
> `TRACE_DECODE_THREADS=8` and `TRACE_LONGGOP_SLICE_THREADS=1` were both tried on the
> Threadripper and neither helped — so the frame-pipeline-refill mechanism this brief leads
> with is not the cause, and its five-minute test section is retained as the record of what
> was ruled out. The fault also reproduces on `3_1080p_H264_MP4\M&M_TopGun_1080.mp4`, the
> validated pool file, so **it is the machine, not the media.**
>
> **The leading hypothesis is now the worker round trip** — the cross-thread cost of the
> async scrub chain (post → condition-variable wake → decode → QueuedConnection delivery →
> UI-thread drain). Playback decodes synchronously on the UI thread and pays none of it,
> which fits the reported symptom exactly: scrub degraded, playback untouched. On the 720p
> ComfyUI file that round trip was measured at 97% of the delivery interval; on a 32-core
> Zen 2 Threadripper (4 CCDs, cross-CCD synchronisation) it is the term that can scale with
> the machine while the file and the code stay identical. Nothing reported it in isolation
> until now.
>
> **The machine is locked down** — no session can run on it and no harness can be driven
> there — so the next step is `--scrub-selftest`, below: one command, one pasteable block,
> built to discriminate between the remaining hypotheses. **Do not change any scrub
> constant until its output from that machine has been read** (see "Do not adapt anything
> yet" at the end).

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

Three legs, each against a fresh reopen of the clip (cold cache, per-leg counters — the same
reason the harnesses restart Trace per run), window forced to 1280x760 logical so both
machines measure at one size (window size drives cache depth, §22.8):

1. **forward sweep** — the whole clip in 1.5s, snap release (`scrub.ps1`'s default gesture);
2. **reversal drag** — `scrub.ps1 -Reversals`, segment for segment;
3. **slow forward drag** — ~3x speed for 2s. No recorded counterpart; it exists because a
   fixed per-request cost shows cleanest at low demand, where `p2p` is nearly pure latency.

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

## Do not adapt anything yet (part 2, deliberately not started)

Three constants in the scrub path are tuned to the dev box and are the honest suspects: the
**batch cap of 4**, the **8ms walk budget**, and the **60ms seek-walk fill budget**. Two
others are already properly derived and are the model to follow — `thread_count` from
`av_cpu_count()`, and the byte-budgeted reverse cache. **Adapting the wrong one is worse
than adapting none**, and any change to those three risks the home box's recorded figures.
Wait for the selftest's output from the affected machine; it names which term is wrong
before anything is derived from anything. When an adaptive version is built, the safety
property is that **it must reproduce the tuned values on the home box** — if it converges to
something other than 4 there, it is wrong.
