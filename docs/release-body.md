## Trace v0.3.0-beta.4

**The long-GOP scrub fix.** The application is `v0.3.0-beta.3` plus one engine change: on
long-GOP H.264 files, a fast backward drag now samples keyframes instead of falling seconds
behind the hand. No interface changes.

Windows, portable ZIP, x64. Unzip anywhere and run `Trace.exe`. There is no installer by design.

### What was wrong

A full sweep of the test pool — every H.264 MP4 and every ProRes, three display passes, 264
scripted drag legs — isolated the last reported scrub fault to one class: **long-GOP H.264
under a drag whose speed exceeds what the decoder can supply.** Trace never skips a frame
during a drag, so on those files the picture walked every intermediate frame and fell behind
by the whole deficit — the worst file in the pool ended a hard reversal drag 216 frames
(1.7 seconds) behind the pointer, on every display, while every other subsystem measured
healthy. ProRes never showed it because ProRes drags already sample adaptively when the hand
outruns the decoder; long-GOP H.264 was the one place that mechanism was gated off, because
naively skipping frames on long-GOP had been measured to make things far worse.

### The fix

During a fast **backward** drag on long-GOP H.264, when the hand is outrunning the decoder,
the preview now hops between keyframes instead of walking every frame — that is what makes the
hops cheap, since a seek lands on a keyframe for free where a mid-GOP frame costs a decode
walk. The hops shrink as the hand slows, converging back onto exact consecutive frames.

This is a visible feel change, and it is honest to describe it as a trade: during the fastest
part of a backward drag on a heavy long-GOP file you now see GOP-spaced frames tracking your
hand, where before you saw every frame arriving seconds late. The moment the hand slows, and
always at release, you are back on exact frames — **the frame you release on is still landed
exactly, at full resolution: `delta 0` on all 264 legs of the sweep**, before and after the
change. Forward drags, stepping, playback and ProRes behaviour are unchanged.

Measured on the three affected files (hard reversal drag, release-to-exact-picture): the worst
went from ending 216 frames behind to ending at the pointer, 1668ms → 85ms; the reported file
241ms → 57ms; the mildest 210ms → 152ms. The other nineteen files in the pool are flat, and
the full regression (cadence, exact release, lifecycle, all 25 transport transitions) is flat.

### The side-by-side, and the rollback

If you want to feel the difference — or if anything about backward scrubbing reads *worse* —
this restores the previous walk-every-frame behaviour:

```
set TRACE_SCRUB_GOP_SAMPLE=0
Trace.exe
```

Good files to judge it on: a long clip with short GOPs (the WeLo 1x1 social cut) and a tall
9:16 online (the Universe file) — fast backward sweeps and quick back-and-forth.

### Also in this build

The sweep that found the class is now a standing regression with a per-file pass bar
(`scripts/measure/scrubbar.ps1`), so "MP4s scrub well" is a checkable verdict rather than a
feeling. Dev-side only; nothing in the application changed for it.

### Everything else

Unchanged from `v0.3.0-beta.3`. Known gaps are the same: 8K ProRes 4444 XQ does not reach real
time and is understood rather than solved; EXR does not open; HDR / BT.2020 has no tonemap;
there is no 10-bit output path; mixed-monitor DPI is validated at 100% and 150% only. If
anything about the picture looks wrong, `TRACE_RENDERER=cpu` is the escape hatch.
