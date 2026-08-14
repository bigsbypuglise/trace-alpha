# The 720p ComfyUI scrub report — measurement pass, 2026-08-14

Owner report: scrubbing is poor on
`Trace_Testing_Assets\14_720P_Comfyui_mp4\video_ComfyUI_0000Fly8.mp4`.

**No implementation was begun.** This is the measurement the brief asked for, and it
changed the leading hypothesis. Display throughout: physical panel, 5120x1440 @
239.999Hz, `scr Odyssey G95SC`, `dpr 1.00`, cursor parked on the primary.

---

## 1. The file, characterised

`TRACE_OPEN_LOG=1` and `ffprobe` agree on every field.

| | |
|---|---|
| codec | H.264 High L3.1, `is_avc`, `nal_length_size=4` |
| dimensions | 1280x720, `yuv420p`, 8-bit, SAR 1:1 (assumed — the file states none) |
| rate | **24.000000 exactly**, `fpsQ 24/1`, `tb 1/12288`, CFR |
| length | **361 frames**, 15.0417s |
| bitrate | 7.08 Mbps video, 13,427,235 bytes |
| B-frames | **none** (`has_b_frames=0`) |
| audio | none |
| colour | fully untagged; Trace infers `bt709* limited` |
| timecode | none |
| encoder | `Lavf62.3.100` (ComfyUI VideoHelperSuite — `VHS_*` tags present) |

**PTS is perfectly regular and there is nothing exotic to blame.** Every packet is 512
ticks apart, `nb_frames` (361), `avg_frame_rate` (24/1) and the real packet count (361)
all agree, and with no B-frames there is no reordering. `frameFromPts`, its monotonic
bump and `seekResolvePending` have nothing unusual to do here. **Brief items 3 and 4 are
answered and clear.**

**~120 KB of ComfyUI JSON is embedded in the container** (`prompt` 36,621 chars +
`workflow` 82,955). It costs nothing measurable: `streaminfoMs=5.07`, `openMs=13.75`.

### The GOP — the brief's leading hypothesis, half right

7 keyframes, at packet indices **0, 24, 122, 199, 244, 321, 355**. Gaps
**24 / 98 / 77 / 45 / 77 / 34**, plus a 5-frame tail. So the worst distance back to a
keyframe is **97 frames**.

It is *not* "one IDR at frame 0" — but it is far coarser than anything validated, and
irregular, which is x264 scenecut detection with a long `keyint` rather than a fixed GOP:

| file | packets | keyframes | gap min / med / **max** | B-frames |
|---|---|---|---|---|
| **14 ComfyUI 720p** | **361** | 7 | 24 / **77** / **98** | 0 |
| 3 1080p H.264 | 241 | 9 | 26 / 30 / 30 | 2 |
| 4 4K H.264 | 121 | 5 | 30 / 30 / 30 | 0 |
| 7 4K 60fps H.264 | 163 | 6 | 30 / 30 / 30 | — |

---

## 2. Playback is not the complaint

`cadence.ps1`, `TRACE_NO_AUDIO=1`, scratch `TRACE_SETTINGS_FILE`, x2:
**100.0% of real time both runs**, 276 / 272 frames, `handler>budget 0 of 275` and
`0 of 271`, **every gap in the ~1x bucket**, `drop 0`, `rephase 0`, drift 2.0 / 2.3ms,
`dec 0.09 | sws 0.26 | total 0.46ms` against a 41.67ms budget. Nothing to fix.

---

## 3. The GOP is real but it is NOT the binding term — the cache absorbs it

A 1280x720 `yuv420p` entry is 1.32 MB, so the 384 MB reverse cache holds **291 of the
clip's 361 frames — 80% of the whole file.** After one pass, misses are rare whatever
the keyframe spacing is.

Reversal drag, docked bar (`TRACE_TRANSPORT_BAR=1`), same gesture, same window:

| | rev-hit | seeks | `ra-walk` | `walk max` | **hitch** |
|---|---|---|---|---|---|
| **ComfyUI 720p** | **99.2%** (486/490) | **3** | **7.67** | 23f | **1** |
| 1080p H.264 | 98.3% (412/419) | 5 | 18.40 | 29f | 4 |
| 4K H.264 | 98.0% (246/251) | 3 | 3.00 | 6f | 1 |

**Warm, this file is the best of the three by every cache metric.** `ra-walk 7.67` is
less than half the 1080p file's.

The GOP does show up **cold**, and it shows up exactly where the container predicts.
First backward sweep after open, 1.5s:

| | `walk max` | `ra-walk` | seeks | rev-hit | hitch | worst paint gap |
|---|---|---|---|---|---|---|
| **ComfyUI 720p** | **97f** — the 98-frame GOP, to the frame | **49.86** | 6 | 98.1% | **5** | 66.8ms |
| 1080p H.264 | 29f | 25.11 | 8 | 96.4% | **8** | 76.2ms |

So the long GOP doubles the cold walk cost — and the 1080p file *still* hitches more.
**The GOP is a secondary term. It is not what the owner is seeing.**

---

## 4. WHAT IS ACTUALLY WRONG: the async scrub chain pays one cross-thread round trip per frame, and this file's frames are almost free

Forward sweep, 1.0s, full track, **three repeats of each**, `TRACE_ASYNC_SCRUB` as the
only difference:

| | `dec f/s` | supply | **behind max** | **p2p last/max** |
|---|---|---|---|---|
| async (shipped) | 282.4 / 284.3 / 279.1 | 115 / 114 / 111% | **76 / 72 / 82 f** | **231/236, 219/223, 244/249 ms** |
| `TRACE_ASYNC_SCRUB=0` | 342.3 / 343.4 / 345.3 | 136 / 138 / 137% | **8 / 7 / 7 f** | **12/22, 16/23, 4/22 ms** |

**3 of 3 each, no overlap.** The synchronous walk supplies 22% more frames per second
and leaves the picture **ten times closer to the pointer** — 22ms behind instead of
236ms. 236ms of lag on a fast drag is exactly what "scrubbing is poor" feels like.

### Why here and nowhere else

The decoder is not the limit. The HUD's `shuttle ms/f` — the walk loop's own timing —
reads **0.12 ms/frame** on this file, while the pipeline delivers one frame every
**3.56 ms**. **97% of the per-frame time is not decode, conversion or paint**
(`dec 0.00/0.12 | sws 0.00/0.21 | paint tot 0.05`).

The async penalty appears only where the frame is cheap, and it is monotone in frame
cost. Same gesture, same repeats:

| file | `shuttle ms/f` | async behind / p2p max | sync behind / p2p max |
|---|---|---|---|
| **ComfyUI 720p** | **0.12** | **76f / 247ms** | **8f / 22ms** |
| 1080p H.264 | 0.68 | 9f / 59ms | 9f / 45ms |
| 4K H.264 | 3.87 | 9f / 100ms | 9f / 107ms |

`flushVideoScrub`'s async branch posts **one frame per round trip** and says so in its
own comment: *"The ease and the walk budget are gone from this path rather than ported
… What they were really buying — accelerate when far behind, settle gently on arrival —
falls out of the pipeline by itself: the chain runs as fast as frames can be decoded."*

**That premise holds only while the round trip is small against a frame.** It was true
at 4K (3.87 ms/frame) and at 1080p (0.68). At 0.12 ms/frame the round trip is 30x the
frame, so "as fast as frames can be decoded" is false — and with `kScrubEase` gone the
chain has **no mechanism to close a gap faster than one frame per round trip**. Supply
exceeds demand by only 11–15%, so a 76-frame deficit needs ~2s to work off and the
gesture lasts 1s. The synchronous walk covers `ceil(gap x 0.5)` frames inside one slice
and never falls behind in the first place.

**Tenth premise-expiry, and the first where the expired premise is a comment asserting
that a removed mechanism was redundant.**

### This file is the pool's only instance of the failing combination

It is simultaneously the **frame-densest** clip (361 frames, 1.5x the next) and the
**cheapest per frame** (0.12 ms, 6x cheaper than 1080p, 32x cheaper than 4K). Frame
density sets the demand rate; frame cost sets whether the round trip is amortised. Every
other file in the set fails one half of that.

**This is a format class, not one file.** A 720p/1080p AI export of 10–20 seconds at
24fps is the ordinary ComfyUI output shape, and it lands squarely in it.

### Not the transport, and not the window

Measured through the shipping floating transport (track **404 px** exactly, panel bbox
458x82): `dec 288.7 f/s`, `supply 115%`, `behind 0/78f`, `p2p 220/226ms` — the same as
the docked bar. Also worth recording: **this is the first file in the pool small enough
for §4's natural-size branch to bind** — the viewer opens at `1280x720 1:1`,
`win 1280x760`, where every other file is capped and reduced.

---

## 5. `TRACE_ASYNC_SCRUB=0` IS NOT THE FIX — the control says so plainly

4K ProRes 4444, same gesture, the file the worker was built for:

| | `ui gap avg/max` | over 16ms | behind max | p2p | sampling |
|---|---|---|---|---|---|
| async (shipped) | **1.56 / 23.5 ms** | **1 of 885** | 18f | 21/86 ms | ON, stride 3, 196 skipped |
| `TRACE_ASYNC_SCRUB=0` | 16.59 / 27.0 ms | **69 of 84** | **196f** | **1111/1111 ms** | idle |

The synchronous walk pins the UI thread on heavy media and ends the drag **1.1 seconds**
behind the pointer. (It also loses §15's sampling, which is a confound in the same
direction.) **Flipping the default globally would trade a 236ms problem on light media
for a 1111ms problem on heavy media.**

---

## 6. What this leaves as the choice — an owner/architecture decision, not taken here

The mechanism is identified and bounded: **the async chain needs the catch-up its
comment says it does not need**, and it needs it only when the round trip is large
relative to a frame. Candidate shapes, in the order they look cheap:

1. **Amortise the chain: let one worker request cover more than one frame when the
   pointer is far ahead** — post `walkFrom + dir * n` with the intermediate frames
   decoded and delivered as a batch, `n` derived from `gap` the way `kScrubEase` did.
   This is porting the ease onto the async path rather than inventing anything, and it
   changes *when* frames are delivered, never which ones or in what order. It is also
   the change most likely to collide with the checkpoint/cancellation contract, which
   is currently one-frame-granular.
2. **Do nothing about the pipeline and let the round trip be the round trip**, on the
   grounds that 236ms of lag that converges is within the standing motion-over-fidelity
   rule. I do not think this survives contact with the owner, but it is the honest
   null option and it costs nothing.

**Two things the brief warned against, and both warnings hold.** §15's sampling gate is
correctly refusing here (H.264 is not intra-only) and must not be loosened — the deficit
is not per-frame cost, so striding would buy little and reopen a question that took four
wrong inferences to settle. And more cache bytes would do nothing: `rev-hit` is already
98–99% and the cache already holds 80% of the clip.

## 7. The one question only the owner can answer

**Does another player scrub this file well?** The same contract question the 8K report
needed. Everything above says the picture *trails* the pointer by ~236ms and then
converges — it never shows a wrong frame and never skips one. A player that feels
instant here may be sampling the drag, which Trace deliberately does not do on long-GOP
media. Worth establishing before choosing between options 1 and 2.
