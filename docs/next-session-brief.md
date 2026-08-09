# Next session brief — step 5.6 (scrub/play-state), then GATE B

Written 2026-08-09 after owner validation of steps 5 and 5.5.

## Status

**Steps 5 and 5.5 are signed off** by the owner against the full test set. Record the
sign-off in `§15.5 item 3` of `docs/gpu-initiative-plan.md`, which was explicitly left
open pending exactly this — the harness said the mechanism worked; only the owner could
say whether a sampled preview *reads* as shuttling rather than strobing. It reads as
shuttling. Sampling on intra-only media stays.

Two remaining defects were assessed and **deliberately deferred**, not overlooked:

- occasional small hitch on 4K H.264 under extreme-speed scrub
- ProRes 4444 pauses briefly then catches up under extreme back-and-forth

Owner instruction: *do not continue chasing the rare extreme-scrub hitch unless the next
architecture work makes it worse.* Treat these as regression tripwires for GATE B, not as
work items. Both are the known stall profile (a cache miss forcing a seek plus a GOP walk),
and `docs/gpu-initiative-plan.md §15.3` already records that directional prefetch was
measured and declined.

## Step 5.6 — preserve pre-scrub play/pause state

The one correctness gap the owner found. Contained, and it must land before GATE B.

### The bug

`MainWindow.cpp`, the `sliderPressed` and `valueChanged` lambdas (~lines 619 and 659) both
call `playback_.pause()` / `playTimer_.stop()` / `stopAudio()` unconditionally, and nothing
in `sliderReleased` restores. So scrubbing does not *interrupt* playback, it *ends* it.

### Do NOT fix this by capturing `playback_` state in `sliderPressed`

There is an ordering trap. With `SH_Slider_AbsoluteSetButtons` in force (`9a214f2`), a
groove click sets the value **before** `setSliderDown(true)` emits `sliderPressed` — so by
the time `sliderPressed` runs, the `valueChanged` lambda has already paused, and a capture
there would read "was paused" for a click that began during playback. Gating the capture on
`isSliderDown()` fails for the same reason. Verify the emission order against the Qt 6.10.2
source before relying on either, but prefer the design below, which makes the question moot.

### Do this instead: separate intent from mechanism

Add a single member — call it `userPlayIntent_` — meaning *the user has asked for playback
and has not asked for it to stop*. It is distinct from `playTimer_.isActive()`, which is
whether the mechanism is currently running.

- Set `true` in `togglePlayPause` when starting; `false` when the user pauses.
- Set `false` on: open/close media, reaching end of file, entering J-K-L off-speed or
  reverse, and explicit stepping.
- **Scrubbing never writes it.** Scrub suspends the mechanism only.
- `sliderReleased` restores playback iff `userPlayIntent_` is true.

This is the same shape as `reclaimDecoder()` (`f77d472`) — one property enforced at one
choke point rather than a convention observed at a dozen call sites — and it answers the
owner's fourth question by construction: a Play or Pause pressed *while the release frame
is still resolving* just flips the intent, and the restore reads the intent, so the latest
command always wins. There is no race to lose.

### Resume ordering in `sliderReleased` — this sequence matters

Resume only **after** the exact landing has completed. `startAudioForPlayback()` takes its
offset from `playback_.state().currentFrame` (line ~1240), so resuming before the landing
starts audio at the preview position.

1. `flushVideoScrub(true)` — exact frame landed, full-res, decoder lease reclaimed.
   Playback is synchronous on the UI thread, so the lease **must** be back before step 5.
2. `supersedeInFlightRequests()` — bumps `requestGeneration_`, so any in-flight worker
   result is dropped rather than painted after the landing. This is the mechanism that
   answers "no older scrub-preview frame flashes afterward"; it already exists, just call it.
3. Return early if `!userPlayIntent_`.
4. Return early if `playbackAtEnd_` — releasing on the last frame must not silently
   restart the file. Leave it landed; the Play button already handles the rewind (`c3335ec`).
5. `prepareVideoRequest(RequestMode::Playback, +1, false)`, then `startAudioForPlayback()`,
   then `playbackClock_.start()`, `playbackAccumulatorMs_ = 0.0`, then `playTimer_.start()`.

**Factor out `startPlaybackRun()` first.** `togglePlayPause` currently inlines ~40 lines
resetting the cadence/telemetry counters (`playbackRateClock_`, `firstPresentNs_`,
`schedulerTicks_`, `audioRepeatedFrames_`, `lastClockUpdateMark_`, …). Resume needs all of
it. Duplicating it will silently rot; extract it and call it from both paths. If resume
skips it, the HUD misreports the resumed run and step 6's cadence measurements start from
poisoned counters.

### Regression tests before moving on

Owner's four confirmations, plus what the codebase's own history says to check:

1. Playing → drag → release → **playback continues from the released frame**, and the first
   presented frame equals the landing frame (HUD `target`/`shown`/`delta 0`).
2. Paused → drag → release → **stays paused**.
3. Paused → drag → press Play mid-release → plays. Playing → drag → press Pause mid-release
   → stays paused. (Run this on 4K H.264, where the press landing is 90–125ms and the window
   is actually wide enough to hit.)
4. Audio restarts at the released frame, not the pre-scrub one — check HUD `sync` and that
   `clk × fps` matches the frame index.
5. No preview frame after the landing: HUD must read full-res on the resumed first frame, not
   `previewRes`. `drop` should be non-zero on a snap release with 4444 — see below.
6. `scripts/measure/lifecycle.ps1` in full. It exists for exactly these transitions.
   `scrub.ps1 -SnapRelease` is the only gesture that reliably catches a decode in flight and
   therefore the only one that exercises cancellation at all.
7. Release on the last frame with intent true → stays on the last frame, does not restart.
8. Playing → drag on an **image sequence** and on a **still** → the non-video branch of
   `sliderReleased` needs the same restore; it is a separate code path.

## Step 6 — GATE B, the first native D3D11 surface

After 5.6 lands, go. Scope is unchanged from `docs/gpu-initiative-plan.md §8 item 6`:
frame, stride, aspect, resize, fallback. The `VideoRenderer` boundary (`5765c19`) and
`TRACE_RENDERER` selector already exist; `CpuImageRenderer` stays the default and the
D3D11 backend is opt-in until GATE E.

Carry the two deferred scrub defects in as tripwires: re-run the 4K H.264 and 4444 extreme
gestures after the surface lands and confirm neither got worse.

## Frame-rate lock and display sync — answer to the owner's question

**It is not step 6, and it should not be. It is GATE E (plan §8 item 11, "DXGI presentation
timing"), and it is the last gate before any default change.** The reason is ordering, not
priority: you cannot measure late-present or cadence jitter until there is a native
swapchain to measure them on, and step 6 is the commit that creates one. Attacking cadence
before the surface exists means another scheduler experiment, and the plan already records
three of those as measured and reverted.

Two things are worth doing early, though, and one of them is a real defect today:

**Rational frame rates should be fixed now, in step 5.6 or immediately after — it is
independent of the GPU work.** `VideoDecoderFFmpeg.cpp:821` does `metadata_.fps =
av_q2d(fr)`, which discards the `AVRational` on the spot. 24000/1001 becomes a double, and
every downstream consumer — the tick interval, the timecode HUD, seek target arithmetic —
works from the approximation. Keep the rational alongside the double. It is a small change,
it is a prerequisite for honest cadence measurement, and it is a correctness issue in a tool
whose whole pitch is that timing is exact.

**The composition rule is already written and must not be renegotiated during step 6.**
`docs/gpu-initiative-plan.md §9` states it: **audio stays the rate and position authority;
vsync becomes the phase authority.** Vsync picks the instant, the audio clock picks the frame
for that instant. One owner per question. Unifying those two under the audio clock is what
removed the hold/skip churn (`cd79d49`); giving vsync the "which frame" question as well
brings the two-scheduler bug straight back.

Also carry forward, so GATE E does not re-derive it: **display refresh rate was measured and
is NOT the remaining smoothness gap.** 59 / 119.98 / 240 Hz on the Odyssey G95SC gave
99.1 / 99.1 / 98.7% with `rep 4/4/5, skip 0/0/1` — and the spread between two runs at a
*single* rate equals the spread across all three. The owner's own verdict was "about the
same" at 120 and "hard to tell" at 240. So the ~60/120/240 Hz testing he asks for is worth
doing at GATE E as a *regression* check on the new presentation path, but it is not where a
win is waiting. The honest targets are the residual 4–5 held frames per 10s (the 41ms tick
against a 41.667ms frame, which a real presentation clock does fix) and the landing-frame
scaling quality noted in §9.

## Suggested commit sequence

1. `fix(playback): preserve pre-scrub play/pause state across a drag` — the intent flag,
   `startPlaybackRun()` extraction, resume ordering.
2. `fix(core): keep the source frame rate as a rational` — `AVRational` retained alongside
   the double.
3. `docs: record owner sign-off of steps 5 and 5.5` — plan §15.5 item 3, and the two
   deferred scrub edge cases as tripwires.
4. `feat(gpu): add experimental native D3D11 video surface` — **GATE B**.

Anj cannot push from the sandbox (proxy blocks github.com). Commit locally, then have him
run `cd ~/Claude/Trace && git push origin main`.
