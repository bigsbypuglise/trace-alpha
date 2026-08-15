# Decode threading on the 8K plate — the knee, and what it settles

Measured 2026-08-15. Physical panel 5120x1440 @ 239.999Hz, `d3d11`,
`win 1066x1083`, `display 1066x600 filtered x4`. Machine: AMD Ryzen 9 5900XT,
**16 cores / 32 logical**. File: `12_8K_ProRes4444\Foces_8K_Lut_Dino
Stomp_plate_4444XQ.mov` — 7680x4320 ProRes 4444 XQ, `yuva444p12le`, 23.976 fps,
145 frames, 5739 Mbps.

Harness: `scripts/measure/decthreads.ps1` (sweeps the knob, samples process CPU
across the play window only) and `scripts/measure/strip.ps1`.

**Playback queue left at its default (off) throughout, so this measures decode
throughput and nothing else.** Sweep runs use `TRACE_RT_DROP=0` so `presented`
*is* pipeline throughput and `drop` is 0 by construction; a separate
shipping-config pass reports `drop` and `media%`.

---

## FIRST: THE DEFAULT IS ALREADY 32, AND THE NOTE THAT SAID OTHERWISE WAS MINE

`docs/async-decode-queue-stage-one.md` and the handoff it produced both said
*"`TRACE_DECODE_THREADS` … still sits at FFmpeg's automatic count, which caps at
16 on a 32-thread box"*, and recommended raising it as the cheapest remaining
lever. **That is wrong.** `VideoDecoderFFmpeg.cpp:1005` has read

```cpp
const int cpuThreads = std::min(av_cpu_count(), 64);
impl_->codec->thread_count = envInt("TRACE_DECODE_THREADS", intraOnly ? cpuThreads : 0);
```

since checkpoint 1. Intra-only has defaulted to the machine's logical CPU count —
**32 on this box** — for some time; the "+21% by setting it to 32" figure was
measured *before* that default landed, and the win had already been banked. The
note quoted the old measurement against the new code.

**Thirteenth premise expiry in this project, and the first written by me.** The
guard against a fourteenth is now in the product rather than in a note: the HUD's
threading field reads **`thr slice x32`** / **`thr frame x16`**, read back off the
codec context rather than recomputed from the environment, so "the default is N"
is an observation instead of an inference. It earned itself on its first run —
4K H.264 reads `thr frame x16`, confirming long-GOP is untouched by the
intra-only rule.

**Consequence: option 3 of the stage-one report is closed as already-taken.**

---

## The sweep — vcpkg build (shipping), `TRACE_RT_DROP=0`

| threads | HUD | `dec` avg | `sws` avg | `upload` avg | presented | % real time | `handler>budget` | `drop` | CPU (cores of 32) |
|---|---|---|---|---|---|---|---|---|---|
| **DEFAULT** | `slice x32` | **45.68** | 17.57 | 12.23 | **12.72** | **53.0%** | 143 of 143 | 0 | **15.8** |
| 1 | `slice x1` | 661.23 | 18.92 | 13.17 | 1.45 | 6.0% | 15 of 15 | 0 | 1.0 |
| 8 | `slice x8` | 98.14 | 17.93 | 11.67 | 7.68 | 32.0% | 85 of 85 | 0 | 5.8 |
| 16 | `slice x16` | 62.29 | 17.34 | 11.90 | 10.60 | 44.2% | 119 of 119 | 0 | 9.6 |
| 20 | `slice x20` | 56.90 | 17.27 | 12.06 | 11.23 | 46.8% | 125 of 125 | 0 | 11.3 |
| 24 | `slice x24` | 52.42 | 17.52 | 12.14 | 11.78 | 49.1% | 131 of 131 | 0 | 13.4 |
| 28 | `slice x28` | 48.88 | 17.36 | 12.20 | 12.27 | 51.2% | 138 of 138 | 0 | 14.4 |
| **32** | `slice x32` | **45.95** | 17.60 | 12.18 | **12.66** | **52.8%** | 143 of 143 | 0 | 15.5 |
| 40 | `slice x40` | 45.11 | 17.72 | 12.19 | 12.80 | 53.4% | 143 of 143 | 0 | 15.8 |
| 48 | `slice x48` | 45.10 | 17.87 | 12.27 | 12.75 | 53.2% | 143 of 143 | 0 | 15.6 |
| 64 | `slice x64` | 45.12 | 17.87 | 12.36 | 12.75 | 53.2% | 143 of 143 | 0 | 15.8 |

## The sweep — minimal GCC/mingw LGPL FFmpeg build

| threads | HUD | `dec` avg | `sws` avg | `upload` avg | presented | % real time | `handler>budget` | CPU |
|---|---|---|---|---|---|---|---|---|
| **DEFAULT** | `slice x32` | **39.65** | 18.01 | 12.33 | **13.58** | **56.7%** | 143 of 143 | 13.8 |
| 16 | `slice x16` | 55.53 | 17.40 | 12.25 | 11.37 | 47.4% | 127 of 127 | 9.2 |
| 24 | `slice x24` | 46.10 | 17.96 | 12.57 | 12.54 | 52.3% | 141 of 141 | 12.2 |
| **32** | `slice x32` | **39.08** | 18.01 | 12.33 | **13.64** | **56.9%** | 143 of 143 | 13.8 |
| 40 | `slice x40` | 39.19 | 18.04 | 12.32 | 13.62 | 56.8% | 143 of 143 | 13.5 |

## Shipping configuration (`TRACE_RT_DROP` on, default threads)

| build | presented | % RT | frames | `drop` | `media` | `handler>budget` |
|---|---|---|---|---|---|---|
| vcpkg | 12.61 | 52.6% | 78 | **67** (ticks 67, max 1) | **97.8%** | 77 of 77 |
| minimal | 13.46 | 56.2% | 83 | **62** (ticks 62, max 1) | **98.1%** | 82 of 82 |

`media ~98%` is the owner's 2026-08-13 decision working: the *movie* stays on the
clock and *picture* is dropped. It is not acceptance of the file.

---

## The knee, and why it is where it is

**The knee is at 32 = the logical CPU count, and the curve is flat beyond it to
64.** `dec` reads 45.95 / 45.11 / 45.10 / 45.12 at t=32/40/48/64 on vcpkg, and
39.08 / 39.19 at t=32/40 on the minimal build. There is nothing above the knee.

**Scaling, vcpkg, from t=1:** 661.23 → 98.14 (6.7x at 8) → 62.29 (10.6x at 16) →
52.42 (12.6x at 24) → 45.95 (**14.4x at 32**). Amdahl on the t=1/t=32 pair gives
a serial fraction of **~3.9%**, whose asymptote would be ~26ms; the measured
floor is 45ms and flat, so **the limit past 16 threads is not thread count.**

**CPU never exceeds ~50% of the machine — 15.8 of 32 logical cores at every
setting from 32 upward.** The box is not thread-starved and is not saturated. The
16→32 range is SMT siblings on 16 physical cores and returns about 26% for double
the threads, which is the ordinary SMT yield; beyond 32 there are no more
execution resources to ask for. **Whatever bounds ProRes 4444 XQ decode here is
per-core throughput and memory traffic, not parallelism, and no thread setting
reaches it.**

---

## Can decode throughput alone reach 23.976 fps? No — and it is not close

At 23.976 fps the whole frame budget is **41.71 ms**. At the knee on the fastest
build, **decode alone is 39.08 ms — 94% of the entire budget** — before
conversion (18.01), upload (12.33), paint or scheduling. Serial total 72.02 ms.

Three ceilings, each measured rather than assumed:

- **Decode-only ceiling if everything else were free:** 1000 / 39.08 =
  **25.6 fps.** That is the absolute upper bound of any pipeline arrangement on
  this build and this machine, and it leaves 2.4 ms of headroom for conversion,
  upload, paint and the scheduler combined.
- **Best measured throughput, decode work alone:** **13.64 fps = 56.9% of real
  time** (minimal build at the default).
- **Best measured throughput including stage one at depth 2** (previous session):
  **14.87 fps = 62.0%.**

**The answer to the question asked is no.** Decode threading is already at its
knee by default, the knee is flat, and the plate sits at 57%. Threading was never
the missing 43%.

---

## Recommendations

**1. Shipping/default thread policy: keep exactly what ships. Change nothing.**

`av_cpu_count()` clamped to 64 for intra-only; FFmpeg's automatic count for
long-GOP. The sweep confirms the intra-only default already sits on the knee, and
the policy is *derived from the machine* rather than hard-coded — a four-core box
gets 4, which a literal 32 would break. Long-GOP must keep the automatic count
because there `thread_count` is frames in flight and a deeper pipeline costs a
longer refill after every seek; **`thr frame x16` on 4K H.264 confirms that split
is live.** `TRACE_DECODE_THREADS` stays as the override and the control.

**The raised count also helps random access, which is worth recording because the
opposite was plausible.** 4K ProRes 4444 `scrub -SnapRelease`, default (32)
against `TRACE_DECODE_THREADS=8`: shuttle **29.63 → 15.89 ms/frame**, `hitch`
**2 → 0**, paints **48 → 84**, with `target 261 shown 261 delta 0` and full-res
`YUV444P12 planar` on both. Slice threading means `thread_count` is threads *per
frame*, so a seek needs no pipeline refill and there is no scrub penalty to trade
against.

**2. Best measured 8K throughput: 13.64 fps (56.9% of real time)** on the minimal
GCC FFmpeg build at the default thread count, `drop 0`, full quality, every frame
presented. In the shipping drop-enabled configuration the same build reads
**13.46 fps presented with `media 98.1%` and `drop 62`.**

**3. Stage two is NOT justified for the purpose it was funded for.**

It was funded to bring the 8K plate close to real time. The arithmetic now has
measured numbers in every term:

- A *perfect* two-stage pipeline gives `max(dec, sws, upload+paint)` =
  `max(39.08, 18.01, ~13.5)` = **39.08 ms = 25.6 fps**, at zero contention.
- Stage one **measured** the contention that arrangement runs into: `sws` +24%
  and `upload` +91% once two stages run concurrently. There is no reason decode
  is exempt, and it has 2.4 ms of margin against the target.
- So stage two's realistic landing zone is **roughly 22–25 fps in the best case
  and below 24 in the likely one**, in exchange for changing the decoder's output
  boundary — `VideoFrame` is post-conversion, and `VideoFrame.h` admits no FFmpeg
  type because the image-sequence path must compile without FFmpeg.

**Stage one already banked the cheap half of that (53.6 → 62.0% combined) with no
boundary change and a one-line default.** Stage two is a structural change to buy
the expensive half of a gap it probably cannot close.

**What is actually left is outside both stages:** decode is 39 ms, flat in
threads, at 50% CPU. Closing a 1.6x gap on ProRes 4444 XQ needs a faster decoder,
not a better arrangement of this one — and hardware decode is explicitly excluded
by the owner. The honest position is that **this file is not reachable at 23.976
on this machine with this decoder**, which is the same conclusion `ffmpeg -f null`
reached independently at 20.5 fps for decode with every other stage deleted.

---

## Regression — preserved, physical panel

The only code change is a diagnostic readout (`VideoPerfStats::threadCount`, read
off the codec context, printed on the existing threading field). Verified anyway:

4K H.264 cadence x3 **100.0 / 100.0 / 100.0%**, 120 frames, `0 of 119`, all gaps
~1x, `drop 0`, `rephase 0`, `thr frame x16` · ProRes 4444 x2 **99.8%**, 261
frames, `0 of 260`, `thr slice x32` · `scrub -SnapRelease` `target 261 shown 261
delta 0` full-res planar, `hitch 0` · **exact paused stepping**: `-StepCycle`
landed frame 62 and ended frame 62 through 3 x (Right x5 / Left x5) · both
lifecycle legs **81.8%** and the **0% control** · **25 of 25 transitions**.

Async landing behaviour and the stage-one default (off) are untouched.
