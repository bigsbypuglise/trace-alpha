# The 4K 9:16 Seedance/ComfyUI scrub report — measurement, 2026-08-14

`16_4kSeedance_9x16_Comfyui_MP4\video_ComfyUI_00004_.mp4`. Owner report: **scrubbing is poor,
playback is fine.** Second scrub report from the AI-export class in two days.

Physical panel, 5120x1440 @ 239.999Hz, cursor parked on the primary, `TRACE_TRANSPORT_BAR=1`,
`TRACE_NO_AUDIO=1`, shipping `build\app\Release\Trace.exe` at `201ce54`. Window widened to
1400px for every run — see "the 9:16 HUD problem is free to solve" below; `display` is
**identical** widened and not, so no figure here is a wide-window figure.

---

## The answer in one paragraph

**The file has exactly ONE keyframe — frame 0 — for all 97 frames.** Every random-access
request that misses the frame cache therefore seeks to the head of the file and walks the whole
distance back to the target, and a walked frame on this media costs **8.9 ms**. That walk is
synchronous on the UI thread for a timeline click, a frame step and a scrub release, so the
application **freezes for 261 ms at 30% of the clip, 431–520 ms at 60% and 585 ms at 85%**
against 59–94 ms for the same gesture on 4K H.264. Playback never touches that path and
measures **100.0% of real time**, which is exactly the split the owner reported. It is not the
async round trip (the batch reads `last 1 max 1` here), not the GOP-versus-cache-depth thrash
the handoff predicted, and not a regression from anything that shipped this week.

---

## What the file actually is — and it is not what the handoff assumed

The handoff reasoned from "a long `libx264` `keyint` these tools leave at default". This file is
not `libx264` and not H.264:

| | 14_720P ComfyUI | **16 Seedance 9:16** |
|---|---|---|
| codec | h264 High | **hevc Main 10** |
| pixel format | yuv420p, 8-bit | **yuv420p10le, 10-bit** |
| size | 1280x720 | **2160x3840** (8.3 Mpx) |
| B-frames | **none** | **reorder depth 2** |
| frames / duration | 361 / 15.04s | 97 / 4.04s |
| bitrate | 4.9 Mbps | 34.2 Mbps |
| **keyframes** | **7** (gaps 24/98/77/45/77/34) | **1 — frame 0, the whole clip** |

Read back by Trace's own opener (`TRACE_OPEN_LOG=1`), not only by ffprobe:
`codec=hevc w=2160 h=3840 pixfmt=yuv420p10le fps=24.000000 fpsQ=24/1 frames=97 depth=10
tagmtx=bt709 tagrange=limited timecode=none`.

For context, every other H.264 file in the pool: 4K `Splash_1` 5 keyframes / 121 frames,
1080p `M&M_TopGun` 9 / 241, 4K 60fps 6 / 163. The shape fixtures are single-keyframe too, but
they are 1080p 8-bit synthetic material and nobody scrubs them.

---

## Playback is fine, and the number says so

`cadence.ps1 -Seconds 5 -Repeats 2`, scratch `TRACE_SETTINGS_FILE`:

**100.0% of real time on both runs**, 96 frames in 4.00s, `handler 3.55/4.43ms` and
`3.43/4.54ms` against a 41.67 ms budget, `outside 37.95ms`, **all 95 cadence gaps in the ~1x
bucket** and none in any other, p50 41.7 / max 44.1 ms, `drop 0`, `rephase 0`.

`dec 0.01/1.75ms` during playback. **That is not the per-frame decode cost and must not be read
as one** — long-GOP keeps `FF_THREAD_FRAME|FF_THREAD_SLICE` (`thr frame` on the HUD), so the
decoder works through the 38 ms the tick spends idle and hands the tick a frame that is already
finished. The true sustained cost only appears when frames are demanded back-to-back, which is
what a walk does.

---

## The cost model, from two clicks

`clickland.ps1` presses and releases once on the groove from frame 0 and measures how long the
window stops answering messages. A click is a jump: press and release on the same value, landed
exactly through `RequestMode::Step`, **synchronously on the UI thread** (`scrubJumpPending_`,
`c3335ec`). Its whole cost is seek + walk.

| click at | frame | frames walked | **window frozen** |
|---|---|---|---|
| 0.30 | 28 | 28 | **261 ms** |
| 0.60 | 57 | 57 | **431 / 453 / 459 / 520 ms** (four runs) |
| 0.85 | 82 | 82 | **585 ms** |

Two-point solve on the first two: **8.9 ms per walked frame, intercept ≈ 11 ms.**

**The intercept is the finding as much as the slope.** The reverse-shuttle work costed a
keyframe-aligned sample on long-GOP H.264 at ~30 ms fixed and hypothesised 20–24 ms of it was
the frame-threaded decoder refilling after a flush. Here seek + flush + the landing conversion
+ the paint together are ~11 ms. **The seek is free on this file. The walk is the entire cost**,
and it is bounded by the clip rather than by a GOP.

Confirmed from the inside on the 0.85 click: `dec 552.50ms` last, `walk 82f`,
`cache 3cv/15.22ms`, `seek 0.22/0.20 n=3`. 552 ms of decode, 15 ms of conversion.

Controls, same gesture, same instrument:

| file | click 0.30 | click 0.60 | walk at 0.60 | `dec` last |
|---|---|---|---|---|
| **16 Seedance** | **261 ms** | **431–520 ms** | **57 f** | **552.50 ms** (at 0.85) |
| 4K H.264 `Splash_1` | 59 ms | 94 ms | 13 f (at 0.85) | 36.23 ms |
| 720p ComfyUI | 54 ms | 19 ms | 64 f (at 0.85) | 23.24 ms |
| 1080p `M&M_TopGun` | — | — | 2 f (at 0.85) | 22.19 ms |

**The 720p file is the control that makes the mechanism legible.** It walks 64 frames on one
click — as far as this file walks at 0.60 — and it costs **23 ms**, because a frame there is
0.36 ms. It also cached **all 64** frames it walked (`64cv/15.40ms`) where this file cached
**3 of 82**, because a conversion is 0.24 ms there and 5.07 ms here against the same 18 ms
budget. Same GOP problem, opposite outcome. **The 720p report was right to rule the GOP out;
that ruling does not transfer, and neither does its cause.**

---

## Where it is felt

### 1. Every timeline click — 261 to 585 ms of dead window

Above. This is the "slow to lock on" failure the project has met before (`c3335ec`), at four to
six times the size.

### 2. Frame stepping — 2 ms, 2 ms, 2 ms, then 411 ms

`stepcost.ps1`, 24 backward steps after a click at 0.60, shipping defaults:

```
step back x24 : 10 2 3 2 4 2 4 2 2 2 411 2 2 3 4 3 2 4 3 3 4 2 4 4
```

4K H.264 for the same sequence: `3 2 2 2 3 2 2 2`, max 3 ms.

`TRACE_SEEK_LOG=1` names the mechanism exactly. The click is
`seek#2 reason=StepJump requested=58 current=0`. The freeze is
`seek#3 reason=DecoderBehindOrAtTarget requested=54 current=55 lastDecoded=58` — the decoder is
parked where the click left it, the cache covers the handful of frames the walk was allowed to
keep, and the first step past them re-walks from frame 0.

**The cache cannot paper over this, and the reason is a byte budget rather than a policy.** A
full-resolution entry here is **23.7 MB** (Y 2160x3840x2 plus two 1080x1920x2 chroma planes),
so the 384 MB budget holds **16 frames of a 97-frame clip** — `cache 2/16 (47.8/384 MB)` on the
HUD, read directly. Preview entries during a drag are 1.5–3.6 MB and 100+ of them fit
(`cache 172/173 (381.7/384 MB)`, `rev-hit 98.2%`) — but **a preview entry cannot serve a step or
a landing**, by design, so stepping is stuck with the 16.

### 3. The press that starts a drag anywhere but the playhead

Backward drag, cold, `scrub.ps1 -Seconds 1.5 -Backward`: `ui gap 1.99/439.5ms` with
**`over 16ms: 2 of 661`** — two events, not a pattern. The forward drag on the same file, whose
press lands on the playhead and walks nothing, reads `ui gap 1.31/6.4ms`, `over 16ms: 0 of 1458`.

### 4. Forward dragging is clean and should be said so

`behind 0/0f`, `p2p 4/8ms`, `dec 62.6 f/s` against `ptr 49.1 f/s` (**supply 127%**),
`walk max 0f`, `seeks 1`, `paints 100/100`, `stalls 0 of 93`, **`hitch 0`**, landing
`target 96 shown 96 delta 0`. A forward drag never leaves the walked run, so the single keyframe
costs it nothing.

### 5. The batch is not the binding term — confirmed as instructed, before anything else

Forward drag `batch cap 4 last 1 max 1`; backward `batch cap 4 last 1 max 4`. The 8 ms walk
budget collapses it exactly as it does on ProRes 4444. `be9f7ec` cannot help this file and does
not harm it.

---

## What the shipping knobs do here

### `TRACE_SCRUB_FILL_MS` — real, and it fixes the drag but not the press

Backward drag, three runs, everything else identical:

| | **60** (ships) | **240** (§15.2's recorded decision) | **600** |
|---|---|---|---|
| `hitch` | **2** | **0** | **0** |
| `smooth` max | **480.7 ms** | **4.9 ms** | **4.9 ms** |
| `stalls` | 2 of 56 | 0 of 55 | 0 of 55 |
| `dec` | 42.7 f/s | **54.1 f/s** | 53.8 f/s |
| `rev-hit` | 90.3% (56/62) | 91.8% (56/61) | 91.8% (56/61) |
| `seeks` | 3 | 2 | 2 |
| `ui gap` max | 443.5 ms | 438.0 ms | 439.0 ms |
| `behind` max | 52 f | 55 f | 55 f |

240 is a clear win inside the drag and **600 is indistinguishable from 240**, so the budget
saturates — it is bounded by what fits, not by time. It does **nothing** for the press freeze,
which is the dominant complaint, and `ui gap max` says so.

### `TRACE_SEEK_CACHE_WINDOW` — buys step coverage at ~5% of the landing

| forced window | click at 0.60 | first freeze in 24 backward steps |
|---|---|---|
| default (18 ms budget ≈ 10 frames) | 431 ms | **step 11**, 411 ms |
| 16 | 453 ms | **step 16**, 362 ms |
| 64 | 459 ms | none in 24 (see caveat) |

**64 is clamped to 16** — `cacheWindow` is bounded by `entriesThatFit(false)` = 384 MB / 23.7 MB
= 16 — so the third row should equal the second and on one run it did not. Treat the third row
as unexplained variance rather than as a result; what is solid is that **+22 ms on a 431 ms
landing moves the next freeze from step 11 to step 16**, and that 16 is a hard ceiling.

The recorded discrepancy is confirmed on the way past: `scrubWalkCacheBudgetMs` initialises to
**240.0** and `open()` overwrites it unconditionally with `envInt("TRACE_SCRUB_FILL_MS", 60)`,
so **60 is what ships** and §15.2's 240 is not in force. `reverseCacheCapacity`'s stale BGRA
footprint is **not** implicated: `entriesThatFit` prices from `fullResEntryBytes`, learned from a
real entry.

---

## The generalisation the handoff asked for — restated, because the proposed one does not survive

The handoff proposed that the two AI-export reports share a **coarse irregular GOP from a
default `libx264` keyint**, with resolution deciding only whether the cache can cover a GOP.
**The encoder half is wrong** — one file is `libx264` 8-bit with no B-frames and seven
keyframes, the other is HEVC Main 10 with B-frames and one — and **the cache half is not what
bit here**: the drag cache reads `rev-hit 90–98%` on this file and does fine.

What actually predicts the failure is a product of two terms, and both have to be large:

> **cost of a random-access miss = (frames back to the previous keyframe) x (per-frame decode cost)**

- 720p ComfyUI: 98 x 0.36 ms = **35 ms**. Harmless. Its real fault was the async round trip.
- 4K H.264: 29 x 2.8 ms = **81 ms**. Tolerable, and it is the recorded 90–125 ms press landing.
- **This file: 96 x 8.9 ms = 855 ms.** Not tolerable anywhere.

So the class is not "AI exports" and not "long GOP". It is **any file where the walk back to a
keyframe costs more than a few hundred milliseconds**, and what AI video tooling contributes is
only that it picks `keyint` for file size with no thought for review — up to and including one
keyframe for the whole clip. **Both numbers are needed to predict an asset; a keyframe count
alone would have cleared the 720p file and condemned it equally.** Both are already read at open
(`TRACE_OPEN_LOG` prints the codec and size; the keyframe interval is not printed yet and would
be a cheap addition).

---

## What is NOT the cause, each checked rather than argued

- **The async scrub batch** (`be9f7ec`): `last 1 max 1` forward. Not binding.
- **Cache depth thrash across a GOP**: the drag cache reaches `rev-hit 98.2%` with `seeks 2` on
  the reversals gesture and `walk max 3f`. The drag is not thrashing.
- **The seek and the frame-threading flush**: the two-point intercept is ~11 ms, including the
  landing conversion and the paint.
- **I/O**: `io play seq 100.0% seek 0 stall 0`, `src local (fixed local volume) NTFS 16.5 MB
  34.2 Mbps`, `open 13.14ms`, `streaminfo 0.82ms`. The whole file is 16.5 MB.
- **Colour, alpha, bit depth, rotation, SAR**: `sar 1:1* dar 0.5625`, `rot 0`,
  `display 460x818 filtered x3`, tagged bt709 limited and honoured.
- **The window shape**: `display 460x818` is identical at the §4 default (`win 496x1287`) and
  widened (`win 1384x1287`), because a portrait fit is height-bound.

---

## Under a fast gesture the drag is supply-limited, and this needs a control to read

Reversals gesture (`scrub.ps1 -Reversals`, press at frame 0 so there is no press jump):

| | **16 Seedance** | 4K H.264 | 720p ComfyUI |
|---|---|---|---|
| `dec` | 130.2 f/s | 168.5 f/s | 540.7 f/s |
| `ptr` | 128.3 f/s | 160.7 f/s | 486.2 f/s |
| supply | 102% | 105% | 111% |
| `behind` max | 46 f | 39 f | 34 f |
| `rev-hit` | 98.2% | 97.3% | 98.8% |
| `walk max` | 3 f | 6 f | 45 f |
| **`hitch`** | **6** | **1** | **1** |
| `smooth` max | **161.3 ms** | 91.3 ms | 36.6 ms |
| `ui gap` max | 186.0 ms | 76.8 ms | 17.4 ms |
| paints | 334 | 424 | 1368 |

**`p2p` max reads 1000 / 1216 / 2420 ms and is therefore useless here** — it is worst on the
*healthiest* file. Quote `hitch` and `smooth max`. On those, this file is 5–6x the control.

That residue is ordinary supply limit — 130 f/s of preview decode against a pointer asking for
up to 194 — and §15's sampling is gated on `AV_CODEC_PROP_INTRA_ONLY`, which HEVC is not.
**Do not reach for sampling**: strided steps on long-GOP measured catastrophic (hits 85.4% →
13.3%), and one keyframe makes that worse, not better.

---

## Open, and it belongs to the owner

**Does another player scrub this file well, and is it showing the exact frame?** The same
contract question the 8K and 720p reports both needed, and it is sharper here than usual:
showing frame 57 of a single-GOP file *requires* decoding 58 frames, in any player ever written.
A player that feels instant is doing one of two different things — decoding off its UI thread so
the window stays alive while the picture catches up, or showing an approximate frame during the
drag and the exact one on release. **Trace can copy the first without giving anything up. The
second is a product decision** and is the rule this project has already taken six times for
frames *in motion* (fidelity is owed to the frame you stop on) — but never yet for a click,
which is a landing.

---

## Options, ranked, none built

1. **Take the exact landing off the UI thread** (roadmap item 2b, already written down: *"it
   could be issued to the worker and awaited with the event loop alive, the way remote reads
   already are"*). Does not make 520 ms shorter; makes it not a freeze. Highest user-visible
   value here, and it is the same ownership machinery as the open checkpoint-2 work — **sequence
   it with that, not before it.**
2. **Make the fill budget proportional to the walk it is keeping, instead of a constant 18 ms.**
   Measured above: +22 ms on a 431 ms landing, freeze frequency from every ~10 steps to every 16.
   Small, low risk, and it retires a constant whose premise (*"every speculative conversion is
   delay in front of the one frame that is wanted"*) expires precisely when the walk is expensive.
   **Capped at 16 entries by the 384 MB budget, so it is a 60% improvement to stepping, not an
   order of magnitude** — and more cache bytes is not the answer (§26.5, 768 MB past the knee).
3. **Attack the 8.9 ms.** `skip_frame = AVDISCARD_NONREF` while walking, restored before the last
   few frames so the landing stays exact. This file's mini-GOP is I/P at +4 with B at +2 and
   non-reference b at +1/+3, so roughly **half the frames are skippable** on a walk. Standard
   technique, real upside, and the most invasive of the three: it is decoder surgery on the one
   path that must never be approximate.
4. **Ship `TRACE_SCRUB_FILL_MS=240` as the default.** Measured here (`hitch 2 → 0`,
   `smooth max 480.7 → 4.9 ms`) and recorded as the intended value at §15.2 in the first place.
   Needs re-measuring on 4444 and 4K H.264 before it moves, because it is a shared path.

---

## Harness added

- **`scripts/measure/widen.ps1`** — widens the window without changing the video rect, and
  proves it by printing `display` either side. Portrait media is height-bound, so width is pure
  letterbox. This is what makes phase 12's "the dev HUD clips on narrow windows" limitation
  cost nothing on 9:16, 4:5 and 1:1 material, and it is also the only reason `scrub.ps1` can run
  on this file at all — at the §4 width the groove is under its 300 px minimum run and the
  harness reports `groove not found`. **It steps +1/-1 afterwards**, because `refreshHud()` is
  not called on `resizeEvent` and the HUD would otherwise redraw the new geometry with the old
  string in it.
- **`scripts/measure/clickland.ps1`** — one groove click, timed from outside as the longest
  stretch during which the window stops answering messages. No drag harness measures this:
  `scrub.ps1` presses and immediately sweeps, so the press cost is folded into the gesture.
- **`scripts/measure/stepcost.ps1`** — per-step freeze for N steps in one direction. Stepping
  never involves the scrub worker, so no scrub harness reaches it.

### A stale instrument, caught this time before it wrote anything down

`clickland.ps1`'s first version sent **one** `SendMessageTimeout` after the click. A sent message
is serviced ahead of posted mouse input, so the probe was answered before the click was
dispatched: it reported **3 ms** for a landing that went on to block for 450, and it did so
consistently enough to look like a result — it "showed" that `TRACE_SEEK_CACHE_WINDOW=16`
removed the click freeze entirely. It does not. Polling with a 5 ms timeout and reporting the
longest contiguous dead stretch reproduces the HUD's own `ui gap max` and the two now agree.
**Eighth instrument in this project to accuse or exonerate a build wrongly** — and the first
where the wrong answer was the *flattering* one.
