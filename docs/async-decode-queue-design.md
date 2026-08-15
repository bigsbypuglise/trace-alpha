# Checkpoint 2 — the bounded async sequential decode queue: design

Written 2026-08-14, before implementation, on instruction. **The headline is that the
arithmetic this checkpoint was scoped from is wrong about where conversion runs, and a
single-stage queue therefore cannot reach the 8K acceptance target.** Everything else in the
design survives that correction; the depth, the ownership and the draining are unchanged.

---

## 1. The premise expired, and it expired in the code rather than in a note

The handoff's arithmetic:

> decode (including its demux read) **39.68 ms** · conversion + upload **30.3 ms** · serial
> today 70 ms = 14.3 fps · **overlapped `max(39.7, 30.3)` = 39.7 ms = 25.2 fps**

That splits the frame into "decode" on one side and "conversion + upload" on the other. **In
Trace they are not on those sides.** `convertCurrentFrame` is called from inside
`decodeFrameAt` (`VideoDecoderFFmpeg.cpp:2185` and `:2200`), and the scrub worker's only
decoder call is `decoder_->decodeFrameAt(...)` (`ScrubDecodeWorker.cpp:217`). **Conversion runs
wherever the decode runs.** It is not a deduction: during a drag, when the worker holds the
lease, the HUD's `sws` figure comes from `ScrubResult::perf`, which the worker fills from
`decoder_->perfStats()` on its own thread.

So moving decode to a worker moves conversion with it, and what is left on the UI thread is
upload and paint.

### Measured, this session, on the 8K plate

`12_8K_ProRes4444\Foces_8K_Lut_Dino Stomp_plate_4444XQ.mov`, shipping vcpkg
`build\app\Release`, `TRACE_NO_AUDIO=1`, **`TRACE_RT_DROP=0`** as the owner requires,
physical panel, `display 1091x614 filtered x4`, `win 1484x1083`:

```
dec 45.35/49.33 | sws 16.73/17.02 | upload 11.91/12.10 tex 3 | paint 0.01/0.09 tot 1.14
handler 75.03/78.25 | outside 0.26/2.08 | budget 41.71ms | handler>budget 88 of 88 (max 87.1)
presented 12.38 / 23.98 fps (51.6% real time) | frames 88 | elapsed 7.11s | drop 0 | thr slice
```

`49.33 + 17.02 + 12.10 = 78.45` against a handler of `78.25`. Strictly serial, `outside 2.08 ms`,
**nothing is waiting on anything.**

### What one stage actually buys

| | worker stage (demux + decode + convert) | UI stage (upload + paint) | overlapped | fps |
|---|---|---|---|---|
| vcpkg, serial today | — | — | 78.5 ms | **12.7** |
| vcpkg, one queue stage | 49.3 + 17.0 = **66.3** | 12.1 + 1.1 = **13.2** | 66.3 ms | **15.1** |
| minimal FFmpeg, serial today | — | — | 69.5 ms | **14.4** |
| minimal FFmpeg, one queue stage | 39.7 + 17.0 = **56.7** | **13.2** | 56.7 ms | **17.6** |

**+22%, and 17.6 fps against an acceptance of 24.** Building the queue against the 25.2 figure
would produce a working implementation that measures as a failure.

### The 25.2 figure is right — for a design nobody has described yet

`max(decode, convert + upload)` = `max(39.7, 29.1)` = 39.7 ms = **25.2 fps** is exactly
reachable, but only if conversion is in the **second** stage rather than riding with the decode.
That is a **two-stage pipeline** — decode frame N+2 while converting N+1 while uploading N — and
it is a different piece of work from the one this checkpoint names:

- there is one `VideoDecoderFFmpeg` and one owner of it at any instant (plan §14), and its four
  sws slots are decoder state, so "convert on another thread" cannot simply be another caller of
  the same object;
- conversion would have to operate on a decoded `AVFrame` handed out of the decoder, which means
  the decoder's output boundary changes from `VideoFrame` (converted) to something earlier — and
  `VideoFrame.h` deliberately admits no FFmpeg type because it is reached from the
  image-sequence path, which must compile with `TRACE_WITH_FFMPEG` undefined;
- frame-threaded/slice-threaded decoding already uses the box's cores, so a second stage
  competes with the first for them. The 39.7 and 17.0 figures were both measured with the whole
  machine available; run concurrently they will each be slower, and the margin is **~2 ms a
  frame** before that is counted. The handoff already flagged the margin as thin. It is thinner.

**This is an owner decision and it is why nothing is built yet.** One stage is a real
improvement, is the prerequisite for two, and does not reach the 8K bar. Two stages might, and
is a materially larger change to the decoder's output boundary.

---

## 2. The design, unchanged by the above

### 2.1 It is a bounded lookahead cache, never a schedule

The rule the handoff is right about, and it is the one most likely to sink the work. Under the
audio master clock the **audio clock** picks the frame — it may hold the current one, or advance
up to three. Under GATE E the deadline scheduler picks the slot. A queue that runs ahead is
implicitly asserting the next frame is `current + 1`, which is true of the 8K plate (no audio
track) and false in general. `cd79d49` is the record of two mechanisms each owning half of
"which frame, when".

So the tick's target arithmetic is **untouched**. After it computes `targetFrame` exactly as it
does today:

- entries with `frameIndex < targetFrame` are **discarded** (an audio catch-up of 3 discards 2)
  and counted;
- if the head is `targetFrame`, it is popped and presented — no decode on the UI thread;
- if the head is **ahead** of the target, nothing is consumed. A hold already returns before
  reaching here (`delta <= 0`), so this is a defensive branch, not a path;
- if the queue cannot answer, the tick **holds and counts a starve**. It does *not* take the
  decoder back — reclaiming costs a `revokeLease()` wait of up to one frame decode and would
  turn the pipeline into a synchronous walk with a stall in front of it. This is the decision
  the reverse shuttle already took, in the same words: *"a starve is a cadence event worth
  counting, not an excuse to take the work back."*

`playback_.setCurrentFrame()` moves only when a frame is actually presented, and the identity
comes off `VideoFrame::frameIndex`, never off the arithmetic that asked for it — the
`presentQueuedShuttleFrame()` rule, for the `e76eabb` reason.

### 2.2 Where the existing ownership machinery is used — required, and specific

- **`requestGeneration_`** is stamped on every posted `ScrubRequest` in `pumpPlaybackQueue()`,
  the same field `pumpShuttleQueue()` sets. It is compared at the delivery boundary in
  `onScrubResult()`, which already drops any result whose generation is not
  `scrubWorker_.latestGeneration()`.
- **`supersedeInFlightRequests()`** is *not* called from the playback path directly, for the
  same reason the drag does not call it per pointer move: it is reached through
  `reclaimDecoder()`, which bumps the generation **before** waiting for the worker to park, so
  anything published during the wait is already stale by construction.
- **The lease** (plan §14) is granted by `grantDecoderLease()` before the first post and comes
  back only through `reclaimDecoder()`. `loadCurrentFrame()` calls `reclaimDecoder()` on entry,
  so **every** synchronous decode in the application — step, press landing, release landing,
  shuttle landing, the Go To prompts — drains the prefetch automatically.

Attempt three is allowed to proceed because that machinery exists and is validated, not because
the idea improved. The first two attempts (`a171e3a`/`1d280eb`, reverted `9cd2a0c`/`a2f7999`)
had none of it.

### 2.3 Draining is one choke point, not an enumeration

**`playbackQueue_.clear()` goes inside `reclaimDecoder()`**, beside the generation bump — not at
each of the transitions the requirements list. Pause, stop, seek, scrub, step, shuttle start,
file change, end-of-media and shutdown all already funnel through it, so "deterministic
cancellation and draining" becomes a property of one function rather than a list that has to
stay complete. Clearing an empty queue is free, which is what makes it safe to put there.

This is §29.2's lesson applied in advance: GATE E was validated on the Play action alone and
every path that started the timer without establishing the timeline kept compiling silently.

`endShuttleRun()` calls `reclaimDecoder()`, so the shuttle and the prefetch cannot both hold
the worker. `startPlaybackPrefetch()` refuses while `shuttleRunActive_ || scrubbing_ ||
storageBusy_`, mirroring `startShuttleRun()`'s guards — including that it must test
**`scrubbing_`, not `isVideoScrubActive()`**, which means "the media is a video file".

### 2.4 Engagement is narrow and is a predicate that already exists

Ordinary 1× forward playback only: `ordinaryForwardPlay` — forward at exactly 1× — is already
the predicate that decides which case keeps the play intent, which gets sound, and which does
*not* become a shuttle run (spec phase 3). Reverse and every shuttle rung are already queued by
`startShuttleRun`; below 1× every frame is presented by owner decision and there is nothing to
run ahead of.

### 2.5 Depth: shallow, and byte-bounded as well as count-bounded

A full-resolution 8K 12-bit 4:4:4 frame is ~199 MB, so a depth-3 queue is ~600 MB **on top of**
the 384 MB reverse cache (`TRACE_REVERSE_CACHE_MB`, verified bounded at §26.5) and a working set
already ~900 MB at 4K. The two budgets do not know about each other and this design does not
make them: they are **summed and reported**, because the reverse cache is bounded and verified
independently and coupling them would put a second policy inside a settled one.

Depth is therefore `min(TRACE_PLAYBACK_QUEUE, budget / entryBytes)` with the entry size learned
from a real frame the way `fullResEntryBytes` already is — so 8K gets 2 and 4K H.264 gets the
count. **The number is justified by the measured starvation count, not chosen**, which means
the first measurement pass sweeps depth and reports `starve` at each.

### 2.6 Instrumentation — the owner asked for the terms separately, and for good reason

New HUD field beside the existing `shuttle` line:

```
pq N/D (max M) | starve S | ahead-drop A | wait W.Wms | posted P | MB cur/peak
```

- **queue depth**, its **high-water mark**, **starvation count**, entries discarded by a
  catch-up, **queue wait** (time the tick spent blocked on the queue — must read 0, and a
  non-zero value means the fallback is being taken), posted requests, current and peak bytes.
- demux/read, decode and conversion are already separate (`io`, `dec`, `sws`) and are published
  from the worker's own snapshot; upload and paint are already separate on the UI side.
- **Do not infer overlap from the frame rate.** The check that overlap is real is
  `handler` collapsing to upload+paint while `dec`+`sws` stay where they are, with `outside`
  rising to absorb the difference. If `handler` falls and `dec` falls with it, the run decoded
  fewer frames rather than overlapping them.

### 2.7 Rollback and comparison

`TRACE_PLAYBACK_QUEUE=N`, **default 0 = off**, so the first commit changes nothing until
measured. The synchronous path is not a second implementation kept in sync — it is the path that
runs when the queue cannot answer, so it stays exercised.

---

## 3. What this does not touch

The validated scrub worker's drag behaviour, the decoder lease's semantics, exact scrub release,
exact stepping, reverse playback, the shuttle, and frame ordering. The queue adds a second
*caller* of `ScrubDecodeWorker`, which already serves two (the drag chain and the shuttle) and
whose depth-1 latest-wins contract is unchanged.

---

## 4. Recommended sequence

1. **Take the exact landing off the UI thread** (roadmap item 2b) — it is the same ownership
   machinery, it is the fix for the 4K HEVC report measured this session
   (`docs/comfyui-4k-hevc-scrub-measurement.md`: 261–585 ms of frozen window per click), and it
   is smaller than either queue.
2. **Single-stage queue** as designed above, default off, measured and reported. +22% on the 8K
   plate, real headroom on everything else, and the prerequisite for step 3.
3. **Two-stage pipeline** — only if the owner wants the 8K acceptance target pursued, because it
   changes the decoder's output boundary and its margin is ~2 ms a frame before contention.

**Nothing in 1–3 is started.** The correction in §1 is the reason.
