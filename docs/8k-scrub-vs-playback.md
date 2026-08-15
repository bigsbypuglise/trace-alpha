# Why scrubbing the 8K plate feels better than playing it

Measured 2026-08-14, physical panel 5120x1440 @ 239.999Hz, `d3d11`, vcpkg build,
`win 1091x1083`. File: `12_8K_ProRes4444\Foces_8K_Lut_Dino Stomp_plate_4444XQ.mov`
— 7680x4320 ProRes 4444 XQ, `yuva444p12le`, 23.976 fps, 145 frames, 5739 Mbps.

The owner raised it: *"scrubbing the 8K plate feels better than real-time
playback, which I find strange."* It is not strange, it is measurable, and the
measurement is the clearest available statement of what the decode pipeline has
to buy back. **Three instruments, one file, one session.**

---

## The three measurements

**Playback** (`TRACE_RT_DROP=0`, so nothing is dropped and the serial cost is
visible):

```
dec 52.56/45.70 | sws 17.63/16.94 | upload 12.35/11.97 | total 82.55 | budget 41.71ms
handler 80.77/75.26 | outside 3.60/2.17
presented 12.86 / 23.98 fps (53.6% real time) | frames 144 | drop 0 | media 53.6%
handler>budget 143 of 143 (max 111.4) | walk 0f | seek n=1
display 1091x614 filtered x4 | dst YUV444P12 planar
```

**A slow drag**, button held, captured mid-gesture (`previewshot.ps1 -At 0.7`):

```
dst RGB32/BGRA 1090x614 | display 1090x614 1:1
dec 47.96/48.13 | sws 4.54/8.66 | upload 0.19/4.39 | total 52.72
ptr 13.8 f/s | dec 13.0 f/s | supply 94% | behind 0/1f | p2p 58/104ms
sample idle | stride 1 | skipped 0 over 0 steps
walk 0f | ra-walk 0.00f/seek | seeks 1
```

**A fast drag**, a 0.4s sweep of the whole clip, captured after release:

```
sample ON | stride 3 | skipped 92 over 7 steps
ptr 163.9 f/s | dec 13.6 f/s | supply 8% | behind 0/90f | p2p 298/298ms
landing: target 144 | shown 144 | delta 0 | dst YUV444P12 planar | release 65.4ms
```

---

## The answer, and it is not quite the one that was predicted

The handoff proposed four reasons. **Two hold, two need correcting, and the one
that actually explains the feeling was not on the list.**

### 1. The preview converts to the DISPLAY SIZE. Confirmed, and it is the larger half of the saving.

`dst RGB32/BGRA 1090x614` against playback's full-resolution `YUV444P12 planar`.
The two non-decode stages collapse:

| | playback | drag preview |
|---|---|---|
| `sws` (last/avg) | 17.63/16.94 | **4.54/8.66** |
| `upload` (last/avg) | 12.35/11.97 | **0.19/4.39** |
| `total` per frame | **82.55ms** | **52.72ms** |

On a 7680x4320 source in a 1090x614 viewer that is a 44:1 area reduction, and it
is why the *expensive half* of the frame nearly vanishes during a drag.

### 2. Intra-only, so a seek lands on the target. TRUE, but it is NOT a difference versus playback on this file.

`walk 0f` and `ra-walk 0.00f/seek` on the drag — and **playback reads `walk 0f`
too**. Every frame is a keyframe, so neither path ever walks. This is what makes
a drag on *this* file cheap compared with a drag on a long-GOP file (the
single-GOP Seedance clip walks 96 frames per miss); it is not what makes
scrubbing better than playback *here*. Quoting it as part of this gap would be
attributing a real mechanism to the wrong comparison.

### 3. Sampling is GATED ON but only ENGAGES when the drag is fast. Half true as stated.

The handoff said sampling "is active on intra-only media, so a fast drag
legitimately skips source frames". The gate is open, but the controller is
demand-driven and does nothing until demand exceeds supply:

- slow drag: **`sample idle | stride 1 | skipped 0 over 0 steps`**
- fast drag: **`sample ON | stride 3 | skipped 92 over 7 steps`**

So it explains a *fast* sweep and contributes nothing to the ordinary drag the
owner is most likely describing. The ordinary drag was already keeping up.

### 4. THE DECISIVE ONE, AND IT WAS NOT ON THE LIST: a drag has no deadline and playback does.

**The decoder supplies about thirteen frames a second whatever the gesture is.**

| | frames per second |
|---|---|
| playback, presented | **12.86** |
| slow drag, `dec` | **13.0** |
| fast drag, `dec` | **13.6** |

It is the same rate. What differs is *what that rate is judged against*.

Playback is judged against 23.976 fps and misses it on **every single frame** —
`handler>budget 143 of 143`, 53.6% of real time, and the picture visibly runs at
half speed. A drag is judged against the pointer, and the pointer is the user's
own hand:

- at **13.8 f/s** demanded it is **met** — `supply 94%`, `behind 0/1f`,
  `p2p 58ms`. Nothing is skipped, nothing trails, and it feels correct because
  it *is* correct;
- at **163.9 f/s** demanded it is **allowed to trail and to sample** —
  `supply 8%`, `behind 0/90f`, `stride 3` — and the standing
  motion-over-fidelity rule says that is the right answer, with the landing
  still exact (`target 144 shown 144 delta 0`, full-res planar).

**So the owner's perception is exactly right and the cause is not a faster path.
The pipeline delivers ~13 fps in both cases. Scrubbing feels better because
nothing promised it 24.**

---

## What the pipeline has to buy back

**13 fps → 24 fps at full resolution, every frame, in order.** That is the whole
of the 8K acceptance, and this measurement bounds the parts:

- **Conversion and upload together are 28.91ms of the 74.61ms average frame.**
  Overlapping them perfectly — which is what the second pipeline stage is for —
  leaves `max(dec, sws+upload)` = **max(45.70, 28.91) = 45.70ms = 21.9 fps** on
  the vcpkg build. Still short of 24.
- **Decode alone is the floor, and on this build it is above budget by itself.**
  `dec 45.70ms` against a `41.71ms` period. No amount of scheduling reaches 24
  fps while decode alone costs more than a frame period — which is precisely why
  checkpoint 1 (the minimal GCC FFmpeg build, `dec 38.58ms`) is a prerequisite
  rather than a nicety, and why the design doc's two-stage arithmetic is
  `max(38.6, 30.1) = 25.9 fps` rather than anything the vcpkg build can reach.
- **The drag proves the non-decode stages are compressible and the decode is
  not.** The preview cuts `sws + upload` from 28.91 to 13.05ms by converting
  1/44th the pixels, and `dec` does not move at all — 45.70 in playback against
  48.13 on the drag. Decode is the term that has to come down through the
  toolchain, and conversion+upload is the term that has to come off the critical
  path through the pipeline. They are separate levers and this file needs both,
  exactly as the owner's decision-2 ruling assumed.

## What this does NOT say

It is not evidence that scrubbing this file is *good* — it is evidence that
scrubbing is being asked for less. A drag that demanded 24 f/s would measure the
same deficit playback does. And it says nothing about the Seedance clip, whose
scrub problem was a 96-frame GOP walk on the UI thread and is fixed by a
different change entirely (`cc8e638`).
